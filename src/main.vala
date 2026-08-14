public static int main (string[] args) {
    Intl.setlocale (LocaleCategory.ALL, "");
    Intl.bindtextdomain ("call-ducker", "/usr/share/locale");
    Intl.textdomain ("call-ducker");
    return new CallDuckerApp ().run (args);
}
