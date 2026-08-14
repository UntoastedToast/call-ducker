public class ApplicationInfo : Object {
    public string id { get; private set; }
    public string name { get; private set; }
    public string icon { get; private set; }
    public bool input { get; private set; }
    public bool game { get; private set; }
    public bool available { get; private set; }

    public ApplicationInfo (string id, string name, string icon, bool input, bool game, bool available) {
        this.id = id;
        this.name = name != "" ? name : id;
        this.icon = icon != "" ? icon : "application-x-executable-symbolic";
        this.input = input;
        this.game = game;
        this.available = available;
    }

    public string key () {
        return (input ? "i|" : "o|") + id;
    }

    public bool matches (string query) {
        string needle = query.casefold ();
        return needle == "" || name.casefold ().contains (needle) || id.casefold ().contains (needle);
    }
}

/* Owns application snapshot semantics; the window only renders this catalog. */
public class ApplicationCatalog : Object {
    private GLib.HashTable<string, ApplicationInfo> entries =
        new GLib.HashTable<string, ApplicationInfo> (str_hash, str_equal);

    private static bool contains (string[] values, string id) {
        foreach (string value in values) if (value == id) return true;
        return false;
    }

    public void reconcile (Variant applications, string[] calls, string[] always, string[] never) {
        var next = new GLib.HashTable<string, ApplicationInfo> (str_hash, str_equal);
        for (int i = 0; i < applications.n_children (); i++) {
            Variant app = applications.get_child_value (i);
            string id = "", name = "", icon = "";
            bool input = false, game = false, available = false;
            app.lookup ("id", "s", out id);
            app.lookup ("name", "s", out name);
            app.lookup ("icon", "s", out icon);
            app.lookup ("input", "b", out input);
            app.lookup ("game", "b", out game);
            app.lookup ("available", "b", out available);
            bool configured = input ? contains (calls, id) : contains (always, id) || contains (never, id);
            if (id == "" || (input && game) || (!available && !configured)) continue;
            var info = new ApplicationInfo (id, name, icon, input, game, available);
            next.insert (info.key (), info);
        }
        entries = next;
    }

    private static int compare (ApplicationInfo a, ApplicationInfo b) {
        if (a.available != b.available) return a.available ? -1 : 1;
        int by_name = a.name.collate (b.name);
        if (by_name != 0) return by_name;
        return a.id.collate (b.id);
    }

    public ApplicationInfo[] visible (bool input, string query = "") {
        ApplicationInfo[] result = {};
        var iterator = GLib.HashTableIter<string, ApplicationInfo> (entries);
        unowned string key;
        unowned ApplicationInfo info;
        while (iterator.next (out key, out info))
            if (info.input == input && info.matches (query)) result += info;
        /* Tiny lists are expected; insertion sort keeps this GLib-only and deterministic. */
        for (int i = 1; i < result.length; i++) {
            ApplicationInfo value = result[i];
            int j = i - 1;
            while (j >= 0 && compare (value, result[j]) < 0) {
                result[j + 1] = result[j];
                j--;
            }
            result[j + 1] = value;
        }
        return result;
    }

    public bool has_duplicate_name (ApplicationInfo item) {
        var iterator = GLib.HashTableIter<string, ApplicationInfo> (entries);
        unowned string key;
        unowned ApplicationInfo other;
        while (iterator.next (out key, out other))
            if (other.input == item.input && other.id != item.id && other.name == item.name) return true;
        return false;
    }
}

/* Owns asynchronous snapshot loading and coalesces bursts of change signals. */
public class ApplicationStore : ApplicationCatalog {
    private bool load_in_flight;
    private bool reload_pending;
    private Variant? snapshot;

    public signal void changed ();
    public signal void failed (Error error);

    public void request (DaemonClient client, SettingsModel settings) {
        if (load_in_flight) {
            reload_pending = true;
            return;
        }
        load.begin (client, settings);
    }

    public void refresh_rules (SettingsModel settings) {
        if (snapshot == null) return;
        reconcile (snapshot, settings.settings.get_strv ("call-app-selectors"),
            settings.settings.get_strv ("always-duck-selectors"),
            settings.settings.get_strv ("never-duck-selectors"));
        changed ();
    }

    private async void load (DaemonClient client, SettingsModel settings) {
        load_in_flight = true;
        try {
            Variant reply = yield client.call ("ListApplications");
            snapshot = reply.get_child_value (0);
            reconcile (snapshot, settings.settings.get_strv ("call-app-selectors"),
                settings.settings.get_strv ("always-duck-selectors"),
                settings.settings.get_strv ("never-duck-selectors"));
            changed ();
        } catch (Error error) {
            failed (error);
        } finally {
            load_in_flight = false;
            if (reload_pending) {
                reload_pending = false;
                request (client, settings);
            }
        }
    }
}
