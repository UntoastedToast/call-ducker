using Gtk;
using Adw;

public class ApplicationsView : Object {
    private CallDuckerApplicationsPage page;
    private GLib.Settings settings;
    private SettingsModel settings_model;
    private ApplicationStore catalog = new ApplicationStore ();
    private Gtk.Widget[] call_widgets = {};
    private Gtk.Widget[] target_widgets = {};

    public signal void changed ();
    public signal void failed (Error error);

    public ApplicationsView (CallDuckerApplicationsPage page, SettingsModel settings_model) {
        this.page = page;
        this.settings_model = settings_model;
        this.settings = settings_model.settings;
        page.search_requested.connect (render);
        catalog.changed.connect (() => {
            render ();
            changed ();
        });
        catalog.failed.connect ((error) => failed (error));
    }

    public bool request (DaemonClient? client) {
        if (client == null || !client.is_connected ()) return false;
        catalog.request (client, settings_model);
        return true;
    }

    public bool has_adjustable_target () {
        string[] always = settings.get_strv ("always-duck-selectors");
        string[] never = settings.get_strv ("never-duck-selectors");
        bool automatic_games = settings.get_boolean ("auto-detect-games");
        foreach (ApplicationInfo info in catalog.visible (false)) {
            if (!info.available || SettingsModel.contains (never, info.id)) continue;
            if (SettingsModel.contains (always, info.id)) return true;
            if (automatic_games && info.game) return true;
        }
        return false;
    }

    private void refresh_rules () {
        catalog.refresh_rules (settings_model);
    }

    private void set_policy (string selector, uint selected) {
        settings_model.set_target_policy (selector, selected);
        refresh_rules ();
    }

    private void set_call_selector (string selector, bool active) {
        settings_model.set_call_selector (selector, active);
        refresh_rules ();
    }

    private string application_subtitle (ApplicationInfo info) {
        string availability = info.available ? _("Available now") : _("Not currently available");
        return catalog.has_duplicate_name (info) ? "%s · %s".printf (availability, info.id) : availability;
    }

    private void clear_rows (Adw.PreferencesGroup group, ref Gtk.Widget[] widgets) {
        foreach (Gtk.Widget widget in widgets) group.remove (widget);
        widgets = {};
    }

    private void render () {
        clear_rows (page.dynamic_calls_group, ref call_widgets);
        clear_rows (page.dynamic_targets_group, ref target_widgets);
        string query = page.search_entry.text;
        ApplicationInfo[] calls = catalog.visible (true, query);
        ApplicationInfo[] targets = catalog.visible (false, query);
        foreach (ApplicationInfo info in calls) {
            var row = new Adw.SwitchRow ();
            row.title = info.name;
            row.subtitle = application_subtitle (info);
            row.add_prefix (new Gtk.Image.from_icon_name (info.icon));
            row.active = SettingsModel.contains (settings.get_strv ("call-app-selectors"), info.id);
            string selector = info.id;
            row.notify["active"].connect (() => set_call_selector (selector, row.active));
            page.dynamic_calls_group.add (row);
            call_widgets += row;
        }
        foreach (ApplicationInfo info in targets) {
            var row = new Adw.ComboRow ();
            row.title = info.name;
            row.subtitle = application_subtitle (info);
            row.add_prefix (new Gtk.Image.from_icon_name (info.icon));
            row.model = new Gtk.StringList ({ _("Automatic"), _("Set target during calls"), _("Never adjust") });
            if (SettingsModel.contains (settings.get_strv ("never-duck-selectors"), info.id)) row.selected = 2;
            else if (SettingsModel.contains (settings.get_strv ("always-duck-selectors"), info.id)) row.selected = 1;
            string selector = info.id;
            row.notify["selected"].connect (() => set_policy (selector, row.selected));
            page.dynamic_targets_group.add (row);
            target_widgets += row;
        }
        bool empty = calls.length + targets.length == 0;
        page.empty_group.visible = empty;
        page.dynamic_calls_group.visible = calls.length > 0;
        page.dynamic_targets_group.visible = targets.length > 0;
        if (empty) {
            bool searching = query.strip () != "";
            page.empty_row.title = searching ? _("No matching applications") : _("No applications available");
            page.empty_row.subtitle = searching ? _("Try a different search.") :
                _("Start an application that uses a microphone or plays audio.");
        }
    }
}
