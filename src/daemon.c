#include "cd-daemon.h"
#include "pulse-backend.h"

#include <glib-unix.h>
#include <locale.h>

#define BUS_NAME "io.github.UntoastedToast.CallDucker.Daemon"
#define OBJECT_PATH "/io/github/UntoastedToast/CallDucker/Daemon"
#define SHUTDOWN_GUARD_MS 5000

typedef struct
{
  GMainLoop *loop;
  CdDaemon *daemon;
  CdAudioBackend *backend;
  GDBusConnection *connection;
  GCancellable *shutdown_cancellable;
  guint owner;
  guint sigterm;
  guint sigint;
  guint shutdown_guard;
  gboolean name_acquired;
  gboolean stopping;
  int exit_status;
} Runner;

static gboolean
shutdown_guard_expired (gpointer data)
{
  Runner *runner = data;

  runner->shutdown_guard = 0;
  g_warning ("Timed out while restoring volumes during shutdown");
  g_cancellable_cancel (runner->shutdown_cancellable);
  g_main_loop_quit (runner->loop);
  return G_SOURCE_REMOVE;
}

static void
restore_done (GObject *object, GAsyncResult *result, gpointer data)
{
  Runner *runner = data;
  g_autoptr (GError) error = NULL;

  (void)object;
  if (!cd_daemon_restore_finish (runner->daemon, result, &error)
      && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Could not restore all currently available streams before shutdown: %s",
               error != NULL ? error->message : "unknown error");
  if (runner->shutdown_guard != 0)
    {
      g_source_remove (runner->shutdown_guard);
      runner->shutdown_guard = 0;
    }
  g_main_loop_quit (runner->loop);
}

static void
begin_shutdown (Runner *runner)
{
  if (runner->stopping)
    return;
  runner->stopping = TRUE;
  /* Release the name before unexporting. A client must see the service go
   * away instead of an owned name whose object path no longer answers. */
  if (runner->owner != 0)
    {
      g_bus_unown_name (runner->owner);
      runner->owner = 0;
    }
  if (runner->daemon == NULL)
    {
      g_main_loop_quit (runner->loop);
      return;
    }
  cd_daemon_prepare_shutdown (runner->daemon);
  runner->shutdown_cancellable = g_cancellable_new ();
  cd_daemon_restore_async (runner->daemon, runner->shutdown_cancellable, restore_done, runner);
  runner->shutdown_guard = g_timeout_add (SHUTDOWN_GUARD_MS, shutdown_guard_expired, runner);
}

static gboolean
stop_cb (gpointer data)
{
  begin_shutdown (data);
  /* Keep the source alive so that main() can still remove it by id. */
  return G_SOURCE_CONTINUE;
}

static void
name_acquired (GDBusConnection *connection, const char *name, gpointer data)
{
  Runner *runner = data;

  (void)connection;
  (void)name;
  if (runner->stopping)
    return;
  runner->name_acquired = TRUE;

  /* Connecting to PulseAudio is the first side effect of this process, so it
   * waits for exclusive name ownership. */
  cd_pulse_backend_start (CD_PULSE_BACKEND (runner->backend));
  cd_daemon_start (runner->daemon);
}

static void
name_lost (GDBusConnection *connection, const char *name, gpointer data)
{
  Runner *runner = data;

  (void)connection;
  (void)name;
  if (!runner->name_acquired)
    {
      g_printerr ("Another Call Ducker daemon instance is already running\n");
      runner->exit_status = 1;
      begin_shutdown (runner);
      return;
    }

  /* A later loss can leave adjusted streams behind. Restore with the same
   * bounded shutdown path used for SIGTERM/SIGINT. */
  runner->name_acquired = FALSE;
  runner->exit_status = 1;
  begin_shutdown (runner);
}

static gboolean
setup (Runner *runner)
{
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;

  runner->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (runner->connection == NULL)
    {
      g_printerr ("Could not connect to the session bus: %s\n", error->message);
      return FALSE;
    }

  /* Only file reads happen here. A second instance loses the name below and
   * exits without having connected to PulseAudio or written any state. */
  settings = g_settings_new ("io.github.UntoastedToast.CallDucker");
  runner->backend = CD_AUDIO_BACKEND (cd_pulse_backend_new ());
  runner->daemon
      = cd_daemon_new (settings, runner->backend, cd_state_store_new ("applications.json"),
                       cd_state_store_new ("pulse-restore.json"));

  /* Export before requesting the name. The bus announces a new owner as soon
   * as the request is processed, and clients call the moment they see it. */
  if (!cd_daemon_export (runner->daemon, runner->connection, OBJECT_PATH, &error))
    {
      g_printerr ("Could not export D-Bus service: %s\n", error->message);
      return FALSE;
    }
  runner->owner = g_bus_own_name_on_connection (runner->connection, BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, name_acquired,
                                                name_lost, runner, NULL);
  return TRUE;
}

int
main (void)
{
  Runner runner = { 0 };

  setlocale (LC_ALL, "");
  runner.loop = g_main_loop_new (NULL, FALSE);
  runner.sigterm = g_unix_signal_add (SIGTERM, stop_cb, &runner);
  runner.sigint = g_unix_signal_add (SIGINT, stop_cb, &runner);
  if (!setup (&runner))
    runner.exit_status = 1;
  else
    g_main_loop_run (runner.loop);

  if (runner.shutdown_guard != 0)
    g_source_remove (runner.shutdown_guard);
  if (runner.sigterm != 0)
    g_source_remove (runner.sigterm);
  if (runner.sigint != 0)
    g_source_remove (runner.sigint);
  if (runner.owner != 0)
    g_bus_unown_name (runner.owner);
  cd_daemon_free (runner.daemon);
  g_clear_object (&runner.shutdown_cancellable);
  g_clear_object (&runner.backend);
  g_clear_object (&runner.connection);
  g_main_loop_unref (runner.loop);
  return runner.exit_status;
}
