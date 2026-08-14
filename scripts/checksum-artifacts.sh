#!/usr/bin/env bash

# Writes and verifies the aggregated SHA256SUMS over every release asset, and
# refuses to continue if an expected asset is missing.
#
# Usage: scripts/checksum-artifacts.sh <artifact-dir> [--expect <glob>]...

set -euo pipefail

usage() {
  echo "Usage: $0 <artifact-dir> [--expect <glob>]..." >&2
}

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi

artifact_dir=$(readlink -f -- "$1")
shift

expected=()
while [[ $# -gt 0 ]]; do
  case $1 in
    --expect)
      [[ $# -ge 2 ]] || {
        usage
        exit 2
      }
      expected+=("$2")
      shift 2
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

if [[ ! -d ${artifact_dir} ]]; then
  echo "Artifact directory does not exist: ${artifact_dir}" >&2
  exit 1
fi

cd "${artifact_dir}"
rm -f SHA256SUMS

mapfile -t assets < <(find . -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | sort)
if [[ ${#assets[@]} -eq 0 ]]; then
  echo "No release assets found in ${artifact_dir}" >&2
  exit 1
fi

missing=0
for pattern in ${expected[@]+"${expected[@]}"}; do
  found=0
  for asset in "${assets[@]}"; do
    # shellcheck disable=SC2053 # the pattern is a glob on purpose
    if [[ ${asset} == ${pattern} ]]; then
      found=1
      break
    fi
  done
  if [[ ${found} -eq 0 ]]; then
    echo "Missing expected release asset: ${pattern}" >&2
    missing=1
  fi
done
if [[ ${missing} -ne 0 ]]; then
  printf 'Present: %s\n' "${assets[@]}" >&2
  exit 1
fi

sha256sum "${assets[@]}" > SHA256SUMS
sha256sum --check SHA256SUMS

echo "Checksummed ${#assets[@]} release assets:"
printf '  %s\n' "${assets[@]}"
