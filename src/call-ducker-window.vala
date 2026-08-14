using Gtk;
using Adw;

[GtkTemplate (ui = "/io/github/UntoastedToast/CallDucker/ui/call-ducker-window.ui")]
public class CallDuckerWindow : Adw.ApplicationWindow {
    [GtkChild] public unowned Adw.ToastOverlay toast_overlay;
    [GtkChild] public unowned Adw.NavigationView navigation;

    public CallDuckerMainPage main_page { get; private set; }
    public CallDuckerApplicationsPage applications_page { get; private set; }

    private GLib.Settings settings;
    private SettingsModel settings_model;
    private ApplicationsView applications_view;
    private bool syncing_enabled;
    private bool syncing_volume;
    private const uint MAX_RECONNECT_ATTEMPTS = 5;

    private bool preview_call_in_flight;
    private uint reconnect_source;
    private uint reconnect_attempts;
    private string? reconnect_message;
    private DBusProxy? daemon;
    private DaemonClient? daemon_client;

    public unowned GLib.Settings window_settings { get { return settings; } }

    public CallDuckerWindow (CallDuckerApp application) {
        Object (application: application);
        settings_model = new SettingsModel ();
        settings = settings_model.settings;
        main_page = new CallDuckerMainPage ();
        applications_page = new CallDuckerApplicationsPage ();
        applications_view = new ApplicationsView (applications_page, settings_model);
        navigation.add (main_page);
        navigation.add (applications_page);

        main_page.volume_adjustment.value = settings.get_double ("background-volume") * 100;
        settings.bind ("auto-detect-games", main_page.games_row, "active", SettingsBindFlags.DEFAULT);
        main_page.enabled_row.notify["active"].connect (enabled_changed);
        main_page.volume_adjustment.value_changed.connect (volume_changed);
        main_page.games_row.notify["active"].connect (() => refresh_preview ());
        main_page.preview_button.clicked.connect (() => toggle_preview.begin ());
        main_page.manage_row.activated.connect (() => navigation.push_by_tag ("applications"));
        settings.changed["background-volume"].connect (volume_setting_changed);
        applications_view.changed.connect (() => {
            reconnect_attempts = 0;
            reconnect_message = null;
            refresh_preview ();
        });
        applications_view.failed.connect ((error) => {
            if (is_transient_dbus_error (error)) handle_daemon_error (error);
            else show_error (friendly_error (error));
        });
        close_request.connect (() => {
            if (reconnect_source != 0) {
                Source.remove (reconnect_source);
                reconnect_source = 0;
            }
            return false;
        });
        connect_daemon.begin ();
        refresh_enabled ();
        refresh_preview ();
    }

    private void volume_changed () {
        if (syncing_volume) return;
        settings.set_double ("background-volume", main_page.volume_adjustment.value / 100.0);
        refresh_preview ();
    }

    private void volume_setting_changed () {
        double value = settings.get_double ("background-volume") * 100;
        if (main_page.volume_row.value != value) {
            syncing_volume = true;
            main_page.volume_row.value = value;
            syncing_volume = false;
        }
        refresh_preview ();
    }

    private async void connect_daemon () {
        try {
            daemon_client = yield DaemonClient.create ();
            daemon = daemon_client.proxy;
            daemon.g_properties_changed.connect ((changed, invalidated) => refresh_status ());
            daemon.g_signal.connect ((sender, signal_name, parameters) => {
                if (signal_name == "ApplicationsChanged") request_application_reload ();
            });
            daemon.notify["g-name-owner"].connect (daemon_owner_changed);
            if (daemon.get_name_owner () == null) {
                if (settings.get_boolean ("enabled")) {
                    set_connecting_status ();
                    schedule_daemon_reconnect ();
                } else show_disabled_status ();
                return;
            }
            refresh_status ();
            request_application_reload ();
        } catch (Error error) {
            handle_daemon_error (error);
        }
    }

    private bool is_transient_dbus_error (Error error) {
        return DaemonClient.is_transient_error (error);
    }

    private void handle_daemon_error (Error error) {
        if (settings.get_boolean ("enabled") && is_transient_dbus_error (error)) {
            reconnect_message = error.message;
            set_connecting_status ();
            schedule_daemon_reconnect ();
        } else if (settings.get_boolean ("enabled")) {
            set_status (_("Audio service unavailable"), error.message, "dialog-warning-symbolic");
        } else show_disabled_status ();
    }

    private void set_connecting_status () {
        set_status (_("Connecting to the audio service…"),
            _("CallDucker will retry automatically"), "emblem-synchronizing-symbolic");
        refresh_preview ();
    }

