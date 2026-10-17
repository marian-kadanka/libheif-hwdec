#!/bin/bash
# Regenerates the HEIC test files in tests/data/ with ImageMagick (convert) and heif-enc (libheif
# with its x265 encoder). The files are committed, so running the tests needs neither tool; run
# this only to change the set.
#
# Usage: tests/make-fixtures.sh
#
# Needs ImageMagick, heif-enc, ffmpeg, x265 and MP4Box (GPAC).
#
# Each file covers one stream layout the plugin has to turn into heif planes. 161x151 is odd in
# both directions, so 4:2:0 and 4:2:2 chroma planes are rounded up and the HEVC conformance window
# crops the coded 168x152 picture; it is also above NVDEC's minimum height of 144. libheif's x265
# plugin codes 8-bit images as Main Still Picture and 10/12-bit or 4:2:2/4:4:4 images with HEVC
# Range Extensions profiles.

set -euo pipefail

data_dir="$(cd "$(dirname "$0")" && pwd)/data"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$data_dir"
cd "$work"

size=161x151
convert -size $size -seed 7 plasma:red-blue -depth 8 rgb8.png
convert -size $size -seed 7 plasma:red-blue -depth 16 rgb16.png
convert rgb8.png -colorspace Gray -depth 8 gray8.png
convert rgb16.png -colorspace Gray -depth 16 gray16.png
convert rgb8.png \( -size $size gradient:white-black \) -alpha off -compose CopyOpacity -composite rgba8.png
convert -size 1x1 xc:'#3080c0' one.png

# A 3x2 grid of 64x64 tiles, cropped to 161x93: the right column and bottom row are partial.
# The tiles are below NVDEC's minimum height, so with the cuda backend they decode in software.
convert -size 192x128 -seed 3 plasma:green-yellow -depth 8 grid.png
mkdir tiles
for y in 0 1; do
  for x in 0 1 2; do
    convert grid.png -crop 64x64+$((x * 64))+$((y * 64)) +repage tiles/tile-$y-$x.png
  done
done

enc() {
  local name=$1
  shift
  heif-enc -q 60 "$@" -o "$data_dir/$name.heic" > /dev/null
  echo "$name.heic"
}

enc yuv420-8bit rgb8.png
enc yuv420-10bit -b 10 rgb16.png
enc yuv420-12bit -b 12 rgb16.png
enc yuv422-8bit -p chroma=422 rgb8.png
enc yuv422-10bit -p chroma=422 -b 10 rgb16.png
enc yuv444-8bit -p chroma=444 rgb8.png
enc yuv444-10bit -p chroma=444 -b 10 rgb16.png
enc yuv444-12bit -p chroma=444 -b 12 rgb16.png
# Lossless is coded as 4:4:4 with the identity matrix (matrix_coefficients = 0): G in the luma
# channel, B and R in the chroma channels. FFmpeg decodes it to gbrp.
enc rgb-lossless -L rgb8.png
enc mono-8bit gray8.png
enc mono-10bit -b 10 gray16.png
# The alpha channel is a separate monochrome HEVC image that libheif decodes with the same plugin.
enc alpha-8bit rgba8.png
# BT.2020 primaries and matrix, HLG transfer, limited range: checks the colour description the
# plugin attaches to the decoded image.
enc limited-bt2020 --matrix_coefficients 9 --colour_primaries 9 --transfer_characteristic 14 --full_range_flag 0 rgb8.png
enc tiny-1x1 one.png
enc grid-cropped -T tiles/tile-0-0.png --tiled-image-width 161 --tiled-image-height 93

# Single pictures beyond what the GPUs accept, which have to fall back to software: wider than
# NVDEC's 8192, and wider than the 16384 Intel's VAAPI driver reports.
convert -size 9216x256 gradient:navy-gold -depth 8 large1.png
convert -size 16896x256 gradient:gold-navy -depth 8 large2.png
enc large-9216x256 large1.png
enc large-16896x256 large2.png

# The files above use the profiles libheif's x265 plugin picks, and FFmpeg 6.1's VAAPI code cannot
# map the Range Extensions intra profiles it uses for 10/12-bit, 4:2:2 and 4:4:4. The files below
# use the plain profiles hardware decoders list (Main 10, Main 12, Main 4:2:2 10, Main 4:4:4 ...),
# so those formats are decoded on the GPU too. x265 signals an "Intra" profile for a stream of
# intra pictures only, so it encodes two frames, and MP4Box stores the first one as the image.
# The x265 command line only accepts sizes divisible by the chroma subsampling, so these are
# 160x152.
convert -size 160x152 -seed 11 plasma:orange-purple -depth 16 profile16.png

x265_enc() {
  local name=$1 pix_fmt=$2 profile=$3 source=${4:-profile16.png}
  ffmpeg -v error -y -loop 1 -i "$source" -frames:v 2 -pix_fmt "$pix_fmt" -strict -1 \
    -f yuv4mpegpipe "$name.y4m"
  x265 --log-level error --no-progress --input "$name.y4m" --y4m --profile "$profile" --crf 24 --output "$name.hevc"
  rm -f "$data_dir/$name.heic"
  MP4Box -quiet -add-image "$name.hevc:primary" -new "$data_dir/$name.heic"
  echo "$name.heic"
}

x265_enc main10 yuv420p10le main10
x265_enc main12 yuv420p12le main12
x265_enc main422-10 yuv422p10le main422-10
x265_enc main422-12 yuv422p12le main422-12
x265_enc main444-8 yuv444p main444-8
x265_enc main444-10 yuv444p10le main444-10
x265_enc main444-12 yuv444p12le main444-12

# A picture 64 pixels wide, which NVIDIA's VDPAU decodes wrongly, so the plugin has to decode it in
# software on that backend; through VAAPI it is decoded on the GPU.
convert -size 64x152 -seed 13 plasma:teal-pink -depth 16 narrow16.png
x265_enc narrow-64x152 yuv420p main narrow16.png

# AVIF (AV1) images, encoded by libheif's aom plugin. The GPUs decode AV1 Main profile (4:2:0 and
# monochrome, 8 or 10 bits); 4:4:4 is High profile and decodes in software.
enc_avif() {
  local name=$1
  shift
  heif-enc -A -q 60 "$@" -o "$data_dir/$name.avif" > /dev/null
  echo "$name.avif"
}

convert -size 384x256 -seed 3 plasma:green-yellow -depth 8 avif-grid.png
mkdir avif-tiles
for y in 0 1; do
  for x in 0 1 2; do
    convert avif-grid.png -crop 128x128+$((x * 128))+$((y * 128)) +repage avif-tiles/tile-$y-$x.png
  done
done

enc_avif av1-420-8bit rgb8.png
enc_avif av1-420-10bit -b 10 rgb16.png
enc_avif av1-444-8bit -p chroma=444 rgb8.png
enc_avif av1-mono-8bit gray8.png
enc_avif av1-alpha-8bit rgba8.png
# A 3x2 grid of 128x128 tiles cropped to 333x251.
enc_avif av1-grid -T avif-tiles/tile-0-0.png --tiled-image-width 333 --tiled-image-height 251
