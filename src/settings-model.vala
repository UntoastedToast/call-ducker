public class SettingsModel : Object {
    public Settings settings { get; private set; }

    public SettingsModel () {
        settings = new Settings ("io.github.UntoastedToast.CallDucker");
    }

    public static bool contains (string[] values, string selector) {
        foreach (string value in values)
            if (value == selector) return true;
        return false;
    }

    public static string[] without (string[] values, string selector) {
        string[] result = {};
        foreach (string value in values)
            if (value != selector) result += value;
        return result;
    }

    public void set_target_policy (string selector, uint policy) {
        string[] always = without (settings.get_strv ("always-duck-selectors"), selector);
        string[] never = without (settings.get_strv ("never-duck-selectors"), selector);
        if (policy == 1) always += selector;
        if (policy == 2) never += selector;
        settings.set_strv ("always-duck-selectors", always);
        settings.set_strv ("never-duck-selectors", never);
    }

    public void set_call_selector (string selector, bool active) {
        string[] calls = without (settings.get_strv ("call-app-selectors"), selector);
        if (active) calls += selector;
        settings.set_strv ("call-app-selectors", calls);
    }
}
