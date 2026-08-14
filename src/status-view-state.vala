public class StatusViewState : Object {
    public string title { get; private set; }
    public string subtitle { get; private set; }
    public string icon_name { get; private set; }

    public StatusViewState (string title, string subtitle, string icon_name) {
        this.title = title;
        this.subtitle = subtitle;
        this.icon_name = icon_name;
    }

    public static StatusViewState status (bool enabled, string state, string? trigger,
                                          uint ducked_count, string? error) {
        if (!enabled || state == "Disabled")
            return new StatusViewState (_("CallDucker is off"),
                _("Turn on automatic volume adjustment to start listening"),
                "media-playback-stop-symbolic");
        /* A message while the service runs is a notice about a single stream
           group, not an outage. It replaces the subtitle, never the title. */
        bool notice = error != null && error != "";
        if (state == "Listening")
            return new StatusViewState (_("Listening for calls"),
                notice ? error : _("No active call detected"),
                notice ? "dialog-information-symbolic" : "audio-input-microphone-symbolic");
        if (state == "Call active") {
            string[] details = {};
            if (trigger != null && trigger != "")
                details += _("Call: %s").printf (trigger);
            if (ducked_count > 0)
                details += _("Adjusted: %u application(s)").printf (ducked_count);
            if (notice)
                details += error;
            return new StatusViewState (_("Call active"), details.length == 0
                ? _("Applying the target volume") : string.joinv (" · ", details),
                notice ? "dialog-information-symbolic" : "audio-volume-low-symbolic");
        }
        return new StatusViewState (_("Audio service unavailable"),
            error != null && error != "" ? error : _("Check the system journal for details"),
            "dialog-warning-symbolic");
    }
}

public class PreviewViewState : Object {
    public string label { get; private set; }
    public string subtitle { get; private set; }
    public bool sensitive { get; private set; }

    public PreviewViewState (string label, string subtitle, bool sensitive) {
        this.label = label;
        this.subtitle = subtitle;
        this.sensitive = sensitive;
    }

    public static PreviewViewState create (bool active, bool has_target, bool enabled, bool connected,
                                           string daemon_state, bool in_flight) {
        string subtitle;
        if (active)
            subtitle = _("The selected volume is currently applied");
        else if (!has_target)
            subtitle = _("Start an application whose volume can be adjusted to preview it");
        else
            subtitle = _("Temporarily apply the selected volume");
        bool sensitive = !in_flight && enabled && connected &&
            (active || (daemon_state == "Listening" && has_target));
        return new PreviewViewState (active ? _("Stop Preview") : _("Preview"),
            subtitle, sensitive);
    }
}
