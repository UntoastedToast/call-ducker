# Architecture

CallDucker installs a session-bus daemon, a GTK/libadwaita client, and a command-line client. The
D-Bus XML remains the stable boundary between them; version 0.1.1 defines the method, property,
or signal signatures.

## Audio backend

`CdAudioBackend` exposes immutable stream snapshots and asynchronous sink-input volume changes.
`CdPulseBackend` implements it with `libpulse` and `libpulse-mainloop-glib`. Sink inputs represent
playback and source outputs represent recording. A source output is active when it is not corked;
playback with media role `phone` or `communication` can also trigger a call.

Stream identity prefers the portal app ID, then `application.id`, process binary, and application
name. Subscription bursts are debounced for 50 ms. Subscription acknowledgement and complete,
successful sink-input and source-output queries form one snapshot transaction; a failed query keeps
the last valid snapshot and reconnects. Reconnect delays grow from one to 30 seconds and reset only
after a subscribed, complete snapshot. Before changing a sink input, the backend queries that index
again and verifies its expected stable identity, restore ID, writable volume, and channel layout.
The same code operates against PulseAudio and `pipewire-pulse` and never starts audio CLI tools.

## Absolute volume reconciliation

`CdVolumeController` serializes asynchronous work in generation order, gives restoration priority,
and completes superseded requests with a defined error. A reconcile compares every current volume
with its absolute desired value, so an external mixer change converges again on the next snapshot.
New originals are saved together before the first set operation; successful restore batches are
removed together only after their acknowledgements and a durable journal update. Stream identity is
checked again immediately before dispatch. Every target change is calculated from the captured
original, never from the currently applied target value.

The target is `background-volume × PA_VOLUME_NORM`. Originals are first remapped from their stored
PulseAudio channel map to the current stream map, then `pa_cvolume_scale()` retains channel balance;
an entirely silent original receives the target on every channel when the target is above zero.
An invalid remap leaves the journal entry pending. The backend changes only volume and never changes
the mute flag. Therefore 0% is volume zero and 100% is exactly PulseAudio's normal volume, even if
the original was quieter or louder.

## Restore journal

`pulse-restore.json` schema version 1 stores selector, PulseAudio index,
`module-stream-restore.id`, display name, raw channel values, and stable PulseAudio channel names for
every playback stream. It is replaced with GLib's consistent and durable file-write mode and file
mode `0600`. Loading validates the complete document into temporary state, including types, ranges,
channel counts, positions, and duplicate identities. An unknown version or corrupt document is left
untouched and blocks volume adjustment for that daemon lifetime.

An index is remapped and durably journaled only when a non-empty selector/restore-ID pair identifies
exactly one journal entry and one current stream globally. Ambiguous streams are neither restored nor
captured as new originals. Missing, ambiguous, and failed restores remain pending for a later
snapshot or reconnect, but do not delay a completed request or shutdown when no current stream can
be resolved.

## Daemon and D-Bus completion

Every snapshot selects exactly one controller mode: active-call target, active Preview target, or
restore. Only an actual call-to-no-call transition receives the 750 ms grace period. Preview remains
active across snapshots, includes newly appearing targets, and starts its duration only after its
journal and volume operations complete. Disable, CancelPreview, Restore, and shutdown request an
immediate restore. D-Bus methods reply after their resolvable controller work and journal update;
partial Preview failure rolls back already changed streams.

The process requests its session-bus name with `DO_NOT_QUEUE`. It creates PulseAudio, controller,
and journal state only from the name-acquired callback, so a second instance exits without audio or
filesystem side effects. Losing an acquired name follows the asynchronous restore path with a
five-second shutdown guard.

## Tests

Unit tests cover convergence, delayed and superseded operations, index reuse, multichannel remaps,
ambiguous restore IDs, fail-closed journals, D-Bus completion, and the complete published contract.
The backend test starts a private PulseAudio server with a null sink and exercises playback,
recording, subscriptions, identity validation, mute preservation, and reconnect. CI forces these and
the Xvfb UI test on Fedora 43, Arch, Debian 13, and Ubuntu 24.04, including ASan/UBSan. CI also
builds the Fedora RPM, both Debian-family packages, and the Arch package from the same source
archive on every pull request. Before release, Linux Mint 22 installs the Ubuntu 24.04 package for a
Cinnamon smoke test; a Fedora live session additionally tests `pipewire-pulse` and desktop mixer
values.
