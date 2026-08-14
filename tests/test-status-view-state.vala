private void disabled_status () {
    var state = StatusViewState.status (false, "", null, 0, null);
    assert (state.title == "CallDucker is off");
    assert (state.icon_name == "media-playback-stop-symbolic");
}

private void active_status () {
    var state = StatusViewState.status (true, "Call active", "Discord", 2, null);
    assert (state.title == "Call active");
    assert (state.subtitle.contains ("Discord"));
    assert (state.subtitle.contains ("2"));
}

private void notice_is_not_an_outage () {
    var listening = StatusViewState.status (true, "Listening", null, 0,
        "Restore identity is ambiguous");
    assert (listening.title == "Listening for calls");
    assert (listening.subtitle == "Restore identity is ambiguous");
    assert (listening.icon_name == "dialog-information-symbolic");

    var active = StatusViewState.status (true, "Call active", "Discord", 1,
        "Restore identity is ambiguous");
    assert (active.title == "Call active");
    assert (active.subtitle.contains ("Discord"));
    assert (active.subtitle.contains ("ambiguous"));

    var outage = StatusViewState.status (true, "Audio service unavailable", null, 0, "gone");
    assert (outage.title == "Audio service unavailable");
    assert (outage.subtitle == "gone");
}

private void preview_boundaries () {
    var unavailable = PreviewViewState.create (false, true, true, true,
        "Audio service unavailable", false);
    assert (!unavailable.sensitive);
    var available = PreviewViewState.create (false, true, true, true,
        "Listening", false);
    assert (available.sensitive);
    var active = PreviewViewState.create (true, false, true, true,
        "Audio service unavailable", false);
    assert (active.sensitive);
}

public static int main (string[] args) {
    Test.init (ref args);
    Test.add_func ("/view-state/disabled", disabled_status);
    Test.add_func ("/view-state/active", active_status);
    Test.add_func ("/view-state/notice", notice_is_not_an_outage);
    Test.add_func ("/view-state/preview-boundaries", preview_boundaries);
    return Test.run ();
}
