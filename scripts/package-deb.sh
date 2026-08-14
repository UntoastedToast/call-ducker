#!/usr/bin/env bash

# Builds and validates a Debian-family binary package from a source archive
# produced by scripts/build-source-dist.sh.
#
# Debian 13 and Ubuntu 24.04 would otherwise both emit
# call-ducker_<version>-1_amd64.deb, so the Debian revision carries a
# distribution suffix. A tilde sorts below a plain -1, which is the correct
# ordering for an unofficial per-distribution build.
#
# Usage: scripts/package-deb.sh <source-archive.tar.xz> <suffix> <codename>
#   e.g. scripts/package-deb.sh dist/call-ducker-0.1.0.tar.xz deb13 trixie

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <source-archive.tar.xz> <suffix> <codename>" >&2
  exit 2
fi

source_archive=$(readlink -f -- "$1")
suffix=$2
codename=$3

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

archive_name=$(basename "${source_archive}")
version=${archive_name#call-ducker-}
version=${version%.tar.xz}
if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Could not derive a version from ${archive_name}" >&2
  exit 1
fi

artifact_dir=${repo_root}/artifacts
mkdir -p "${artifact_dir}"

work_dir=$(mktemp -d -t call-ducker-deb.XXXXXXXX)
cleanup() {
  rm -rf -- "${work_dir}"
}
trap cleanup EXIT

tar -xf "${source_archive}" -C "${work_dir}"
source_dir=${work_dir}/call-ducker-${version}
if [[ ! -d ${source_dir}/packaging/debian ]]; then
  echo "Source archive does not contain packaging/debian" >&2
  exit 1
fi

# Build from the packaging shipped inside the archive so the package matches the
# source that is published alongside it.
cp -a "${source_dir}/packaging/debian" "${source_dir}/debian"

deb_version=${version}-1~${suffix}
(
  cd "${source_dir}"
  DEBEMAIL="45534729+UntoastedToast@users.noreply.github.com" \
    DEBFULLNAME="UntoastedToast" \
    dch --force-bad-version \
    --newversion "${deb_version}" \
    --distribution "${codename}" \
    "Build for ${suffix}."
  dpkg-buildpackage -b -us -uc
)

# The glob deliberately excludes the automatically generated call-ducker-dbgsym
# package: it has no counterpart in the RPM asset set and is useless without a
# matching debug archive, so it is not published.
mapfile -t debs < <(find "${work_dir}" -maxdepth 1 -type f -name "call-ducker_${deb_version}_*.deb" -print)
if [[ ${#debs[@]} -ne 1 ]]; then
  echo "Expected exactly one binary package for ${deb_version}" >&2
  find "${work_dir}" -maxdepth 1 -type f -name '*.deb' -print >&2
  exit 1
fi
deb=${debs[0]}

# meson.build templates libexecdir into the unit's ExecStart, so a wrong
# --libexecdir produces a package that installs cleanly and then fails to start.
# Assert the layout rather than trusting the configure override.
contents=$(dpkg-deb -c "${deb}")
for path in \
  './usr/lib/call-ducker/call-ducker-daemon' \
  './usr/lib/systemd/user/call-ducker.service' \
  './usr/bin/call-ducker' \
  './usr/bin/call-duckerctl'; do
  if ! grep -qF " ${path}" <<< "${contents}"; then
    echo "Package is missing ${path}" >&2
    echo "${contents}" >&2
    exit 1
  fi
done

unit=$(dpkg-deb --fsys-tarfile "${deb}" | tar -xO ./usr/lib/systemd/user/call-ducker.service)
if ! grep -qx 'ExecStart=/usr/lib/call-ducker/call-ducker-daemon' <<< "${unit}"; then
  echo "call-ducker.service does not point at the packaged daemon" >&2
  echo "${unit}" >&2
  exit 1
fi

lintian --fail-on error "${deb}"

# GitHub rewrites a tilde to a dot in release asset names, which would leave
# SHA256SUMS listing a file name that does not exist after download. Rename the
# file here so it survives the upload untouched. The package version keeps its
# tilde -- that is the string apt compares, and the point of the tilde is that
# it sorts below a later official -1 revision.
asset_name=$(basename "${deb}" | tr '~' '.')
cp "${deb}" "${artifact_dir}/${asset_name}"
echo "Created ${asset_name} (package version ${deb_version})"
