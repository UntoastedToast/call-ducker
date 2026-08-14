#!/usr/bin/env bash

# Validates packaging/arch/PKGBUILD against a source archive produced by
# scripts/build-source-dist.sh, and optionally emits the AUR-ready copy with the
# published checksum filled in.
#
# CallDucker is distributed to Arch users through the AUR, so the built
# .pkg.tar.zst is a verification artifact only and is never published.
#
# Usage: scripts/package-arch.sh <source-archive.tar.xz> [--emit-aur <path>]

set -euo pipefail

usage() {
  echo "Usage: $0 <source-archive.tar.xz> [--emit-aur <path>]" >&2
}

if [[ $# -ne 1 && $# -ne 3 ]]; then
  usage
  exit 2
fi

source_archive=$(readlink -f -- "$1")
shift

emit_aur=""
if [[ $# -eq 2 ]]; then
  if [[ $1 != --emit-aur ]]; then
    usage
    exit 2
  fi
  emit_aur=$2
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

archive_name=$(basename "${source_archive}")
version=${archive_name#call-ducker-}
version=${version%.tar.xz}
if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Could not derive a version from ${archive_name}" >&2
  exit 1
fi

checksum=$(sha256sum "${source_archive}" | cut -d' ' -f1)

if [[ -n ${emit_aur} ]]; then
  # The in-repo PKGBUILD carries sha256sums=('SKIP') because the archive does not
  # exist until it is built. This copy is what the maintainer commits to the AUR.
  mkdir -p -- "$(dirname -- "${emit_aur}")"
  sed "s|^sha256sums=.*|sha256sums=('${checksum}')|" packaging/arch/PKGBUILD > "${emit_aur}"
  echo "Wrote AUR PKGBUILD for ${version} to ${emit_aur}"
fi

if [[ $(id -u) -eq 0 ]]; then
  if ! getent passwd builder > /dev/null; then
    echo "makepkg cannot run as root; create an unprivileged 'builder' user first" >&2
    echo "(scripts/ci/setup-container.sh arch package does this)" >&2
    exit 1
  fi
  build_user=builder
else
  build_user=""
fi

work_dir=$(mktemp -d -t call-ducker-arch.XXXXXXXX)
cleanup() {
  rm -rf -- "${work_dir}"
}
trap cleanup EXIT

cp "${source_archive}" "${work_dir}/"
# Point the PKGBUILD at the locally built archive instead of the release URL, so
# the packaging is validated against the exact source that will be published.
sed -e "s|^source=.*|source=(\"\${pkgname}-\${pkgver}.tar.xz\")|" \
  -e "s|^sha256sums=.*|sha256sums=('${checksum}')|" \
  packaging/arch/PKGBUILD > "${work_dir}/PKGBUILD"

if [[ -n ${build_user} ]]; then
  chown -R "${build_user}:${build_user}" "${work_dir}"
  runuser -u "${build_user}" -- bash -c "cd '${work_dir}' && makepkg -f --noconfirm"
else
  (cd "${work_dir}" && makepkg -f --noconfirm)
fi

# Anchored on the version so the call-ducker-debug split package that makepkg
# produces alongside it does not match. That package is a build artifact only.
mapfile -t packages < <(
  find "${work_dir}" -maxdepth 1 -type f -name "call-ducker-${version}-*.pkg.tar.zst" -print
)
if [[ ${#packages[@]} -ne 1 ]]; then
  echo "Expected exactly one Arch package" >&2
  find "${work_dir}" -maxdepth 1 -type f -name '*.pkg.tar.*' -print >&2
  exit 1
fi

namcap "${work_dir}/PKGBUILD"
namcap "${packages[0]}"

echo "Validated packaging/arch/PKGBUILD for ${version} (built $(basename "${packages[0]}"))"
