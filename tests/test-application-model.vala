private struct FakeApplication {
    public string id;
    public string name;
    public string icon;
    public bool input;
    public bool game;
    public bool available;
}

private Variant applications (FakeApplication[] values) {
    var list = new VariantBuilder (new VariantType ("aa{sv}"));
    foreach (FakeApplication value in values) {
        var app = new VariantBuilder (new VariantType ("a{sv}"));
        app.add ("{sv}", "id", new Variant.string (value.id));
        app.add ("{sv}", "name", new Variant.string (value.name));
        app.add ("{sv}", "icon", new Variant.string (value.icon));
        app.add ("{sv}", "input", new Variant.boolean (value.input));
        app.add ("{sv}", "game", new Variant.boolean (value.game));
        app.add ("{sv}", "available", new Variant.boolean (value.available));
        list.add_value (app.end ());
    }
    return list.end ();
}

private void snapshot_reconciliation () {
    var catalog = new ApplicationCatalog ();
    FakeApplication[] first = {
        { "application:z", "Same", "", false, false, true },
        { "application:a", "Same", "alpha", false, false, true },
        { "binary:offline", "Offline", "offline", false, false, false },
        { "binary:gone", "Gone", "gone", false, false, false },
        { "binary:game-input", "Game input", "game", true, true, true },
        { "application:mic", "Microphone", "mic", true, false, true }
    };
    catalog.reconcile (applications (first), { "application:mic" }, { "binary:offline" }, {});

    ApplicationInfo[] outputs = catalog.visible (false);
    assert (outputs.length == 3);
    assert (outputs[0].id == "application:a");
    assert (outputs[1].id == "application:z");
    assert (outputs[2].id == "binary:offline");
    assert (outputs[0].available && !outputs[2].available);
    assert (outputs[1].icon == "application-x-executable-symbolic");
    assert (catalog.has_duplicate_name (outputs[0]));
    assert (catalog.visible (true).length == 1);
    assert (catalog.visible (true)[0].id == "application:mic");

    FakeApplication[] second = {
        { "application:a", "Updated", "alpha", false, false, true },
        { "binary:offline", "Offline", "offline", false, false, false }
    };
    catalog.reconcile (applications (second), {}, { "binary:offline" }, {});
    outputs = catalog.visible (false);
    assert (outputs.length == 2);
    assert (outputs[0].name == "Updated");
    assert (!catalog.has_duplicate_name (outputs[0]));
}

private void search_name_and_id () {
    var catalog = new ApplicationCatalog ();
    FakeApplication[] values = {
        { "application:org.example.Player", "Music Box", "music", false, false, true },
        { "application:chat", "Conference", "chat", true, false, true }
    };
    catalog.reconcile (applications (values), {}, {}, {});
    assert (catalog.visible (false, "MUSIC").length == 1);
    assert (catalog.visible (false, "ORG.EXAMPLE").length == 1);
    assert (catalog.visible (false, "missing").length == 0);
}

public static int main (string[] args) {
    Test.init (ref args);
    Test.add_func ("/applications/snapshot-reconciliation", snapshot_reconciliation);
    Test.add_func ("/applications/search-name-and-id", search_name_and_id);
    return Test.run ();
}
