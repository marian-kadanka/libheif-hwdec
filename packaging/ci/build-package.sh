#!/bin/bash
# Builds the binary package of libheif-hwdec for one distribution, inside a container of that
# distribution, and copies it to OUT_DIR. The package version carries the distribution as a
# suffix (0.1.0-1+ubuntu24.04), so the packages of one release do not collide. Unit tests run as
# part of each package build; the GPU tests are excluded.
#
# Used by .github/workflows/packages.yml, and runs the same way locally:
#   podman run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" IMAGE /src/packaging/ci/build-package.sh DISTRO /out
#
# DISTRO: ubuntu-24.04 ubuntu-26.04 debian-13 debian-testing fedora-44 opensuse-tumbleweed arch
#
# It installs build dependencies with the distribution's package manager, so it refuses to run
# outside a container.

set -euo pipefail

if [ $# -ne 2 ]; then
  sed -n '2,14p' "$0"
  exit 2
fi
distro=$1
out=$2

if [ ! -f /.dockerenv ] && [ ! -f /run/.containerenv ]; then
  echo "build-package.sh installs packages; run it inside a container (see the comment at the top)" >&2
  exit 1
fi

src=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -n 's/^project(libheif-hwdec VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
if [ "${GITHUB_REF_TYPE:-}" = tag ] && [ "${GITHUB_REF_NAME:-}" != "v$version" ]; then
  echo "tag ${GITHUB_REF_NAME} does not match the version $version in CMakeLists.txt" >&2
  exit 1
fi

work=$(mktemp -d)
mkdir -p "$out"
out=$(cd "$out" && pwd)

# The source tree as a release tarball unpacks: libheif-hwdec-<version>/ without build output.
copy_source() {
  mkdir -p "$1"
  tar -C "$src" --exclude=./.git --exclude=./build --exclude='./build-*' --exclude=./dist \
    --exclude='./obj-*' -cf - . | tar -C "$1" -xf -
}

case "$distro" in
  ubuntu-* | debian-*)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential debhelper devscripts cmake pkgconf \
      libheif-dev libavcodec-dev libavutil-dev libva-dev libvulkan-dev > /dev/null
    tree="$work/libheif-hwdec-$version"
    copy_source "$tree"
    suffix=${distro/-/}
    sed -i "1s/(\([^)]*\))/(\1+$suffix)/" "$tree/debian/changelog"
    (cd "$tree" && dpkg-buildpackage -b -us -uc)
    find "$work" -maxdepth 1 -name '*.deb' ! -name '*dbgsym*' -exec cp -v {} "$out/" \;
    ;;

  fedora-* | opensuse-*)
    if command -v dnf > /dev/null; then
      dnf -y -q install rpm-build tar gzip > /dev/null
      dnf -y -q builddep "$src/packaging/rpm/libheif-hwdec.spec" > /dev/null
      dist=$(rpm --eval '%{?dist}')
    else
      zypper -n -q install rpm-build tar gzip > /dev/null
      rpmspec -q --buildrequires "$src/packaging/rpm/libheif-hwdec.spec" | xargs -d '\n' zypper -n -q install > /dev/null
      dist=".${distro/opensuse-/suse.}"
    fi
    copy_source "$work/libheif-hwdec-$version"
    sources=$(rpm --eval '%{_sourcedir}')
    mkdir -p "$sources"
    tar -C "$work" -czf "$sources/libheif-hwdec-$version.tar.gz" "libheif-hwdec-$version"
    rpmbuild -bb --define "dist $dist" "$src/packaging/rpm/libheif-hwdec.spec"
    find "$(rpm --eval '%{_rpmdir}')" -name 'libheif-hwdec-[0-9]*.rpm' -exec cp -v {} "$out/" \;
    ;;

  arch)
    pacman -Syu --noconfirm --needed base-devel cmake libheif ffmpeg libva vulkan-headers > /dev/null
    # makepkg refuses to run as root.
    id builder > /dev/null 2>&1 || useradd -m builder
    build="$work/arch"
    mkdir -p "$build"
    cp "$src/packaging/arch/PKGBUILD" "$build/"
    copy_source "$work/libheif-hwdec-$version"
    tar -C "$work" -czf "$build/libheif-hwdec-$version.tar.gz" "libheif-hwdec-$version"
    chown -R builder "$work"
    # The tarball is already in place, so makepkg does not download it; it has no fixed checksum
    # before the release exists.
    su builder -c "cd '$build' && PKGEXT=.pkg.tar.zst makepkg --noconfirm --skipchecksums"
    cp -v "$build"/libheif-hwdec-[0-9]*.pkg.tar.zst "$out/"
    ;;

  *)
    echo "unknown distribution: $distro" >&2
    exit 2
    ;;
esac
