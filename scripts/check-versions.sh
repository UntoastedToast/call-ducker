#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "Usage: $0 [--tag [v]MAJOR.MINOR.PATCH]" >&2
}

release_tag=""
if [[ $# -gt 0 ]]; then
  if [[ $# -ne 2 || $1 != --tag ]]; then
    usage
    exit 2
  fi
  release_tag=$2
fi

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

meson_version=$(sed -n "s/^project('call-ducker'.*version: '\([^']*\)'.*/\1/p" meson.build)
spec_version=$(sed -n 's/^Version:[[:space:]]*//p' packaging/rpm/call-ducker.spec)
deb_version=$(sed -n '1s/^call-ducker (\([0-9.]*\)-.*/\1/p' packaging/debian/changelog)
pkgbuild_version=$(sed -n 's/^pkgver=//p' packaging/arch/PKGBUILD)
appstream_version=$(grep -oE '<release version="[^"]+"' \
  data/io.github.UntoastedToast.CallDucker.metainfo.xml | \
  sed -n '1{s/.*version="//; s/"$//; p;}')
man_version=$(sed -n '1s/.*"CallDucker \([^"]*\)"$/\1/p' data/call-ducker.1)
ctl_man_version=$(sed -n '1s/.*"CallDucker \([^"]*\)"$/\1/p' data/call-duckerctl.1)

if [[ ! ${meson_version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Meson version is missing or invalid: ${meson_version:-<missing>}" >&2
  exit 1
fi

for version_source in \
  "RPM spec:${spec_version}" \
  "Debian changelog:${deb_version}" \
  "Arch PKGBUILD:${pkgbuild_version}" \
  "AppStream:${appstream_version}" \
  "call-ducker.1:${man_version}" \
  "call-duckerctl.1:${ctl_man_version}"; do
  source_name=${version_source%%:*}
  source_version=${version_source#*:}
  if [[ ${source_version} != "${meson_version}" ]]; then
    echo "Version mismatch: ${source_name} has '${source_version:-<missing>}', expected '${meson_version}'" >&2
    exit 1
  fi
done

if [[ -n ${release_tag} ]]; then
  if [[ ! ${release_tag} =~ ^v?[0-9]+\.[0-9]+\.[0-9]+$ ]] || \
    [[ ${release_tag#v} != "${meson_version}" ]]; then
    echo "Release tag ${release_tag} does not match ${meson_version}" >&2
    exit 1
  fi
fi

printf '%s\n' "${meson_version}"
