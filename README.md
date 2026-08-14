# CallDucker

CallDucker sets background applications to a chosen absolute volume while a voice call is active,
then restores every stream to its original per-channel volume. It supports both a native
PulseAudio server and PipeWire's `pipewire-pulse` compatibility server. The daemon uses libpulse's
GLib main-loop integration; it does not invoke `pactl`, `wpctl`, `pw-cli`, access the network,
collect telemetry, or require a root service.

Version 0.1.1 supports Fedora 43+, current Arch Linux, Debian 13+, Ubuntu 24.04+, and Linux Mint
22+. GNOME, KDE Plasma, and Cinnamon are the tested desktops. A local PulseAudio-compatible server
and systemd user services are required. Pure ALSA, JACK, and PipeWire without `pipewire-pulse` are
not supported. GTK 4.12 and libadwaita 1.5 remain minimum requirements.

## Install a release

Every release ships an x86_64 package for each supported distribution. Pick the matching asset from
the [latest GitHub Release](https://github.com/UntoastedToast/call-ducker/releases/latest):

| Distribution | Asset |
| --- | --- |
| <img src="https://cdn.simpleicons.org/fedora/51A2DA" height="14" align="top" alt=""> Fedora 43 and newer | `call-ducker-<version>-1.fc*.x86_64.rpm` |
| <img src="https://cdn.simpleicons.org/debian/A81D33" height="14" align="top" alt=""> Debian 13 and newer | `call-ducker_<version>-1~deb13_amd64.deb` |
| <img src="https://cdn.simpleicons.org/ubuntu/E95420" height="14" align="top" alt=""> <img src="https://cdn.simpleicons.org/linuxmint/87CF3E" height="14" align="top" alt=""> Ubuntu 24.04+ and Linux Mint 22+ | `call-ducker_<version>-1~ubuntu24.04_amd64.deb` |
| <img src="https://cdn.simpleicons.org/archlinux/1793D1" height="14" align="top" alt=""> Arch Linux | `PKGBUILD` |

**Only the Fedora package has been tested in real use so far.** Every package is built and passes
the full automated test suite on its own distribution, including the PulseAudio integration test.
But the live acceptance test — a real desktop session, real games, and a real voice call, against
both PulseAudio and `pipewire-pulse` — has only been carried out on Fedora. Treat the Debian,
Ubuntu, Mint, and Arch packages as working but unproven, and please
[report anything that misbehaves](https://github.com/UntoastedToast/call-ducker/issues).

### Verify the download first

The packages are not GPG-signed, so `dnf` and `apt` warn that they are unsigned. Verify the GitHub
build attestation instead. Download `SHA256SUMS` alongside your package, then, from the download
directory:

```sh
gh attestation verify call-ducker* \
  --repo UntoastedToast/call-ducker \
  --signer-workflow UntoastedToast/call-ducker/.github/workflows/release.yml
sha256sum --ignore-missing --check SHA256SUMS
```

This needs the [GitHub CLI](https://cli.github.com/). `--ignore-missing` checks the assets you
actually downloaded and skips the rest.

<details open>
<summary>
  <img src="https://cdn.simpleicons.org/fedora/51A2DA" height="16" align="top" alt="">
  <b>Fedora 43 and newer</b>
</summary>

```sh
sudo dnf install ./call-ducker-*.x86_64.rpm
```

`dnf` resolves the PulseAudio dependency itself. Confirm the unsigned-package prompt.

</details>

<details>
<summary>
  <img src="https://cdn.simpleicons.org/debian/A81D33" height="16" align="top" alt="">
  <b>Debian 13 and newer</b>
</summary>

```sh
sudo apt install ./call-ducker_*~deb13_amd64.deb
```

Use `apt install ./file.deb`, not `dpkg -i` — the leading `./` is what makes `apt` treat it as a
local file, and only `apt` pulls in the dependencies. If you already run PipeWire, `apt` keeps it;
`pulseaudio` is only installed when no PulseAudio-compatible server is present yet.

</details>

<details>
<summary>
  <img src="https://cdn.simpleicons.org/ubuntu/E95420" height="16" align="top" alt="">
  <img src="https://cdn.simpleicons.org/linuxmint/87CF3E" height="16" align="top" alt="">
  <b>Ubuntu 24.04+ and Linux Mint 22+</b>
</summary>

```sh
sudo apt install ./call-ducker_*~ubuntu24.04_amd64.deb
```

Use `apt install ./file.deb`, not `dpkg -i` — the leading `./` is what makes `apt` treat it as a
local file, and only `apt` pulls in the dependencies.

Linux Mint 22 is built on Ubuntu 24.04, so it uses the Ubuntu package. There is no separate Mint
asset.

</details>

<details>
<summary>
  <img src="https://cdn.simpleicons.org/archlinux/1793D1" height="16" align="top" alt="">
  <b>Arch Linux</b>
</summary>

Arch has no binary asset on purpose: Arch is a rolling release, so a package built against one
week's `gtk4` or `libpulse` breaks as soon as those libraries change. Build from the released
`PKGBUILD`, which downloads and checksums the published source archive:

```sh
mkdir call-ducker && cd call-ducker
curl -LO https://github.com/UntoastedToast/call-ducker/releases/latest/download/PKGBUILD
makepkg -si
```

`makepkg -s` installs the build dependencies and `-i` installs the finished package.

</details>

### Start the service

```sh
systemctl --user start call-ducker.service
```

Only needed once — the service starts automatically on later logins. Check it with
`systemctl --user status call-ducker.service`, then open CallDucker from your application menu.

### Notes

Every package is built from the published `call-ducker-<version>.tar.xz`, which contains the
`packaging/` directory, so that archive alone is enough to rebuild any of them.

There is no Flatpak, and there will not be one: CallDucker installs a systemd user service and
inspects host processes to recognise games launched by Steam, Heroic, or Lutris. A Flatpak sandbox
permits neither, so a Flatpak build would install and run but never detect a game.

To uninstall, use `sudo dnf remove call-ducker`, `sudo apt remove call-ducker`, or
`sudo pacman -R call-ducker`.

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

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) for development checks and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for backend and restore guarantees.

CallDucker is licensed under the GNU GPL v3.0 or later.

Copyright © 2026 UntoastedToast.
