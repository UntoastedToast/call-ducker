private string helper_path;

private bool wait_until (owned SourceFunc condition, int timeout_ms = 3000) {
    int64 deadline = GLib.get_monotonic_time () + timeout_ms * 1000;
    while (GLib.get_monotonic_time () < deadline) {
        while (MainContext.default ().iteration (false)) {}
        if (condition ()) return true;
        Thread.usleep (10000);
    }
    return condition ();
}

private Subprocess start_helper () throws Error {
    return new Subprocess.newv ({ helper_path }, SubprocessFlags.NONE);
}

private void client_tracks_service_lifecycle () {
    var bus = new TestDBus (TestDBusFlags.NONE);
    DaemonClient? client = null;
    bus.up ();
    Test.message ("test bus started");
    try {
        Subprocess helper = start_helper ();
        Test.message ("helper started");
        client = DaemonClient.connect_sync ();
        Test.message ("client created");
        assert (wait_until (() => client.is_connected ()));
        Test.message ("owner appeared");
        assert (client.property_sync ("State").get_string () == "Listening");
        Test.message ("property read");
        Variant listed = client.call_sync ("ListApplications");
        assert (listed.get_child_value (0).n_children () == 1);
        Test.message ("applications listed");

        uint signals = 0;
        client.proxy.g_signal.connect ((sender, name, parameters) => {
            if (name == "ApplicationsChanged") signals++;
        });
        client.call_sync ("Preview", new Variant ("(u)", 250));
        Test.message ("preview called");
        assert (client.property_sync ("PreviewActive").get_boolean ());
        assert (wait_until (() => signals == 1));
        Test.message ("signal received");
        client.call_sync ("CancelPreview");
        assert (!client.property_sync ("PreviewActive").get_boolean ());
        client.call_sync ("Restore");
        Test.message ("restore called");

        helper.force_exit ();
        helper.wait ();
        Test.message ("helper stopped");
        assert (wait_until (() => !client.is_connected ()));
        Test.message ("owner vanished");

        helper = start_helper ();
        assert (wait_until (() => client.is_connected ()));
        Test.message ("owner returned");
        assert (client.property_sync ("State").get_string () == "Listening");
        Test.message ("reconnected property read");
        helper.force_exit ();
        helper.wait ();
        Test.message ("second helper stopped");
        client.proxy.get_connection ().close_sync ();
        client = null;
        while (MainContext.default ().iteration (false)) {}
    } catch (Error error) {
        Test.message ("%s", error.message);
        Test.fail ();
    } finally {
        Test.message ("stopping test bus");
        bus.stop ();
        Test.message ("test bus stopped");
    }
}

/* A daemon owns its bus name slightly before and after its object exists.
   Calls in those windows must read as a restart, not as a broken contract. */
private void missing_object_is_transient () {
    var bus = new TestDBus (TestDBusFlags.NONE);
    bus.up ();
    try {
        Subprocess helper = start_helper ();
        var client = DaemonClient.connect_sync ();
        assert (wait_until (() => client.is_connected ()));

        var stray = new DBusProxy.for_bus_sync (BusType.SESSION, DBusProxyFlags.NONE, null,
            DaemonClient.BUS_NAME, "/io/github/UntoastedToast/CallDucker/Absent",
            DaemonClient.INTERFACE_NAME);
        try {
            stray.call_sync ("ListApplications", null, DBusCallFlags.NONE, -1, null);
            Test.fail ();
        } catch (Error error) {
            assert (error.message.contains ("UnknownMethod"));
            assert (DaemonClient.is_transient_error (error));
        }

        helper.force_exit ();
        helper.wait ();
    } catch (Error error) {
        Test.message ("%s", error.message);
        Test.fail ();
    } finally {
        bus.stop ();
    }
}

int main (string[] args) {
    Test.init (ref args);
    helper_path = args[1];
    Test.add_func ("/daemon-client/lifecycle", client_tracks_service_lifecycle);
    Test.add_func ("/daemon-client/missing-object", missing_object_is_transient);
    return Test.run ();
}
