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

Maintainers prepare releases from `main`. A release commit updates these version sources together:

- the project version in `meson.build`;
- `Version` and the dated `%changelog` entry in `call-ducker.spec`;
- the newest release in `data/io.github.UntoastedToast.CallDucker.metainfo.xml`;
- the version in both manpage headers.

Run the complete Fedora package build before merging the release commit:

```sh
./scripts/build-package.sh
```

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

The tag workflow verifies every version source, builds and tests the project on Fedora x86_64,
validates the metadata and RPM, and creates a draft GitHub Release. The draft contains the binary
RPM, source RPM, source archive, and `SHA256SUMS`; GitHub build provenance is attached to every
asset. Rerunning the workflow replaces assets in an existing draft and leaves published releases
untouched.

Open a GitHub issue from the [release checklist](.github/ISSUE_TEMPLATE/release.md) and complete
every item against the draft artifacts. Publish the release only after the live audio acceptance
test passes and its evidence is attached to the release issue.

## Repository policy

The `main` branch requires the distribution build matrix, quality gate, and sanitizer checks.
Workflow tokens use read-only permissions by default, releases are immutable, and GitHub Actions
references stay pinned to full commit SHAs. Dependabot submits weekly updates for those references.
