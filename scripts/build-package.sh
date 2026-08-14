#!/usr/bin/env bash

# Local end-to-end Fedora release build: verify every version source, run the
# quality gate, produce the source archive, build and validate the RPMs, and
# checksum the result.
#
# CI does not use this script. It runs the same steps as separate jobs so that
# the Debian and Arch packaging can consume the identical source archive; see
# .github/workflows/package.yml.
#
# Usage: scripts/build-package.sh [--tag [v]MAJOR.MINOR.PATCH]

set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

meson_version=$(scripts/check-versions.sh "$@")

artifact_dir=${repo_root}/artifacts
mkdir -p "${artifact_dir}"
find "${artifact_dir}" -mindepth 1 -maxdepth 1 -type f -delete

scripts/build-source-dist.sh "$@"

source_archive=${repo_root}/dist/call-ducker-${meson_version}.tar.xz
scripts/package-rpm.sh "${source_archive}"
cp "${source_archive}" "${artifact_dir}/"

scripts/checksum-artifacts.sh "${artifact_dir}" \
  --expect "call-ducker-${meson_version}-*.x86_64.rpm" \
  --expect "call-ducker-${meson_version}-*.src.rpm" \
  --expect "call-ducker-${meson_version}.tar.xz"