    private void schedule_daemon_reconnect () {
        if (!settings.get_boolean ("enabled") || reconnect_source != 0) return;
        /* A daemon that never answers is not restarting: it does not speak our
           contract. Stop retrying silently and report it. */
        if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
            set_status (_("Audio service unavailable"),
                reconnect_message ?? _("Check the system journal for details"),
                "dialog-warning-symbolic");
            refresh_preview ();
            return;
        }
        reconnect_attempts++;
        reconnect_source = Timeout.add (400, () => {
            reconnect_source = 0;
            connect_daemon.begin ();
            return Source.REMOVE;
        });
    }

    private void daemon_owner_changed () {
        if (daemon == null) return;
        if (daemon.get_name_owner () == null) {
            preview_call_in_flight = false;
            if (settings.get_boolean ("enabled")) {
                set_connecting_status ();
                schedule_daemon_reconnect ();
            }
            return;
        }
        refresh_status ();
        request_application_reload ();
    }

    private void request_application_reload () {
        if (!applications_view.request (daemon_client)) {
            schedule_daemon_reconnect ();
        }
    }

    private bool preview_active () {
        if (daemon == null || daemon.get_name_owner () == null) return false;
        Variant? value = daemon.get_cached_property ("PreviewActive");
        return value != null && value.get_boolean ();
    }

    private bool has_adjustable_target () {
        return applications_view.has_adjustable_target ();
    }

    private void refresh_preview () {
        bool active = preview_active ();
        Variant? state_value = daemon != null ? daemon.get_cached_property ("State") : null;
        string state = state_value != null ? state_value.get_string () : "";
        var view_state = PreviewViewState.create (active, has_adjustable_target (),
            settings.get_boolean ("enabled"),
            daemon != null && daemon.get_name_owner () != null, state, preview_call_in_flight);
        main_page.preview_button.label = view_state.label;
        main_page.preview_row.subtitle = view_state.subtitle;
        main_page.preview_button.sensitive = view_state.sensitive;
    }

    private void refresh_status () {
        if (!settings.get_boolean ("enabled")) {
            show_disabled_status ();
            return;
        }
        if (daemon == null) return;
        Variant? state_value = daemon.get_cached_property ("State");
        string state = state_value != null ? state_value.get_string () : "";
        Variant? triggers = daemon.get_cached_property ("ActiveTriggerNames");
        Variant? ducked = daemon.get_cached_property ("DuckedTargetNames");
        Variant? error = daemon.get_cached_property ("LastError");
        var view_state = StatusViewState.status (true, state,
            triggers != null && triggers.n_children () > 0
                ? triggers.get_child_value (0).get_string () : null,
            ducked != null ? (uint) ducked.n_children () : 0,
            error != null ? error.get_string () : null);
        set_status (view_state.title, view_state.subtitle, view_state.icon_name);
        refresh_preview ();
    }

    private void set_status (string title, string subtitle, string icon_name) {
        main_page.status_row.title = title;
        main_page.status_row.subtitle = subtitle;
        main_page.status_icon.icon_name = icon_name;
    }

    private void show_disabled_status () {
        set_status (_("CallDucker is off"),
            _("Turn on automatic volume adjustment to start listening"),
            "media-playback-stop-symbolic");
        refresh_preview ();
    }

    private void show_error (string message) {
        toast_overlay.add_toast (new Adw.Toast (message));
    }

    private string friendly_error (Error error) {
        if (error.message.contains ("PreviewUnavailable")) return _("Preview is not available right now.");
        return error.message;
    }

    private async void toggle_preview () {
        if (daemon == null || daemon.get_name_owner () == null) {
            show_error (_("The audio service is reconnecting. Try again in a moment."));
            schedule_daemon_reconnect ();
            return;
        }
        preview_call_in_flight = true;
        refresh_preview ();
        try {
            string method = preview_active () ? "CancelPreview" : "Preview";
            Variant? parameters = method == "Preview" ? new Variant ("(u)", 5000) : null;
            yield daemon.call (method, parameters, DBusCallFlags.NONE, -1, null);
        } catch (Error error) {
            if (is_transient_dbus_error (error)) handle_daemon_error (error);
            else show_error (friendly_error (error));
        } finally {
            preview_call_in_flight = false;
            refresh_preview ();
        }
    }

    private void refresh_enabled () {
        bool enabled = settings.get_boolean ("enabled");
        syncing_enabled = true;
        main_page.enabled_row.active = enabled;
        syncing_enabled = false;
        main_page.ducking_group.sensitive = enabled;
        main_page.call_apps_group.sensitive = enabled;
        main_page.target_apps_group.sensitive = enabled;
        applications_page.dynamic_calls_group.sensitive = enabled;
        applications_page.dynamic_targets_group.sensitive = enabled;
        if (!enabled) show_disabled_status ();
    }

    private void enabled_changed () {
        if (syncing_enabled) return;
        bool enable = main_page.enabled_row.active;
        reconnect_attempts = 0;
        reconnect_message = null;
        settings.set_boolean ("enabled", enable);
        settings.set_boolean ("onboarding-complete", true);
        main_page.ducking_group.sensitive = enable;
        main_page.call_apps_group.sensitive = enable;
        main_page.target_apps_group.sensitive = enable;
        applications_page.dynamic_calls_group.sensitive = enable;
        applications_page.dynamic_targets_group.sensitive = enable;
        if (enable) set_status (_("Starting CallDucker…"), _("Connecting to the audio service"), "emblem-synchronizing-symbolic");
        else show_disabled_status ();
        if (!enable && reconnect_source != 0) {
            Source.remove (reconnect_source);
            reconnect_source = 0;
        }
        systemd_toggle.begin (enable);
    }

    private async void systemd_toggle (bool enable) {
        try {
            yield ServiceManager.set_enabled (enable);
            if (enable) {
                daemon = null;
                daemon_client = null;
                connect_daemon.begin ();
            } else {
                daemon = null;
                daemon_client = null;
            }
        } catch (Error error) {
            settings.set_boolean ("enabled", !enable);
            syncing_enabled = true;
            main_page.enabled_row.active = !enable;
            syncing_enabled = false;
            refresh_enabled ();
            set_status (_("Could not change service state"), error.message, "dialog-warning-symbolic");
            show_error (error.message);
        }
    }
}
