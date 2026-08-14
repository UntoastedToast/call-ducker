public class DaemonClient : Object {
    public const string BUS_NAME = "io.github.UntoastedToast.CallDucker.Daemon";
    public const string OBJECT_PATH = "/io/github/UntoastedToast/CallDucker/Daemon";
    public const string INTERFACE_NAME = "io.github.UntoastedToast.CallDucker.Daemon1";

    public DBusProxy proxy { get; private set; }

    private DaemonClient (DBusProxy proxy) {
        this.proxy = proxy;
    }

    public static DaemonClient connect_sync () throws Error {
        var proxy = new DBusProxy.for_bus_sync (BusType.SESSION, DBusProxyFlags.NONE, null,
            BUS_NAME, OBJECT_PATH, INTERFACE_NAME);
        return new DaemonClient (proxy);
    }

    public static async DaemonClient create () throws Error {
        var proxy = yield new DBusProxy.for_bus (BusType.SESSION, DBusProxyFlags.NONE, null,
            BUS_NAME, OBJECT_PATH, INTERFACE_NAME);
        return new DaemonClient (proxy);
    }

    public bool is_connected () {
        return proxy.get_name_owner () != null;
    }

    public Variant property_sync (string name) throws Error {
        Variant reply = proxy.call_sync ("org.freedesktop.DBus.Properties.Get",
            new Variant ("(ss)", INTERFACE_NAME, name), DBusCallFlags.NONE, -1, null);
        return reply.get_child_value (0).get_variant ();
    }

    public Variant call_sync (string method, Variant? parameters = null) throws Error {
        return proxy.call_sync (method, parameters, DBusCallFlags.NONE, -1, null);
    }

    public async Variant call (string method, Variant? parameters = null) throws Error {
        return yield proxy.call (method, parameters, DBusCallFlags.NONE, -1, null);
    }

    public Variant? cached_property (string name) {
        return proxy.get_cached_property (name);
    }

    /* The daemon can own its bus name for a moment before or after its object
       is exported, so a missing object is a restart, not a broken contract. */
    public static bool is_transient_error (Error error) {
        return error.message.contains ("ServiceUnknown") ||
            error.message.contains ("NameHasNoOwner") ||
            error.message.contains ("not activatable") ||
            error.message.contains ("UnknownMethod") ||
            error.message.contains ("Object does not exist at path");
    }
}
