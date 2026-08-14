public class ServiceManager : Object {
    private const string BUS_NAME = "org.freedesktop.systemd1";
    private const string OBJECT_PATH = "/org/freedesktop/systemd1";
    private const string INTERFACE_NAME = "org.freedesktop.systemd1.Manager";
    private const string UNIT = "call-ducker.service";

    private static DBusProxy manager_sync () throws Error {
        return new DBusProxy.for_bus_sync (BusType.SESSION, DBusProxyFlags.NONE, null,
            BUS_NAME, OBJECT_PATH, INTERFACE_NAME);
    }

    private static async DBusProxy manager () throws Error {
        return yield new DBusProxy.for_bus (BusType.SESSION, DBusProxyFlags.NONE, null,
            BUS_NAME, OBJECT_PATH, INTERFACE_NAME);
    }

    public static void set_enabled_sync (bool enabled) throws Error {
        DBusProxy proxy = manager_sync ();
        string[] units = { UNIT };
        if (enabled) {
            proxy.call_sync ("EnableUnitFiles", new Variant ("(^asbb)", units, false, true),
                DBusCallFlags.NONE, -1, null);
            try {
                proxy.call_sync ("StartUnit", new Variant ("(ss)", UNIT, "replace"),
                    DBusCallFlags.NONE, -1, null);
            } catch (Error error) {
                try {
                    proxy.call_sync ("DisableUnitFiles", new Variant ("(^asb)", units, false),
                        DBusCallFlags.NONE, -1, null);
                } catch (Error rollback_error) {
                    warning ("Could not roll back service enable: %s", rollback_error.message);
                }
                throw error;
            }
        } else {
            proxy.call_sync ("StopUnit", new Variant ("(ss)", UNIT, "replace"),
                DBusCallFlags.NONE, -1, null);
            try {
                proxy.call_sync ("DisableUnitFiles", new Variant ("(^asb)", units, false),
                    DBusCallFlags.NONE, -1, null);
            } catch (Error error) {
                try {
                    proxy.call_sync ("StartUnit", new Variant ("(ss)", UNIT, "replace"),
                        DBusCallFlags.NONE, -1, null);
                } catch (Error rollback_error) {
                    warning ("Could not roll back service stop: %s", rollback_error.message);
                }
                throw error;
            }
        }
    }

    public static async void set_enabled (bool enabled) throws Error {
        DBusProxy proxy = yield manager ();
        string[] units = { UNIT };
        if (enabled) {
            yield proxy.call ("EnableUnitFiles", new Variant ("(^asbb)", units, false, true),
                DBusCallFlags.NONE, -1, null);
            try {
                yield proxy.call ("StartUnit", new Variant ("(ss)", UNIT, "replace"),
                    DBusCallFlags.NONE, -1, null);
            } catch (Error error) {
                try {
                    yield proxy.call ("DisableUnitFiles", new Variant ("(^asb)", units, false),
                        DBusCallFlags.NONE, -1, null);
                } catch (Error rollback_error) {
                    warning ("Could not roll back service enable: %s", rollback_error.message);
                }
                throw error;
            }
        } else {
            yield proxy.call ("StopUnit", new Variant ("(ss)", UNIT, "replace"),
                DBusCallFlags.NONE, -1, null);
            try {
                yield proxy.call ("DisableUnitFiles", new Variant ("(^asb)", units, false),
                    DBusCallFlags.NONE, -1, null);
            } catch (Error error) {
                try {
                    yield proxy.call ("StartUnit", new Variant ("(ss)", UNIT, "replace"),
                        DBusCallFlags.NONE, -1, null);
                } catch (Error rollback_error) {
                    warning ("Could not roll back service stop: %s", rollback_error.message);
                }
                throw error;
            }
        }
    }
}
