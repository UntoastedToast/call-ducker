#include <gio/gio.h>

#define BUS_NAME "io.github.UntoastedToast.CallDucker.Daemon"
#define OBJECT_PATH "/io/github/UntoastedToast/CallDucker/Daemon"
#define INTERFACE_NAME "io.github.UntoastedToast.CallDucker.Daemon1"

typedef struct
{
  GMainLoop *loop;
  GError *error;
  gboolean answered;
  gboolean timed_out;
} Fixture;

static void
name_appeared (GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
  Fixture *fixture = data;
  g_autoptr (GVariant) reply = NULL;

  (void)name;
  (void)owner;
  /* The bus announces a new owner before the daemon's own name-acquired
   * callback runs. A client calling at this instant must still find the
   * exported object instead of org.freedesktop.DBus.Error.UnknownMethod. */
  reply = g_dbus_connection_call_sync (connection, BUS_NAME, OBJECT_PATH, INTERFACE_NAME,
                                       "ListApplications", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                                       NULL, &fixture->error);
  fixture->answered = reply != NULL;
  g_main_loop_quit (fixture->loop);
}

static gboolean
give_up (gpointer data)
{
  Fixture *fixture = data;

  fixture->timed_out = TRUE;
  g_main_loop_quit (fixture->loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  Fixture fixture = { 0 };
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GSubprocess) daemon = NULL;
  g_autoptr (GError) error = NULL;
  guint watcher;

  g_assert_cmpint (argc, >, 1);
  fixture.loop = g_main_loop_new (NULL, FALSE);
  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  watcher = g_bus_watch_name_on_connection (connection, BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                            name_appeared, NULL, &fixture, NULL);
  daemon = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, argv[1], NULL);
  g_assert_no_error (error);
  g_timeout_add_seconds (10, give_up, &fixture);
  g_main_loop_run (fixture.loop);

  g_subprocess_force_exit (daemon);
  g_subprocess_wait (daemon, NULL, NULL);
  g_bus_unwatch_name (watcher);
  g_main_loop_unref (fixture.loop);

  g_assert_false (fixture.timed_out);
  g_assert_no_error (fixture.error);
  g_assert_true (fixture.answered);
  return 0;
}
