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
- [ ] The draft contains one Fedora binary RPM, one source RPM, one source archive, and checksums.
- [ ] Attestations and `sha256sum --check SHA256SUMS` validate every artifact.
- [ ] The Fedora RPM installs cleanly and `call-ducker.service` starts without errors.

## Automated and source-build coverage

- [ ] Fedora 43, Arch, Debian 13, and Ubuntu 24.04 CI builds and tests passed.
- [ ] Linux Mint 22 was built from source and smoke-tested in Cinnamon.
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
