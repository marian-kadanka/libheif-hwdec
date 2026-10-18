# libheif-hwdec

A [libheif](https://github.com/strukturag/libheif) decoder plugin that decodes HEIC (HEVC) and
AVIF (AV1) images on the GPU through FFmpeg's hardware acceleration API.

libheif loads it from its plugin directory, so every program that reads these formats through
the system's libheif picks it up without being rebuilt: `heif-dec`, and image viewers that use
KDE's Qt image format plugins (`kimageformats`), such as Gwenview.

| backend  | GPUs                                                                                 |
|----------|--------------------------------------------------------------------------------------|
| `vaapi`  | Intel and AMD (Mesa), NVIDIA with [nvidia-vaapi-driver](https://github.com/elFarto/nvidia-vaapi-driver) 0.0.14 or later (see [Configuration](#configuration)) |
| `vdpau`  | NVIDIA, under X11 or XWayland                                                        |
| `cuda`   | NVIDIA, through NVDEC                                                                |
| `vulkan` | any GPU whose Vulkan driver exposes video decoding; tried last, with FFmpeg 8.1 or later (see [Configuration](#configuration)) |

## How it works

libheif creates one decoder per coded picture: one for a plain image, one per tile for a grid
image (phones store a 12–50 megapixel photo as dozens of 512×512 or 1024×1024 tiles), one for an
alpha plane. The plugin turns the data libheif pushes into FFmpeg packets, decodes them with
FFmpeg's `hevc` or `av1` decoder bound to a GPU device, and copies each frame from GPU memory into
libheif's planes.

- **One device per process.** The GPU is opened on first use and kept until the process exits.
  The plugin is linked with `-z nodelete`, so it stays loaded when libheif unloads its plugins,
  which KDE's HEIF image plugin causes after every image it reads.
- **Contexts are reused.** Initialised FFmpeg contexts are pooled and reused for pictures with
  identical sequence parameters (HEVC VPS/SPS/PPS, AV1 sequence header), so NVDEC's per-context
  setup is paid once per worker thread instead of once per tile.
- **Software fallback.** If the GPU cannot decode a stream (unsupported profile, chroma format,
  bit depth or size, or a driver error), the plugin decodes it again with FFmpeg in software:
  `hevc` for HEIC, `libdav1d` or `libaom-av1` for AVIF.
- **Stepping aside.** The plugin declines a format, and libheif uses its regular decoders
  (libde265, dav1d, aom), when there is no usable GPU device, when this FFmpeg lacks the decoders
  the plugin needs (Fedora's `ffmpeg-free` has no `hevc`), or, for AV1, when a tiny AV1 picture
  built into the plugin does not decode on the GPU. Drivers that list AV1 decoding can still fail
  on every stream; that probe runs once per process, the first time an AVIF file is opened.

libheif picks the decoder with the highest priority. The plugin reports 110 for HEVC, above
libde265's 100, and 160 for AV1, above dav1d's 150.

## Compatibility

libheif's plugin interface has changed over time, and libheif 1.21 and later refuse plugins built
for an interface version below 5. The build detects what the libheif headers offer and builds for
that version, which also decides the libheif releases the binary works with:

| built against libheif | plugin API | loads with libheif | package dependency (`.deb` and `.rpm`) |
|-----------------------|-----------:|--------------------|----------------------------------------|
| 1.15 – 1.19           | 3          | 1.15 – 1.20        | `>= 1.15`, `< 1.21`                    |
| 1.20                  | 4          | 1.20               | `>= 1.20`, `< 1.21`                    |
| 1.21                  | 5          | 1.21 and later     | `>= 1.21`                              |
| 1.22 and later        | 6          | 1.22 and later     | `>= 1.22`                              |

Tested with libheif 1.17.6, 1.19.8, 1.20.2, 1.21.2, 1.22.2 and 1.23.6, and FFmpeg 6.1, 7.0, 7.1,
8.0, 8.1 and 9.0. The plugin links FFmpeg by its library versions (`libavcodec.so.60` for FFmpeg 6,
`.62` for 8 and so on), so a binary also belongs to one FFmpeg major version.

## Pre-built packages

### From a package repository

A repository also brings updates with the rest of the system.

Ubuntu 24.04 and 26.04, from the [PPA](https://launchpad.net/~marian.kadanka/+archive/ubuntu/libheif-hwdec):

```sh
sudo add-apt-repository ppa:marian.kadanka/libheif-hwdec
sudo apt install libheif-plugin-hwdec
```

openSUSE Tumbleweed and Leap 16.0, Fedora 44 and Rawhide, from the
[openSUSE Build Service](https://build.opensuse.org/package/show/home:marian.kadanka/libheif-hwdec).
For openSUSE Leap 16.0 use `16.0` in place of `openSUSE_Tumbleweed`; for Fedora Rawhide use
`Fedora_Rawhide` in place of `Fedora_44`:

```sh
# openSUSE
sudo zypper addrepo https://download.opensuse.org/repositories/home:marian.kadanka/openSUSE_Tumbleweed/home:marian.kadanka.repo
sudo zypper refresh
sudo zypper install libheif-hwdec

# Fedora
sudo dnf config-manager addrepo --from-repofile=https://download.opensuse.org/repositories/home:marian.kadanka/Fedora_44/home:marian.kadanka.repo
sudo dnf install libheif-hwdec
```

Restart any image viewer that was already open. See [Distribution notes](#distribution-notes) for
the FFmpeg and driver packages each distribution needs before the GPU is used.

### From a GitHub release

Every [release](https://github.com/marian-kadanka/libheif-hwdec/releases) has a package for each of
these distributions, built by `.github/workflows/packages.yml` against that distribution's libheif
and FFmpeg:

| distribution        | file                                                   | install with |
|---------------------|--------------------------------------------------------|--------------|
| Ubuntu 24.04        | `libheif-plugin-hwdec_<version>-1+ubuntu24.04_amd64.deb` | `sudo apt install ./<file>` |
| Ubuntu 26.04        | `libheif-plugin-hwdec_<version>-1+ubuntu26.04_amd64.deb` | `sudo apt install ./<file>` |
| Debian 13           | `libheif-plugin-hwdec_<version>-1+debian13_amd64.deb`    | `sudo apt install ./<file>` |
| Debian testing      | `libheif-plugin-hwdec_<version>-1+debiantesting_amd64.deb` | `sudo apt install ./<file>` |
| Fedora 44           | `libheif-hwdec-<version>-1.fc44.x86_64.rpm`              | `sudo dnf install ./<file>` |
| openSUSE Tumbleweed | `libheif-hwdec-<version>-1.suse.tumbleweed.x86_64.rpm`   | `sudo zypper install ./<file>` |
| Arch                | `libheif-hwdec-<version>-1-x86_64.pkg.tar.zst`           | `sudo pacman -U <file>` |

A package only works with the libheif and FFmpeg major versions of the release it was built for.
See [Distribution notes](#distribution-notes) for the driver and FFmpeg packages each distribution
needs before the GPU is used. `packaging/ci/build-package.sh` builds any of these packages locally,
in a container of the distribution.

## Requirements

- libheif ≥ 1.15, built with plugin loading
- FFmpeg (`libavcodec`, `libavutil`) with the `hevc` decoder, and for AVIF with the `av1` decoder
  and `libdav1d` or `libaom-av1`
- libva for the `vaapi` backend, the Vulkan headers to build the `vulkan` backend's device
  creation
- A GPU driver that decodes HEVC, see [Distribution notes](#distribution-notes)

## Building

```sh
sudo apt install cmake pkgconf libheif-dev libavcodec-dev libavutil-dev libva-dev libvulkan-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

CMake prints the plugin API version it chose and the directory the plugin installs into, which
it asks libheif for: `/usr/lib/x86_64-linux-gnu/libheif/plugins` on Debian and Ubuntu,
`/usr/lib64/libheif` on Fedora, `/usr/libexec/libheif` on openSUSE, `/usr/lib/libheif` on Arch.
Set `HWDEC_PLUGIN_DIR` to override it.

Try it without installing. `LIBHEIF_PLUGIN_PATH` replaces libheif's built-in plugin directory, so
list both:

```sh
export LIBHEIF_PLUGIN_PATH=$PWD/build:/usr/lib/x86_64-linux-gnu/libheif/plugins
heif-dec --list-decoders        # "hwdec" should be listed first under HEIC decoders
LIBHEIF_HWDEC_DEBUG=1 heif-dec image.heic image.png
```

## Installing

Debian and Ubuntu:

```sh
sudo apt install debhelper devscripts
dpkg-buildpackage -b -us -uc
sudo apt install ../libheif-plugin-hwdec_0.1.1-1_amd64.deb
```

Fedora and openSUSE, from a release tarball:

```sh
rpmbuild -ba packaging/rpm/libheif-hwdec.spec
```

Or directly:

```sh
sudo cmake --install build
```

## Distribution notes

Distributions leave patented codecs out of their main repositories, in the GPU drivers as well as
in FFmpeg and libheif. The plugin needs an FFmpeg with the `hevc` decoder and a driver that
decodes HEVC (and AV1, for AVIF); without them it steps aside and nothing changes.

| distribution | FFmpeg with `hevc` | Intel VAAPI driver with HEVC and AV1 | libde265 for libheif |
|--------------|--------------------|--------------------------------------|----------------------|
| Debian       | main               | `intel-media-va-driver-non-free` (non-free); the main `intel-media-va-driver` has no AV1 | `libheif-plugin-libde265` |
| Ubuntu       | main               | `intel-media-va-driver-non-free` (multiverse); the main package has no AV1 | `libheif-plugin-libde265` |
| Fedora       | `ffmpeg` from RPM Fusion; Fedora's `ffmpeg-free` has none | `intel-media-driver` from RPM Fusion nonfree; Fedora's `libva-intel-media-driver` has no HEVC | `libheif-freeworld` from RPM Fusion |
| openSUSE     | main               | `intel-media-driver`                 | `libheif-HEIF` from Packman |
| Arch         | main               | `intel-media-driver`                 | built into `libheif` |

AV1 decoding on Intel GPUs also needs a recent driver: version 24.1 (Ubuntu 24.04) fails on every
AV1 stream with any FFmpeg version, 26.1 and later decode it.

## Configuration

Environment variables, read once per process:

| variable                | effect                                                                   |
|-------------------------|--------------------------------------------------------------------------|
| `LIBHEIF_HWDEC=0`       | disable the plugin; libheif uses its regular decoders                    |
| `LIBHEIF_HWDEC_BACKEND` | comma-separated backends to try in order: `vaapi`, `vdpau`, `cuda`, `vulkan`; default `vaapi,vdpau,cuda`, and `vaapi,vdpau,cuda,vulkan` with FFmpeg 8.1 or later on a machine with a GPU device node (`/dev/dri/renderD*` or `/dev/nvidia0`) |
| `LIBHEIF_HWDEC_DEVICE`  | DRM render node for `vaapi` (`/dev/dri/renderD129`), device index for `cuda` and `vulkan` |
| `LIBHEIF_HWDEC_DEBUG=1` | log, with the process ID, the chosen device, the AV1 probe, each picture's path (GPU or CPU), format and timings, and FFmpeg's errors |

Without `LIBHEIF_HWDEC_DEVICE`:

- `vaapi` probes every `/dev/dri/renderD*` node and takes the first whose driver can decode HEVC
  Main. NVIDIA nodes are probed last, so on a laptop with integrated graphics and an NVIDIA GPU the
  NVIDIA GPU is not woken up.
- `vaapi` skips nvidia-vaapi-driver when it runs its "egl" backend: decodes through it never
  finish, so every image would hang. That is the default backend of version 0.0.8, the one Ubuntu
  24.04 ships, and its "direct" backend fails to start. Versions 0.0.14 and later default to the
  "direct" backend, which works. Measured on an NVIDIA T1200 with 0.0.14 and 0.0.18, it
  decodes as fast as the `cuda` backend from the second image in a process, but the first image
  takes about 170 ms longer, because the driver's own setup is slower. Where both are available,
  `LIBHEIF_HWDEC_BACKEND=vdpau,cuda` is the better choice for NVIDIA GPUs.
- `vdpau` comes before `cuda` because it sets up in about 20 ms, against about 105 ms for CUDA,
  measured on an NVIDIA T1200: a process that opens one photo finishes about 150 ms sooner, and from
  the second image on both decode as fast (see [Performance](#performance)). It needs an X display
  (X11 or XWayland, through `DISPLAY`); without one the plugin moves on to `cuda`. NVIDIA's VDPAU
  does not decode the HEVC Main Still Picture profile, which libheif's own encoder writes for 8-bit
  images, so those images are decoded in software; `LIBHEIF_HWDEC_BACKEND=cuda` decodes them on the
  GPU. VDPAU also returns wrong pictures, without reporting an error, for HEVC 4:4:4 at 10 or 12
  bits (the colour planes) and for pictures 64 pixels wide or narrower, measured with NVIDIA drivers
  595.99.02 and 615.71.09; the plugin decodes both in software on this backend.
- `vulkan` takes the first Vulkan device with an H.265 video decode queue. With FFmpeg 6.1 and later
  the plugin creates that device itself, with one queue each for compute, transfers and decoding,
  and the extensions and features FFmpeg enables, then hands it to FFmpeg: FFmpeg's own device setup
  asks for every queue the driver offers, and on an NVIDIA T1200 took 570–800 ms with FFmpeg 6.1,
  about 270 ms with 8.0 and about 190 ms with 8.1 even with its `limit_queues=1` option, against
  about 120 ms for the plugin's. The backend is in the default list only with FFmpeg 8.1 or later:
  with 6.1 to 8.0 it decoded about 37% slower than with 8.1 and 9.0, where it is as fast as `vdpau`
  and `cuda`. Mesa's Intel driver (ANV) exposes video decoding only with `ANV_DEBUG=video-decode`,
  at least up to Mesa 26.2.

Applications can also choose the decoder per call: set `decoder_id = "hwdec"` (or `"libde265"`)
in `heif_decoding_options`, or use `heif-dec -d hwdec`.

## What runs on the GPU

Measured on an Intel Alder Lake-P iGPU (VAAPI) and an NVIDIA T1200 (VDPAU, NVDEC and Vulkan), using
the files in `tests/data/`:

| stream                                                     | Intel, VAAPI | NVIDIA, VDPAU | NVIDIA, CUDA | NVIDIA, Vulkan |
|------------------------------------------------------------|--------------|---------------|--------------|----------------|
| HEVC 8-bit 4:2:0 Main                                      | GPU          | GPU           | GPU          | GPU            |
| HEVC 8-bit 4:2:0 Main Still Picture                        | GPU          | software      | GPU          | GPU            |
| HEVC 10-bit 4:2:0 (Main 10)                                | GPU          | GPU           | GPU          | GPU            |
| HEVC 12-bit 4:2:0 (Main 12)                                | GPU          | GPU           | GPU          | GPU with FFmpeg 6.1 and 7.0, software from 7.1 |
| HEVC 4:2:2 10/12-bit (Main 4:2:2 10/12)                    | GPU          | software      | software     | software       |
| HEVC 4:4:4 8-bit (Main 4:4:4)                              | GPU          | GPU           | GPU          | GPU            |
| HEVC 4:4:4 10-bit (Main 4:4:4 10)                          | GPU          | software      | GPU          | GPU            |
| HEVC 4:4:4 12-bit (Main 4:4:4 12)                          | GPU          | software      | GPU          | as Main 12     |
| HEVC 10/12-bit, 4:2:2 or 4:4:4 with a Range Extensions *Intra* profile | software | software | as above | as above      |
| HEVC monochrome, including alpha planes                    | software     | software      | software     | software       |
| HEVC pictures 64 pixels wide or narrower                   | GPU          | software      | software     | software       |
| HEVC pictures under 144 pixels high                        | GPU          | GPU           | software     | not measured   |
| HEVC pictures over 8192 pixels wide or high                | GPU          | software      | software     | software       |
| HEVC pictures over 16384 pixels wide or high               | software     | software      | software     | software       |
| AV1 Main profile 4:2:0 8/10-bit (driver 26.1 or later)     | GPU          | not supported by the T1200 | not supported by the T1200 | not supported by the T1200 |
| AV1 4:4:4 (High profile), AV1 monochrome                   | software     | —             | —            | —              |

- **Intra profiles:** libheif's own x265 encoder (`heif-enc`) writes 10/12-bit, 4:2:2 and 4:4:4
  images with Range Extensions *Intra* profiles, which FFmpeg's VAAPI code has no mapping for.
- **Monochrome:** FFmpeg offers no hwaccel for monochrome HEVC.
- **Output is identical either way:** every test compares the decoded samples with libde265 or
  dav1d.

## Performance

Measured on an Intel Alder Lake-P iGPU (VAAPI) and an NVIDIA T1200 with driver 615.71.09, with
`heif-hwdec-bench` (no colour conversion) on photos of 9248×6936 in 70 tiles of 1024×1024. Every
run is a fresh process started on an idle machine right after 500 ms of light rendering on the GPU
it uses, as on a desktop whose GPU also drives the screen. Times with FFmpeg 9.0.2; software is
libde265, libheif's regular HEVC decoder:

| decoder                     | first photo in a new process, median of 10 | 75 photos, 5 processes of 15 | device setup |
|-----------------------------|---------:|---------:|------:|
| libde265 (software)         | 600 ms   | 66.0 s   | —      |
| hwdec, Intel iGPU (VAAPI)   | 223 ms   | 23.8 s   | 3 ms   |
| hwdec, NVIDIA T1200 (VDPAU) | 312 ms   | 27.2 s   | 19 ms  |
| hwdec, NVIDIA T1200 (CUDA)  | 469 ms   | 27.6 s   | 106 ms |
| hwdec, NVIDIA T1200 (Vulkan)| 416 ms   | 27.9 s   | 120 ms |

With FFmpeg 7.0, 7.1, 8.0 and 8.1 the VAAPI, VDPAU and CUDA times are within 10% of these for the
first photo and within 2% for the 75; with Ubuntu 24.04's FFmpeg 6.1 they are 5–13% longer. Vulkan
takes 574–589 ms for the first photo and 37.8–38.1 s for the 75 with FFmpeg 6.1 to 8.0, and 461 ms
and 27.8 s with 8.1.0.

The device is set up once per process. Image viewers and KDE's thumbnail workers are long-running
processes and pay it once; one-shot `heif-dec` runs pay it every time. An NVIDIA GPU that has been
idle for a few seconds starts from its lowest power state, which made CUDA's setup about 60 ms
longer; one that a laptop has powered down adds about 1.2 s to the first image on any NVIDIA
backend.

## Testing

```sh
cd build && ctest --output-on-failure
```

- `annexb`, `av1_obu`, `frame_copy`: unit tests, no GPU needed. `frame_copy` checks the
  conversion of 27 FFmpeg pixel formats, including every format FFmpeg downloads GPU frames into,
  with and without cropping.
- `compare_vaapi`, `compare_vdpau`, `compare_cuda`, `compare_vulkan` (label `gpu`): decode the files in
  `tests/data/` with the plugin and with a reference decoder (libde265 for HEIC, dav1d or aom for
  AVIF), and fail on any difference in size, decoded samples or colour profile, or if no picture
  was decoded on the GPU. A backend without a device, or a missing reference decoder, skips the
  test; AVIF files are left out where the plugin does not claim AV1.

`tests/make-fixtures.sh` regenerates `tests/data/` with ImageMagick, `heif-enc`, `ffmpeg`, `x265`
and GPAC's `MP4Box`.

`tests/fuzz-files.py` corrupts the test files and decodes each one in a separate process, keeping
every case that crashes, trips a sanitizer or hangs. Build with
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"` for the sanitizer checks:

```sh
python3 tests/fuzz-files.py build/tests/heif-hwdec-bench build vaapi 2000 fuzz-out tests/data/*.heic tests/data/*.avif
```

`build/tests/heif-hwdec-bench` decodes files in one process and prints, per file, the time and a
hash of the decoded planes, for benchmarking and for comparing decoders on your own images:

```sh
build/tests/heif-hwdec-bench -d libde265 *.heic > a.txt
build/tests/heif-hwdec-bench -d hwdec *.heic > b.txt
```

## License

(c) Marian Kadanka, https://kadanka.net/

LGPL-3.0-or-later, the same as libheif. See `COPYING.LESSER` and `COPYING`.
