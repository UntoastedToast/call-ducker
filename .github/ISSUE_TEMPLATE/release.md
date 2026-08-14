---
name: Release checklist
about: Validate and publish a CallDucker release
title: Release
labels: ''
assignees: ''
---

# Release checklist

## Candidate

- [ ] The release commit is on `main`, the signed tag points to it, and CI is green.
- [ ] The draft contains exactly these assets and nothing else:
  - [ ] one Fedora binary RPM and one source RPM
  - [ ] `call-ducker_<version>-1.deb13_amd64.deb`
  - [ ] `call-ducker_<version>-1.ubuntu24.04_amd64.deb`
  - [ ] `PKGBUILD`, whose `sha256sums` matches the published source archive
  - [ ] `call-ducker-<version>.tar.xz` and `SHA256SUMS`
- [ ] Attestations and `sha256sum --check SHA256SUMS` validate every artifact.

## Install coverage

Each install check confirms that `systemctl --user status call-ducker.service` is clean and that
the unit's `ExecStart` path exists — that is what catches a packaging layout regression.

- [ ] Fedora 43: `sudo dnf install ./call-ducker-*.rpm`.
- [ ] Debian 13: `sudo apt install ./call-ducker_*deb13_amd64.deb`.
- [ ] Ubuntu 24.04: `sudo apt install ./call-ducker_*ubuntu24.04_amd64.deb`.
- [ ] Linux Mint 22: the Ubuntu 24.04 package installs and smoke-tests in Cinnamon.
- [ ] Arch Linux: `makepkg -si` from the released `PKGBUILD` succeeds.

## Automated coverage

- [ ] Fedora 43, Arch, Debian 13, and Ubuntu 24.04 CI builds and tests passed.
- [ ] The packaging workflow built the RPM, both debs, and the Arch package.
- [ ] The private PulseAudio/null-sink integration test passed.

## Live audio coverage

Test a native PulseAudio session where available and a Fedora `pipewire-pulse` live session. Use
GNOME, Plasma, and Cinnamon application mixers to record values.

- [ ] A stream starting at 80% and one starting at 40% both show exactly a 70% target.
- [ ] Targets of 0% and 100% behave as volume zero and normal PulseAudio volume respectively.
- [ ] Stereo/multichannel balance is retained and an entirely silent stream receives a nonzero
      target on every channel.
- [ ] A channel-layout change remaps by channel position; an invalid or ambiguous remap stays
      pending without changing the stream.
- [ ] Existing mute flags never change.
- [ ] Multiple streams from one application are changed and restored separately and exactly.
- [ ] New streams during a call receive the target immediately.
- [ ] Policy and target changes reconcile without compounding earlier volume changes.
- [ ] Ending a call, cancelling Preview, disabling the service, and stopping the daemon restore all
      reachable streams.
- [ ] Restarting the audio server reconnects automatically and completes pending work.
- [ ] Injected set and restore failures remain in `pulse-restore.json` until confirmed success.
- [ ] A five-second Preview remains applied for five seconds after its set operations complete and
      includes a matching stream that appears while Preview is active.
- [ ] A corrupt or unknown journal remains byte-for-byte unchanged and blocks new adjustments.
- [ ] A second daemon instance exits without connecting to audio or touching state; acquired-name
      loss follows the guarded restore path.
- [ ] The legacy `restore.json` and legacy setting are neither read nor modified.

## Approval

- [ ] `PendingRestorations` is zero after the final restore.
- [ ] Logs contain no unexplained backend, journal, or D-Bus errors.
- [ ] Evidence and environment details are attached to the release issue.
- [ ] A maintainer reviewed the evidence and published the draft.
- [ ] The AUR `call-ducker` package was updated from the released `PKGBUILD` and its `.SRCINFO`
      regenerated.
