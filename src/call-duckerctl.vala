private void set_enabled (bool enabled) throws Error {
    ServiceManager.set_enabled_sync (enabled);
    var settings = new Settings ("io.github.UntoastedToast.CallDucker");
    settings.set_boolean ("enabled", enabled);
}

private void print_json_status (string state, uint pending) {
    var builder = new Json.Builder ();
    builder.begin_object ();
    builder.set_member_name ("state");
    builder.add_string_value (state);
    builder.set_member_name ("pendingRestorations");
    builder.add_int_value (pending);
    builder.end_object ();
    var generator = new Json.Generator ();
    generator.set_root (builder.get_root ());
    stdout.printf ("%s\n", generator.to_data (null));
}

public static int main (string[] args) {
    if (args.length < 2) {
        stderr.printf ("Usage: call-duckerctl COMMAND\n");
        return 2;
    }
    try {
        switch (args[1]) {
        case "status":
            var client = DaemonClient.connect_sync ();
            string state = client.property_sync ("State").get_string ();
            if (args.length > 2 && args[2] == "--json")
                print_json_status (state,
                    client.property_sync ("PendingRestorations").get_uint32 ());
            else
                stdout.printf ("State: %s\n", state);
            break;
        case "list-apps":
            Variant apps = DaemonClient.connect_sync ().call_sync ("ListApplications")
                .get_child_value (0);
            for (int i = 0; i < apps.n_children (); i++) {
                Variant app = apps.get_child_value (i);
                string name = "";
                string id = "";
                app.lookup ("name", "s", out name);
                app.lookup ("id", "s", out id);
                stdout.printf ("%s\t%s\n", id, name);
            }
            break;
        case "preview":
            DaemonClient.connect_sync ().call_sync ("Preview", new Variant ("(u)", 5000));
            break;
        case "restore":
            DaemonClient.connect_sync ().call_sync ("Restore");
            break;
        case "enable":
            set_enabled (true);
            break;
        case "disable":
            set_enabled (false);
            break;
        default:
            stderr.printf ("Unknown command: %s\n", args[1]);
            return 2;
        }
    } catch (Error error) {
        stderr.printf ("call-duckerctl: %s\n", error.message);
        return 1;
    }
    return 0;
}
