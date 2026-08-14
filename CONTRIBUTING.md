# Contributing to CallDucker

Thanks for helping improve CallDucker. Bug reports, focused pull requests, documentation fixes, and
well-tested feature proposals are all welcome.

## Before you start

CallDucker targets Fedora 43+, current Arch Linux, Debian 13+, Ubuntu 24.04+, and Mint 22+. The
event-driven daemon communicates with PulseAudio or `pipewire-pulse` through libpulse. Contributions
must keep the application local and privacy-preserving: do not add network access, telemetry, root
requirements, audio polling, or `pactl`/`wpctl`/`pw-cli` subprocesses.

For a substantial change, open an issue before writing the implementation. A short description of
the problem, the intended user experience, and any compatibility impact gives maintainers and
contributors a shared direction. Small bug fixes and documentation improvements can go directly to
a pull request.

## Set up the development environment

Install the build and quality-check dependencies on Fedora:

```sh
sudo dnf install appstream clang-tools-extra dbus-daemon dbus-tools dbus-x11 \
  desktop-file-utils gcc glib2-devel gtk4-devel json-glib-devel libadwaita-devel meson \
  ninja-build rpm-build rpmlint ShellCheck systemd-rpm-macros vala pulseaudio \
  pulseaudio-libs-devel \
  xorg-x11-server-Xvfb
```

Build and run the test suite:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

The `integration_tests` Meson feature defaults to `auto` for local work. Use
`meson setup build -Dintegration_tests=enabled` for release validation; this makes missing
PulseAudio or Xvfb test programs a configuration error instead of a skipped test.

After changing build options or dependencies, recreate the build directory with
`meson setup --wipe build`.

## Make a focused change

- Keep each pull request centered on one problem.
- Explain the user-visible behavior and the reason for the change.
- Add or update tests for changed behavior.
- Put user-facing strings in gettext. The interface remains English until translations are added.
- Follow `.clang-format` for C sources and `.editorconfig` for the rest of the repository.
- Update the relevant documentation when commands, settings, or behavior change.

The installed D-Bus XML, GSettings keys, application IDs, filesystem paths, CLI output, and exit
codes are public interfaces. Change them only as an intentional compatibility change and describe
the impact clearly in the pull request.

## Run the checks

Before submitting a pull request, run the same local quality gate used during development:

```sh
./scripts/check-versions.sh
./scripts/check-quality.sh
```

Changes to the daemon, audio state handling, or native code also need the sanitizer build used by
CI:

```sh
meson setup build-sanitize -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-sanitize
meson test -C build-sanitize --print-errorlogs
```

If a check cannot run locally, state which check is missing and why in the pull request. This makes
the remaining review work visible.

## Submit the pull request

Use a concise title and include:

- the problem being solved;
- the approach taken;
- the checks you ran;
- screenshots for visible interface changes;
- compatibility or migration notes when a public interface changes.

Respond to review comments with follow-up commits. Avoid rewriting published history while a review
is active unless a maintainer asks for it; preserving the individual revisions keeps the discussion
easy to follow. Maintainers may squash the commits when merging.

## Release process

Maintainers prepare releases from `main`. A release commit updates these version sources together;
`./scripts/check-versions.sh` fails on any mismatch:

- the project version in `meson.build`, which is the source of truth;
- `Version` and the dated `%changelog` entry in `packaging/rpm/call-ducker.spec`;
- the version and a new entry in `packaging/debian/changelog`, with a plain `-1` revision — the
  per-distribution suffix is added at build time;
- `pkgver` in `packaging/arch/PKGBUILD`, whose `sha256sums` stays `SKIP` because the real checksum
  only exists once the source archive has been built;
- the newest release in `data/io.github.UntoastedToast.CallDucker.metainfo.xml`;
- the version in both manpage headers.

Run the complete Fedora package build before merging the release commit:

```sh
./scripts/build-package.sh
```

That covers the Fedora path only. The Debian and Arch packaging is built by CI on every pull
request, and can be reproduced locally against a container:

```sh
./scripts/build-source-dist.sh
podman run --rm -v "$PWD:/src:z" -w /src debian:13 bash -c \
  './scripts/ci/setup-container.sh debian package &&
   ./scripts/package-deb.sh dist/call-ducker-*.tar.xz deb13 trixie'
podman run --rm -v "$PWD:/src:z" -w /src archlinux:base-devel bash -c \
  './scripts/ci/setup-container.sh arch package &&
   ./scripts/package-arch.sh dist/call-ducker-*.tar.xz'
```

`meson dist` archives `HEAD`, so commit the release changes before building; an uncommitted
packaging file is silently missing from the archive.

After the release commit reaches `main`, derive the version from the repository, create a signed
SemVer tag, and push it:

```sh
git switch main
git pull --ff-only
version=$(sed -n "s/^project('call-ducker'.*version: '\([^']*\)'.*/\1/p" meson.build)
./scripts/check-versions.sh --tag "v${version}"
git tag --sign "v${version}" --message "CallDucker ${version}"
git push origin "v${version}"
```

Release tags are immutable. Correct a release with a new patch version instead of moving or reusing
a tag.

The tag workflow verifies every version source, builds and tests the project, and creates a draft
GitHub Release containing the Fedora binary RPM, the source RPM, the Debian 13 and Ubuntu 24.04
`.deb` packages, the AUR-ready `PKGBUILD`, the source archive, and `SHA256SUMS`. GitHub build
provenance is attached to every asset. Rerunning the workflow refreshes the draft — it replaces
assets, removes ones that are no longer produced, regenerates the notes, and leaves published
releases untouched.

After publishing, update the AUR package: copy the released `PKGBUILD` into the `call-ducker` AUR
repository, regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`, and push. The released
`PKGBUILD` already carries the checksum of the published source archive.

Open a GitHub issue from the [release checklist](.github/ISSUE_TEMPLATE/release.md) and complete
every item against the draft artifacts. Publish the release only after the live audio acceptance
test passes and its evidence is attached to the release issue.

## Repository policy

Changes reach `main` through a pull request. No approving review is required, since the project has
a single maintainer, but the pull request is what makes the checks run before the merge. `main`
requires all eleven of them:

- the distribution build matrix — `Fedora 43`, `Arch Linux`, `Debian 13`, `Ubuntu 24.04`;
- `Quality gate` and `Sanitizers`;
- `Packaging / Source distribution` and the four `Packaging / …` package builds.

A branch has to be up to date with `main` before it merges, conversations have to be resolved, and
history stays linear, so merges are squashes or rebases rather than merge commits. Force pushes and
branch deletion are blocked.

**A job's `name` is its status check name.** Renaming a job stops the required check from ever
reporting and blocks every pull request, so change the required checks on `main` in the same commit.

Workflow tokens use read-only permissions by default — only the release workflow's `publish` job
holds `contents: write`, and it compiles nothing. Releases are immutable, and GitHub Actions
references stay pinned to full commit SHAs. Dependabot submits weekly updates for those references.
