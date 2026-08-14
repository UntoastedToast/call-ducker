#!/usr/bin/env bash

# Builds and validates the Fedora binary and source RPM from a source archive
# produced by scripts/build-source-dist.sh.
#
# Usage: scripts/package-rpm.sh <source-archive.tar.xz>

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <source-archive.tar.xz>" >&2
  exit 2
fi

source_archive=$(readlink -f -- "$1")
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

spec=${repo_root}/packaging/rpm/call-ducker.spec
version=$(sed -n 's/^Version:[[:space:]]*//p' "${spec}")
artifact_dir=${repo_root}/artifacts

work_dir=$(mktemp -d -t call-ducker-rpm.XXXXXXXX)
cleanup() {
  rm -rf -- "${work_dir}"
}
trap cleanup EXIT

rpm_topdir=${work_dir}/rpmbuild
mkdir -p "${rpm_topdir}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
mkdir -p "${artifact_dir}"

cp "${source_archive}" "${rpm_topdir}/SOURCES/"
cp "${spec}" "${rpm_topdir}/SPECS/"

rpmbuild -ba \
  --define "_topdir ${rpm_topdir}" \
  --define "_tmppath ${work_dir}" \
  "${rpm_topdir}/SPECS/call-ducker.spec"

mapfile -t binary_rpms < <(
  find "${rpm_topdir}/RPMS" -type f -name "call-ducker-${version}-*.rpm" -print
)
mapfile -t source_rpms < <(
  find "${rpm_topdir}/SRPMS" -type f -name "call-ducker-${version}-*.src.rpm" -print
)
if [[ ${#binary_rpms[@]} -ne 1 || ${#source_rpms[@]} -ne 1 ]]; then
  echo "Expected exactly one binary RPM and one source RPM" >&2
  find "${rpm_topdir}/RPMS" "${rpm_topdir}/SRPMS" -type f -name '*.rpm' -print >&2
  exit 1
fi
binary_rpm=${binary_rpms[0]}
source_rpm=${source_rpms[0]}

rpm -K "${binary_rpm}" "${source_rpm}"
rpmlint "${spec}" "${source_rpm}" "${binary_rpm}"

cp "${binary_rpm}" "${source_rpm}" "${artifact_dir}/"
echo "Created RPM packages for ${version}:"
printf '  %s\n' "$(basename "${binary_rpm}")" "$(basename "${source_rpm}")"
