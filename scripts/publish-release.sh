#!/usr/bin/env bash

# Creates or refreshes the draft GitHub Release for a tag and uploads every
# release asset. Never touches a release that has already been published.
#
# Usage: scripts/publish-release.sh <tag> <artifact-dir>
#
# Requires GH_TOKEN and GITHUB_REPOSITORY in the environment. RELEASE_NOTICE is
# appended to the generated release notes.

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <tag> <artifact-dir>" >&2
  exit 2
fi

tag=$1
artifact_dir=$2
notice=${RELEASE_NOTICE:-}

: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"

mapfile -t assets < <(find "${artifact_dir}" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | sort)
if [[ ${#assets[@]} -eq 0 ]]; then
  echo "No release assets found in ${artifact_dir}" >&2
  exit 1
fi

# Deliberately not "gh release view <tag>": its exit status cannot distinguish a
# missing release from a network or authentication failure, and guessing wrong
# means publishing into the wrong branch of this script. Listing releases fails
# loudly instead, and a successful list that omits the tag is proof of absence.
# The GET /releases/tags/{tag} endpoint is unusable here because it does not
# return drafts.
releases=$(gh release list --limit 200 --json tagName,isDraft)

matched=$(jq -r --arg tag "${tag}" '[.[] | select(.tagName == $tag)] | length' <<< "${releases}")

if [[ ${matched} -eq 0 ]]; then
  echo "Creating draft release ${tag}"
  mapfile -t asset_paths < <(printf '%s\n' "${assets[@]/#/${artifact_dir}/}")
  gh release create "${tag}" "${asset_paths[@]}" \
    --draft \
    --generate-notes \
    --notes "${notice}" \
    --title "CallDucker ${tag#v}" \
    --verify-tag
  exit 0
fi

is_draft=$(jq -r --arg tag "${tag}" 'first(.[] | select(.tagName == $tag)) | .isDraft' <<< "${releases}")
if [[ ${is_draft} != true ]]; then
  echo "Refusing to modify published release ${tag}" >&2
  exit 1
fi

echo "Refreshing draft release ${tag}"

# --clobber only replaces same-named assets. Asset names carry the distribution
# suffix and the Fedora dist tag, so a rerun can legitimately produce a different
# set; anything left over from a previous run has to go.
mapfile -t existing < <(gh release view "${tag}" --json assets --jq '.assets[].name')
for asset in ${existing[@]+"${existing[@]}"}; do
  if [[ ! -f ${artifact_dir}/${asset} ]]; then
    echo "Removing stale asset ${asset}"
    gh release delete-asset "${tag}" "${asset}" --yes
  fi
done

mapfile -t asset_paths < <(printf '%s\n' "${assets[@]/#/${artifact_dir}/}")
gh release upload "${tag}" "${asset_paths[@]}" --clobber

# gh release edit has no --generate-notes, so passing --notes alone would replace
# the generated changelog with just the notice. Regenerate the body instead.
notes_file=$(mktemp)
cleanup() {
  rm -f -- "${notes_file}"
}
trap cleanup EXIT

gh api --method POST "repos/${GITHUB_REPOSITORY}/releases/generate-notes" \
  -f tag_name="${tag}" --jq .body > "${notes_file}"
if [[ -n ${notice} ]]; then
  printf '\n%s\n' "${notice}" >> "${notes_file}"
fi

gh release edit "${tag}" \
  --title "CallDucker ${tag#v}" \
  --notes-file "${notes_file}"
