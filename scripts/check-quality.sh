#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-${repo_root}/build}
integration_tests=${CALL_DUCKER_INTEGRATION_TESTS:-auto}
cd "${repo_root}"

mapfile -t c_sources < <(find src tests -type f \( -name '*.c' -o -name '*.h' \) -print | sort)
clang-format --dry-run --Werror "${c_sources[@]}"
mapfile -t shell_sources < <(find scripts tests -type f -name '*.sh' -print | sort)
shellcheck "${shell_sources[@]}"

if [[ -f ${build_dir}/meson-private/coredata.dat ]]; then
  meson setup --reconfigure "${build_dir}" "${repo_root}" -Dwerror=true \
    -Dintegration_tests="${integration_tests}"
else
  meson setup "${build_dir}" "${repo_root}" -Dwerror=true \
    -Dintegration_tests="${integration_tests}"
fi
meson compile -C "${build_dir}"
meson test -C "${build_dir}" --print-errorlogs
desktop-file-validate data/io.github.UntoastedToast.CallDucker.desktop
appstreamcli validate --no-net data/io.github.UntoastedToast.CallDucker.metainfo.xml
