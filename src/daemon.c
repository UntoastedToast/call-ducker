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
  return G_SOURCE_REMOVE;
}

static void
name_acquired (GDBusConnection *connection, const char *name, gpointer data)
{
  Runner *runner = data;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  CdPulseBackend *pulse;

  (void)name;
  if (runner->stopping)
    return;
  runner->name_acquired = TRUE;

  /* Creating the backend can connect to PulseAudio and loading the controller
   * can touch persistent state. Do both only after exclusive name ownership. */
  settings = g_settings_new ("io.github.UntoastedToast.CallDucker");
  pulse = cd_pulse_backend_new ();
  runner->backend = CD_AUDIO_BACKEND (pulse);
  runner->daemon
      = cd_daemon_new (settings, runner->backend, cd_state_store_new ("applications.json"),
                       cd_state_store_new ("pulse-restore.json"));
  if (!cd_daemon_export (runner->daemon, connection, OBJECT_PATH, &error))
    {
      g_printerr ("Could not export D-Bus service: %s\n", error->message);
      runner->exit_status = 1;
      begin_shutdown (runner);
      return;
    }
  cd_daemon_start (runner->daemon);
}

static void
name_lost (GDBusConnection *connection, const char *name, gpointer data)
{
  Runner *runner = data;

  (void)name;
  if (!runner->name_acquired)
    {
      if (connection == NULL)
        g_printerr ("Could not connect to the session bus\n");
      else
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

int
main (void)
{
  Runner runner = { 0 };

  setlocale (LC_ALL, "");
  runner.loop = g_main_loop_new (NULL, FALSE);
  runner.sigterm = g_unix_signal_add (SIGTERM, stop_cb, &runner);
  runner.sigint = g_unix_signal_add (SIGINT, stop_cb, &runner);
  runner.owner = g_bus_own_name (G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                 NULL, name_acquired, name_lost, &runner, NULL);
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
  g_main_loop_unref (runner.loop);
  return runner.exit_status;
}
