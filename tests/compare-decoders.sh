#!/bin/bash
# Decodes every file given with a reference decoder and with the hwdec plugin on one backend, and
# fails unless both produce the same size, the same hash of the decoded planes and the same colour
# profile for every file. The reference is libde265 for .heic files and dav1d, or aom if dav1d is
# not installed, for .avif files. Run by ctest; exit code 77 means skipped.
#
# Usage: compare-decoders.sh BENCH PLUGIN_DIR BACKEND FILE...
#   BENCH       the heif-hwdec-bench program
#   PLUGIN_DIR  directory holding the built libheif-hwdec.so
#   BACKEND     vaapi, vdpau, cuda or vulkan (sets LIBHEIF_HWDEC_BACKEND)
#
# Skips when a reference decoder is not installed, or when the backend has no usable device: libheif
# then quietly decodes with its regular decoder even if hwdec is requested, which would compare the
# reference with itself.

set -u
bench=$1
plugin_dir=$2
backend=$3
shift 3

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# libheif's own decoders come from its plugin directory, linked into a directory of their own
# without libheif-hwdec.so: with an installed copy of this plugin on the path as well, libheif
# would load that one, and the test would compare the installed plugin instead of the build.
system_plugins=$("$bench" --plugin-dir)
mkdir "$work/plugins"
for p in "$system_plugins"/*.so; do
  [ -e "$p" ] && [ "$(basename "$p")" != libheif-hwdec.so ] && ln -s "$p" "$work/plugins/"
done
export LIBHEIF_PLUGIN_PATH="$plugin_dir:$work/plugins"

# The plugin only claims AV1 where its probe decode succeeds on the GPU; elsewhere the AVIF files
# are left out instead of letting libheif quietly use its own AV1 decoder.
av1_note=""
if ! LIBHEIF_HWDEC_BACKEND=$backend "$bench" --has-decoder hwdec avif; then
  av1_note="AVIF files left out: the plugin does not claim AV1 on this $backend device"
fi

files=()
heic=()
avif=()
for f in "$@"; do
  case "$f" in
    *.avif) [ -z "$av1_note" ] && avif+=("$f") && files+=("$f") ;;
    *) heic+=("$f") && files+=("$f") ;;
  esac
done
set -- "${files[@]}"

av1_reference=none
if [ ${#avif[@]} -gt 0 ]; then
  av1_reference=dav1d
  "$bench" --has-decoder dav1d avif || av1_reference=aom
fi
for pair in "libde265:heic:${#heic[@]}" "$av1_reference:avif:${#avif[@]}"; do
  IFS=: read -r decoder format count <<< "$pair"
  if [ "$count" -gt 0 ] && ! "$bench" --has-decoder "$decoder" "$format"; then
    echo "SKIP: reference decoder $decoder not available"
    exit 77
  fi
done

# Reference output in the order the files were given.
: > "$work/reference.txt"
reference_status=0
for f in "$@"; do
  case "$f" in
    *.avif) decoder=$av1_reference ;;
    *) decoder=libde265 ;;
  esac
  "$bench" -d "$decoder" "$f" >> "$work/reference.txt" 2>&1 || reference_status=1
done

LIBHEIF_HWDEC_BACKEND=$backend LIBHEIF_HWDEC_DEBUG=1 "$bench" -d hwdec "$@" > "$work/hwdec.txt" 2> "$work/hwdec.log"
hwdec_status=$?

if grep -q 'no hardware device found' "$work/hwdec.log"; then
  echo "SKIP: no usable $backend device"
  exit 77
fi

if [ $reference_status -ne 0 ]; then
  echo "FAIL: a reference decoder could not decode every file:"
  cat "$work/reference.txt"
  exit 1
fi
if [ $hwdec_status -ne 0 ]; then
  echo "FAIL: hwdec run exited with status $hwdec_status:"
  cat "$work/hwdec.txt"
  tail -20 "$work/hwdec.log"
  exit 1
fi

# Bench lines: <file> <milliseconds> <width>x<height> <hash> nclx=<p>/<t>/<m>/<full range>
failures=$(paste -d' ' "$work/reference.txt" "$work/hwdec.txt" |
  awk '$1 != $6 || $3 != $8 || $4 != $9 || $5 != $10 {print "  " $1 ": libde265 " $3 " " $4 " " $5 ", hwdec " $8 " " $9 " " $10}')
expected=$#
compared=$(wc -l < "$work/hwdec.txt")
gpu=$(grep -c 'on gpu' "$work/hwdec.log")
cpu=$(grep -c 'on cpu' "$work/hwdec.log")
hevc_gpu=$(grep -c 'hevc on gpu' "$work/hwdec.log")
hevc_cpu=$(grep -c 'hevc on cpu' "$work/hwdec.log")
av1_gpu=$(grep -c 'av1 on gpu' "$work/hwdec.log")
av1_cpu=$(grep -c 'av1 on cpu' "$work/hwdec.log")

echo "$backend: $compared of $expected files compared (AV1 reference: $av1_reference)"
echo "  plugin decoded HEVC: $hevc_gpu pictures on the GPU, $hevc_cpu in software;" \
     "AV1: $av1_gpu on the GPU, $av1_cpu in software"
if [ -n "$av1_note" ]; then
  echo "  $av1_note"
fi
if [ "$compared" -ne "$expected" ] || [ -n "$failures" ]; then
  echo "FAIL: output differs from libde265:"
  echo "$failures"
  exit 1
fi
if [ "$gpu" -eq 0 ]; then
  echo "FAIL: no picture was decoded on the GPU"
  exit 1
fi
echo "PASS"
