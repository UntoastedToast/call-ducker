using Gtk;
using Adw;

private bool wait_for (SourceFunc condition, int timeout_ms = 500) {
    int64 deadline = GLib.get_monotonic_time () + timeout_ms * 1000;
    while (GLib.get_monotonic_time () < deadline) {
        while (MainContext.default ().iteration (false)) {}
        if (condition ()) return true;
        Thread.usleep (10000);
    }
    return condition ();
}

private void templates_and_bindings () {
    // Never become a remote instance of a CallDucker process running in the
    // desktop session. A remote Gtk.Application has not run GTK's local
    // startup handler and cannot own application windows.
    var application = new CallDuckerApp (ApplicationFlags.NON_UNIQUE);
    try {
        assert (application.register (null));
    } catch (Error error) {
        Test.message ("%s", error.message);
        Test.fail ();
        return;
    }
    var window = new CallDuckerWindow (application);
    assert (window.toast_overlay != null);
    assert (window.navigation != null);
    assert (window.main_page.tag == "main");
    assert (window.applications_page.tag == "applications");
    assert (window.main_page.enabled_row != null);
    assert (window.main_page.status_icon != null);
    assert (window.main_page.preview_button != null);
    assert (window.applications_page.search_entry != null);
    assert (window.main_page.menu_button.menu_model != null);
    Variant? action = window.main_page.menu_button.menu_model.get_item_attribute_value (
        0, Menu.ATTRIBUTE_ACTION, VariantType.STRING);
    assert (action != null && action.get_string () == "app.about");

    window.window_settings.set_boolean ("auto-detect-games", false);
    while (MainContext.default ().iteration (false)) {}
    assert (!window.main_page.games_row.active);
    window.window_settings.set_boolean ("auto-detect-games", true);
    while (MainContext.default ().iteration (false)) {}
    assert (window.main_page.games_row.active);

    uint searches = 0;
    window.applications_page.search_requested.connect (() => searches++);
    window.applications_page.search_entry.text = "game";
    assert (wait_for (() => searches > 0));

    var dynamic_row = new Adw.SwitchRow ();
    dynamic_row.title = "Dynamic";
    window.applications_page.dynamic_calls_group.add (dynamic_row);
    assert (dynamic_row.get_ancestor (typeof (Adw.PreferencesGroup)) ==
        window.applications_page.dynamic_calls_group);
    window.applications_page.dynamic_calls_group.remove (dynamic_row);
    window.destroy ();
}

int main (string[] args) {
    Test.init (ref args);
    Adw.init ();
    Test.add_func ("/ui/templates-bindings", templates_and_bindings);
    return Test.run ();
}
