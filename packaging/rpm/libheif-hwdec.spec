# Builds the libheif-hwdec plugin for Fedora (with FFmpeg from RPM Fusion: Fedora's ffmpeg-free has
# no HEVC decoder) and openSUSE (with FFmpeg from Packman, for the same reason).
#
#   rpmbuild -ba packaging/rpm/libheif-hwdec.spec   with the release tarball in SOURCES

Name:           libheif-hwdec
Version:        1.0.1
Release:        1%{?dist}
Summary:        GPU-accelerated HEVC and AV1 decoder plugin for libheif
License:        LGPL-3.0-or-later
URL:            https://github.com/marian-kadanka/libheif-hwdec
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(libheif) >= 1.15
BuildRequires:  pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavutil)
BuildRequires:  pkgconfig(libva)
BuildRequires:  pkgconfig(vulkan)

%if 0%{?suse_version}
%global libheif_package libheif1
%else
%global libheif_package libheif
%endif

# Which libheif releases load the plugin depends on the plugin API version the build chooses from
# the libheif headers (HWDEC_PLUGIN_API in src/plugin.cc): libheif 1.21 and later refuse versions
# below 5, and versions 5 and 6 need the libheif release that introduced them. The version of the
# libheif installed for the build decides which one that is.
%global libheif_build_version %(pkg-config --modversion libheif 2>/dev/null || echo 0)
%{lua:
local major, minor = rpm.expand("%{libheif_build_version}"):match("^(%d+)%.(%d+)")
major, minor = tonumber(major or 0), tonumber(minor or 0)
local pkg = rpm.expand("%{libheif_package}%{?_isa}")
if major == 1 and minor >= 22 then
  print("Requires: " .. pkg .. " >= 1.22\n")
elseif major == 1 and minor == 21 then
  print("Requires: " .. pkg .. " >= 1.21\n")
elseif major == 1 and minor == 20 then
  print("Requires: " .. pkg .. " >= 1.20\nRequires: " .. pkg .. " < 1.21\n")
elseif major == 1 and minor >= 15 then
  print("Requires: " .. pkg .. " >= 1.15\nRequires: " .. pkg .. " < 1.21\n")
end
}
Enhances:       %{libheif_package}

%description
Decodes HEIC (HEVC) and AVIF (AV1) images on the GPU through FFmpeg's hardware
acceleration API, using VAAPI (Intel, AMD, and NVIDIA with its VAAPI driver),
VDPAU (NVIDIA), CUDA/NVDEC (NVIDIA) or Vulkan Video. Every program that reads
HEIC or AVIF files through libheif uses it once the package is installed,
including image viewers that use KDE's Qt image format plugins, such as Gwenview.

When no suitable GPU is present the plugin steps aside and libheif keeps using
its usual decoders. Streams the GPU cannot decode are decoded in software by
FFmpeg.

%prep
%autosetup

%build
%cmake -DHWDEC_BUILD_TESTS=ON
%cmake_build

%install
%cmake_install
# The plugin directory is the one this libheif reports: lib64/libheif on Fedora and openSUSE,
# <libdir>/libheif/plugins upstream.
find %{buildroot} -name libheif-hwdec.so | sed 's|^%{buildroot}||' > plugin.files

%check
# The tests labelled "gpu" compare against libde265 on a real GPU and are skipped in a build root.
%ctest --label-exclude gpu

%files -f plugin.files
%license COPYING COPYING.LESSER
%doc README.md

%changelog
* Sun Oct 18 2026 Marian Kadanka <marian@kadanka.net> - 1.0.1-1
- Fix the build against an FFmpeg whose libavutil reports version 8.1 but
  lacks av_vk_get_optional_device_extensions(), such as openSUSE's minimal
  FFmpeg 8: the Vulkan backend now looks the function up at run time and
  otherwise uses its own list of the Vulkan extensions FFmpeg can use

* Sun Oct 18 2026 Marian Kadanka <marian@kadanka.net> - 1.0.0-1
- Add a VDPAU backend, tried after VAAPI and before CUDA: on NVIDIA GPUs it
  sets up in about 20 ms, against about 105 ms for CUDA. HEVC 4:4:4 at 10 or
  12 bits and pictures 64 pixels wide or narrower are decoded in software on
  it, because NVIDIA's VDPAU returns them wrong
- Create the Vulkan device in the plugin, with one queue each for compute,
  transfers and decoding: setting up an NVIDIA GPU takes about 120 ms instead
  of up to 800 ms
- Try Vulkan after the other backends with FFmpeg 8.1 or later, when the
  machine has a GPU device node

* Sat Oct 17 2026 Marian Kadanka <marian@kadanka.net> - 0.1.1-1
- Decode in software when the GPU hands back a frame in a format too narrow
  for the stream, as nvidia-vaapi-driver does with 10- and 12-bit 4:4:4
- Skip nvidia-vaapi-driver's egl backend when probing VAAPI devices: its
  decodes never finish

* Sat Oct 17 2026 Marian Kadanka <marian@kadanka.net> - 0.1.0-1
- Initial package
