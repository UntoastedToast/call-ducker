using Gtk;
using Adw;

[GtkTemplate (ui = "/io/github/UntoastedToast/CallDucker/ui/call-ducker-main-page.ui")]
public class CallDuckerMainPage : Adw.NavigationPage {
    [GtkChild] public unowned Adw.SwitchRow enabled_row;
    [GtkChild] public unowned Adw.ActionRow status_row;
    [GtkChild] public unowned Gtk.Image status_icon;
    [GtkChild] public unowned Gtk.Adjustment volume_adjustment;
    [GtkChild] public unowned Adw.SpinRow volume_row;
    [GtkChild] public unowned Adw.ActionRow preview_row;
    [GtkChild] public unowned Gtk.Button preview_button;
    [GtkChild] public unowned Adw.PreferencesGroup ducking_group;
    [GtkChild] public unowned Adw.PreferencesGroup call_apps_group;
    [GtkChild] public unowned Adw.PreferencesGroup target_apps_group;
    [GtkChild] public unowned Adw.SwitchRow games_row;
    [GtkChild] public unowned Adw.ActionRow manage_row;
    [GtkChild] public unowned Gtk.MenuButton menu_button;
}
