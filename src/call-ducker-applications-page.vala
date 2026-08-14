using Gtk;
using Adw;

[GtkTemplate (ui = "/io/github/UntoastedToast/CallDucker/ui/call-ducker-applications-page.ui")]
public class CallDuckerApplicationsPage : Adw.NavigationPage {
    [GtkChild] public unowned Gtk.SearchEntry search_entry;
    [GtkChild] public unowned Adw.PreferencesGroup dynamic_calls_group;
    [GtkChild] public unowned Adw.PreferencesGroup dynamic_targets_group;
    [GtkChild] public unowned Adw.PreferencesGroup empty_group;
    [GtkChild] public unowned Adw.ActionRow empty_row;

    public signal void search_requested ();

    construct {
        search_entry.search_changed.connect (() => search_requested ());
    }
}
