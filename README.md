# CallDucker

CallDucker sets background applications to a chosen absolute volume while a voice call is active,
then restores every stream to its original per-channel volume. It supports both a native
PulseAudio server and PipeWire's `pipewire-pulse` compatibility server. The daemon uses libpulse's
GLib main-loop integration; it does not invoke `pactl`, `wpctl`, `pw-cli`, access the network,
collect telemetry, or require a root service.

Version 0.1.0 supports Fedora 43+, current Arch Linux, Debian 13+, Ubuntu 24.04+, and Linux Mint
22+. GNOME, KDE Plasma, and Cinnamon are the tested desktops. A local PulseAudio-compatible server
and systemd user services are required. Pure ALSA, JACK, and PipeWire without `pipewire-pulse` are
not supported. GTK 4.12 and libadwaita 1.5 remain minimum requirements.

## Install a release

Binary releases currently contain an x86_64 Fedora RPM. Download it from the
[latest GitHub Release](https://github.com/UntoastedToast/call-ducker/releases/latest), verify its
GitHub attestation, and install it:

```sh
gh attestation verify call-ducker-*.rpm \
  --repo UntoastedToast/call-ducker \
  --signer-workflow UntoastedToast/call-ducker/.github/workflows/release.yml
sudo dnf install ./call-ducker-*.rpm
```

Arch, Debian, Ubuntu, and Mint users install from source for now.

## Build from source

Fedora 43 and newer:

```sh
sudo dnf install gcc meson ninja-build vala glib2-devel pulseaudio-libs-devel \
  json-glib-devel gtk4-devel libadwaita-devel desktop-file-utils appstream
```

Arch Linux:

```sh
sudo pacman -S --needed base-devel meson ninja vala glib2 libpulse json-glib gtk4 \
  libadwaita desktop-file-utils appstream
```

Debian 13 and newer, Ubuntu 24.04 and newer, or Linux Mint 22 and newer:

```sh
sudo apt update
sudo apt install build-essential meson ninja-build valac libglib2.0-dev libpulse-dev \
  libjson-glib-dev libgtk-4-dev libadwaita-1-dev desktop-file-utils appstream
```

Build, test, and install with the same commands on every distribution:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
sudo meson install -C build
systemctl --user daemon-reload
```

Integration tests are discovered automatically for local builds. Maintainers and CI use
`-Dintegration_tests=enabled` so configuration fails instead of silently omitting the private
PulseAudio or Xvfb tests when their programs are missing.

## Diagnostics and state

```sh
call-duckerctl status --json
call-duckerctl list-apps
call-duckerctl preview
call-duckerctl restore
journalctl --user -u call-ducker.service
```

Original volumes are stored atomically in
`$XDG_STATE_HOME/call-ducker/pulse-restore.json`, including their channel layouts. An unknown or
corrupt journal is preserved and blocks adjustment instead of risking an incorrect restore.
Version 0.1.0 deliberately does not read, convert, detect, or modify the old `restore.json` file or
old settings.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) for development checks and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for backend and restore guarantees.

CallDucker is licensed under the GNU GPL v3.0 or later.

Copyright © 2026 UntoastedToast.
