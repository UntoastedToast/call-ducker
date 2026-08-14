#!/usr/bin/env bash

# Verifies every version source, runs the full quality gate, and produces the
# one canonical source archive that every distribution package is built from.
#
# Usage: scripts/build-source-dist.sh [--tag [v]MAJOR.MINOR.PATCH]

set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

meson_version=$(scripts/check-versions.sh "$@")

# meson dist runs git archive against HEAD, so anything uncommitted -- a new
# packaging file in particular -- is silently absent from the archive and only
# surfaces as a confusing failure in a downstream packaging step.
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "warning: working tree is dirty; the archive contains HEAD only" >&2
fi

work_dir=$(mktemp -d -t call-ducker-dist.XXXXXXXX)
cleanup() {
  rm -rf -- "${work_dir}"
}
trap cleanup EXIT

build_dir=${work_dir}/build
dist_dir=${repo_root}/dist

mkdir -p "${dist_dir}"
find "${dist_dir}" -mindepth 1 -maxdepth 1 -type f -delete

scripts/check-quality.sh "${build_dir}"
meson dist -C "${build_dir}" --no-tests

source_archive=${build_dir}/meson-dist/call-ducker-${meson_version}.tar.xz
if [[ ! -f ${source_archive} ]]; then
  echo "Expected source archive was not created: ${source_archive}" >&2
  exit 1
fi

cp "${source_archive}" "${dist_dir}/"
echo "Created dist/call-ducker-${meson_version}.tar.xz"
