using Gtk;
using Adw;

public class CallDuckerApp : Adw.Application {
    private CallDuckerWindow? window;

    public CallDuckerApp (ApplicationFlags flags = ApplicationFlags.DEFAULT_FLAGS) {
        Object (application_id: "io.github.UntoastedToast.CallDucker",
                flags: flags);
        var about_action = new GLib.SimpleAction ("about", null);
        about_action.activate.connect (() => show_about ());
        add_action (about_action);
    }

    private void show_about () {
        var about = new Adw.AboutDialog ();
        about.application_name = _("CallDucker");
        about.application_icon = "io.github.UntoastedToast.CallDucker";
        about.developer_name = "UntoastedToast";
        about.version = CallDucker.VERSION;
        about.comments = _("Adjust game audio automatically during calls");
        about.website = "https://github.com/UntoastedToast/call-ducker";
        about.issue_url = "https://github.com/UntoastedToast/call-ducker/issues";
        about.copyright = "© 2026 UntoastedToast";
        about.license_type = Gtk.License.GPL_3_0;
        about.present (active_window);
    }

    protected override void activate () {
        if (window == null) window = new CallDuckerWindow (this);
        window.present ();
    }

}
