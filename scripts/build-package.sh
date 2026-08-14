#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"
meson_version=$(scripts/check-versions.sh "$@")

work_dir=$(mktemp -d -t call-ducker-build.XXXXXXXX)
cleanup() {
  rm -rf -- "${work_dir}"
}
trap cleanup EXIT

build_dir=${work_dir}/build
rpm_topdir=${work_dir}/rpmbuild
artifact_dir=${repo_root}/artifacts

mkdir -p "${rpm_topdir}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
mkdir -p "${artifact_dir}"
find "${artifact_dir}" -mindepth 1 -maxdepth 1 -type f -delete

scripts/check-quality.sh "${build_dir}"
meson dist -C "${build_dir}" --no-tests

source_archive=${build_dir}/meson-dist/call-ducker-${meson_version}.tar.xz
if [[ ! -f ${source_archive} ]]; then
  echo "Expected source archive was not created: ${source_archive}" >&2
  exit 1
fi
cp "${source_archive}" "${rpm_topdir}/SOURCES/"
cp call-ducker.spec "${rpm_topdir}/SPECS/"

rpmbuild -ba \
  --define "_topdir ${rpm_topdir}" \
  --define "_tmppath ${work_dir}" \
  "${rpm_topdir}/SPECS/call-ducker.spec"

mapfile -t binary_rpms < <(
  find "${rpm_topdir}/RPMS" -type f -name "call-ducker-${meson_version}-*.rpm" -print
)
mapfile -t source_rpms < <(
  find "${rpm_topdir}/SRPMS" -type f -name "call-ducker-${meson_version}-*.src.rpm" -print
)
if [[ ${#binary_rpms[@]} -ne 1 || ${#source_rpms[@]} -ne 1 ]]; then
  echo "Expected exactly one binary RPM and one source RPM" >&2
  find "${rpm_topdir}/RPMS" "${rpm_topdir}/SRPMS" -type f -name '*.rpm' -print >&2
  exit 1
fi
binary_rpm=${binary_rpms[0]}
source_rpm=${source_rpms[0]}

rpm -K "${binary_rpm}" "${source_rpm}"
rpmlint call-ducker.spec "${source_rpm}" "${binary_rpm}"

cp "${binary_rpm}" "${source_rpm}" "${source_archive}" "${artifact_dir}/"
(
  cd "${artifact_dir}"
  sha256sum \
    "$(basename "${binary_rpm}")" \
    "$(basename "${source_rpm}")" \
    "$(basename "${source_archive}")" > SHA256SUMS
  sha256sum --check SHA256SUMS
)

expected_assets=(
  "$(basename "${binary_rpm}")"
  "$(basename "${source_rpm}")"
  "call-ducker-${meson_version}.tar.xz"
  "SHA256SUMS"
)
for asset in "${expected_assets[@]}"; do
  [[ -f ${artifact_dir}/${asset} ]] || {
    echo "Missing release asset: ${asset}" >&2
    exit 1
  }
done

asset_count=$(find "${artifact_dir}" -mindepth 1 -maxdepth 1 -type f | wc -l)
if [[ ${asset_count} -ne ${#expected_assets[@]} ]]; then
  echo "Unexpected files found in ${artifact_dir}" >&2
  find "${artifact_dir}" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' >&2
  exit 1
fi

echo "Created and verified ${asset_count} release assets for ${meson_version}:"
printf '  %s\n' "${expected_assets[@]}"
