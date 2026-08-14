#include "cd-daemon.h"
#include "fake-audio-backend.h"

#include <glib/gstdio.h>

#define BUS_NAME "io.github.UntoastedToast.CallDucker.Daemon"
#define OBJECT_PATH "/io/github/UntoastedToast/CallDucker/Daemon"
#define INTERFACE "io.github.UntoastedToast.CallDucker.Daemon1"

typedef struct
{
  GMainLoop *loop;
  GVariant *result;
  GError *error;
  gboolean done;
} AsyncCall;

typedef struct
{
  GMainLoop *loop;
  GDBusProxy *proxy;
  GError *error;
} AsyncProxy;

static void
proxy_done (GObject *object, GAsyncResult *result, gpointer data)
{
  AsyncProxy *creation = data;

  (void)object;
  creation->proxy = g_dbus_proxy_new_finish (result, &creation->error);
  g_main_loop_quit (creation->loop);
}

static GDBusProxy *
create_proxy (GDBusConnection *connection, GError **error)
{
  AsyncProxy creation = { .loop = g_main_loop_new (NULL, FALSE) };

  g_dbus_proxy_new (connection, G_DBUS_PROXY_FLAGS_NONE, NULL, BUS_NAME, OBJECT_PATH, INTERFACE,
                    NULL, proxy_done, &creation);
  g_main_loop_run (creation.loop);
  g_main_loop_unref (creation.loop);
  if (creation.error != NULL)
    {
      g_propagate_error (error, creation.error);
      return NULL;
    }
  return creation.proxy;
}

static void
call_done (GObject *object, GAsyncResult *result, gpointer data)
{
  AsyncCall *call = data;

  call->result = g_dbus_proxy_call_finish (G_DBUS_PROXY (object), result, &call->error);
  call->done = TRUE;
  if (call->loop != NULL)
    g_main_loop_quit (call->loop);
}

static void
start_proxy_call (AsyncCall *call, GDBusProxy *proxy, const char *method, GVariant *parameters)
{
  g_dbus_proxy_call (proxy, method, parameters, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, call_done,
                     call);
}

static GVariant *
await_proxy_call (AsyncCall *call, GError **error)
{
  if (!call->done)
    {
      call->loop = g_main_loop_new (NULL, FALSE);
      g_main_loop_run (call->loop);
      g_clear_pointer (&call->loop, g_main_loop_unref);
    }
  if (call->error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&call->error));
      return NULL;
    }
  return g_steal_pointer (&call->result);
}

static GVariant *
call_proxy (GDBusProxy *proxy, const char *method, GVariant *parameters, GError **error)
{
  AsyncCall call = { 0 };

  start_proxy_call (&call, proxy, method, parameters);
  return await_proxy_call (&call, error);
}

static GVariant *
get_property (GDBusProxy *proxy, const char *name, GError **error)
{
  g_autoptr (GVariant) reply = call_proxy (proxy, "org.freedesktop.DBus.Properties.Get",
                                           g_variant_new ("(ss)", INTERFACE, name), error);
  GVariant *boxed;

  if (reply == NULL)
    return NULL;
  g_variant_get (reply, "(v)", &boxed);
  return boxed;
}

static void
assert_string_property (GDBusProxy *proxy, const char *name, const char *expected)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) value = get_property (proxy, name, &error);

  g_assert_no_error (error);
  g_assert_nonnull (value);
  g_assert_cmpstr (g_variant_get_string (value, NULL), ==, expected);
}

static void
assert_boolean_property (GDBusProxy *proxy, const char *name, gboolean expected)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) value = get_property (proxy, name, &error);

  g_assert_no_error (error);
  g_assert_nonnull (value);
  g_assert_cmpint (g_variant_get_boolean (value), ==, expected);
}

static void
assert_uint_property (GDBusProxy *proxy, const char *name, guint expected)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) value = get_property (proxy, name, &error);

  g_assert_no_error (error);
  g_assert_nonnull (value);
  g_assert_cmpuint (g_variant_get_uint32 (value), ==, expected);
}

static void
assert_strv_property (GDBusProxy *proxy, const char *name, const char *const *expected)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) value = get_property (proxy, name, &error);
  g_auto (GStrv) actual = NULL;
  gsize actual_length;
  gsize expected_length = g_strv_length ((char **)expected);

  g_assert_no_error (error);
  g_assert_nonnull (value);
  actual = g_variant_dup_strv (value, &actual_length);
  g_assert_cmpuint (actual_length, ==, expected_length);
  for (gsize index = 0; index < expected_length; index++)
    g_assert_cmpstr (actual[index], ==, expected[index]);
}

static void
spin_for (guint milliseconds)
{
  gint64 until = g_get_monotonic_time () + milliseconds * 1000;

  while (g_get_monotonic_time () < until)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
}

static void
spin_until_elapsed (gint64 started, guint milliseconds)
{
  gint64 elapsed = g_get_monotonic_time () - started;

  if (elapsed < (gint64)milliseconds * 1000)
    spin_for ((guint)(((gint64)milliseconds * 1000 - elapsed) / 1000));
}

static void
applications_signal (GDBusProxy *proxy, const char *sender, const char *name, GVariant *parameters,
                     gpointer data)
{
  guint *count = data;

  (void)proxy;
  (void)sender;
  (void)parameters;
  if (g_str_equal (name, "ApplicationsChanged"))
    (*count)++;
}

static void
daemon_contract_over_real_bus (void)
{
  g_autoptr (GTestDBus) test_bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_autoptr (GDBusConnection) service_connection = NULL;
  g_autoptr (GDBusConnection) client_connection = NULL;
  g_autoptr (GDBusProxy) proxy = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp ("call-ducker-dbus-XXXXXX", NULL);
  g_autofree char *applications_path = g_build_filename (directory, "applications.json", NULL);
  g_autofree char *restore_path = g_build_filename (directory, "pulse-restore.json", NULL);
  const char *always[] = { "application:target", "application:second", NULL };
  const char *empty[] = { NULL };
  const char *target_name[] = { "Target", NULL };
  const char *call_target_names[] = { "Second", "Target", NULL };
  const char *discord_name[] = { "Discord", NULL };
  CdDaemon *daemon;
  guint owner;
  guint application_signals = 0;

  g_test_dbus_up (test_bus);
  g_test_message ("test bus started");
  service_connection = g_dbus_connection_new_for_address_sync (
      g_test_dbus_get_bus_address (test_bus),
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
          | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_assert_no_error (error);
  client_connection = g_dbus_connection_new_for_address_sync (
      g_test_dbus_get_bus_address (test_bus),
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
          | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_assert_no_error (error);
  settings = g_settings_new ("io.github.UntoastedToast.CallDucker");
  g_settings_set_boolean (settings, "enabled", TRUE);
  g_settings_set_boolean (settings, "auto-detect-games", FALSE);
  g_settings_set_strv (settings, "always-duck-selectors", always);
  test_audio_backend_add_stream (backend, 10, "application:target", "Target", FALSE, TRUE, "target",
                                 "target", NULL, FALSE, 0.8);
  daemon = cd_daemon_new (settings, CD_AUDIO_BACKEND (backend),
                          cd_state_store_new_for_path (applications_path),
                          cd_state_store_new_for_path (restore_path));
  g_assert_true (cd_daemon_export (daemon, service_connection, OBJECT_PATH, &error));
  g_assert_no_error (error);
  owner = g_bus_own_name_on_connection (service_connection, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                        NULL, NULL, NULL, NULL);
  spin_for (20);
  proxy = create_proxy (client_connection, &error);
  g_test_message ("proxy created");
  g_assert_no_error (error);
  g_signal_connect (proxy, "g-signal", G_CALLBACK (applications_signal), &application_signals);

  test_audio_backend_changed (backend);
  spin_for (30);
  g_assert_cmpuint (application_signals, ==, 1);
  g_autoptr (GVariant) listed = call_proxy (proxy, "ListApplications", NULL, &error);
  g_test_message ("applications listed");
  g_assert_no_error (error);
  g_autoptr (GVariant) applications = g_variant_get_child_value (listed, 0);
  g_assert_cmpuint (g_variant_n_children (applications), ==, 1);
  g_test_message ("all initial properties read");
  assert_string_property (proxy, "State", "Listening");
  assert_strv_property (proxy, "ActiveTriggerNames", empty);
  assert_strv_property (proxy, "DuckedTargetNames", empty);
  assert_uint_property (proxy, "PendingRestorations", 0);
  assert_string_property (proxy, "LastError", "");
  assert_boolean_property (proxy, "PreviewActive", FALSE);

  test_audio_backend_set_delayed (backend, TRUE);
  AsyncCall superseded_preview = { 0 };
  AsyncCall delayed_preview = { 0 };
  start_proxy_call (&superseded_preview, proxy, "Preview", g_variant_new ("(u)", 500u));
  spin_for (30);
  g_assert_false (superseded_preview.done);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  assert_boolean_property (proxy, "PreviewActive", FALSE);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);

  /* Enter rollback before superseding so the first controller request returns
   * a real stale failure rather than the controller's cancellation result. */
  g_assert_true (test_audio_backend_complete_next (backend, FALSE));
  spin_for (30);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  start_proxy_call (&delayed_preview, proxy, "Preview", g_variant_new ("(u)", 500u));
  spin_for (30);
  g_assert_true (superseded_preview.done);
  g_assert_false (delayed_preview.done);
  g_autoptr (GError) superseded_error = NULL;
  g_autoptr (GVariant) superseded_result
      = await_proxy_call (&superseded_preview, &superseded_error);
  g_autofree char *remote_error = g_dbus_error_get_remote_error (superseded_error);
  g_assert_null (superseded_result);
  g_assert_cmpstr (remote_error, ==, "io.github.UntoastedToast.CallDucker.Error.Superseded");

  /* The superseded apply now finishes its rollback. Its stale failure must
   * neither mark the backend unavailable nor abort the replacement preview. */
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  spin_for (30);
  g_assert_false (delayed_preview.done);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  g_autoptr (GVariant) preview = await_proxy_call (&delayed_preview, &error);
  g_test_message ("preview completed only after the backend acknowledgement");
  g_assert_no_error (error);
  g_assert_nonnull (preview);
  assert_boolean_property (proxy, "PreviewActive", TRUE);
  assert_strv_property (proxy, "DuckedTargetNames", target_name);
  assert_uint_property (proxy, "PendingRestorations", 1);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.70, 0.001);

  AsyncCall delayed_cancel = { 0 };
  AsyncCall preview_after_barrier = { 0 };
  start_proxy_call (&delayed_cancel, proxy, "CancelPreview", NULL);
  spin_for (30);
  g_assert_false (delayed_cancel.done);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  assert_boolean_property (proxy, "PreviewActive", FALSE);
  start_proxy_call (&preview_after_barrier, proxy, "Preview", g_variant_new ("(u)", 500u));
  spin_for (30);
  g_assert_false (preview_after_barrier.done);

  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  spin_for (30);
  g_assert_true (delayed_cancel.done);
  g_assert_false (preview_after_barrier.done);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_autoptr (GVariant) cancelled = await_proxy_call (&delayed_cancel, &error);
  g_assert_no_error (error);
  g_assert_nonnull (cancelled);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);

  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  g_autoptr (GVariant) preview_again = await_proxy_call (&preview_after_barrier, &error);
  g_assert_no_error (error);
  g_assert_nonnull (preview_again);
  assert_boolean_property (proxy, "PreviewActive", TRUE);

  AsyncCall delayed_restore = { 0 };
  start_proxy_call (&delayed_restore, proxy, "Restore", NULL);
  spin_for (30);
  g_assert_false (delayed_restore.done);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_autoptr (GVariant) restored = NULL;
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  restored = await_proxy_call (&delayed_restore, &error);
  g_assert_no_error (error);
  g_assert_nonnull (restored);
  assert_boolean_property (proxy, "PreviewActive", FALSE);
  assert_uint_property (proxy, "PendingRestorations", 0);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);

  AsyncCall interrupted_preview = { 0 };
  AsyncCall interrupting_cancel = { 0 };
  start_proxy_call (&interrupted_preview, proxy, "Preview", g_variant_new ("(u)", 500u));
  spin_for (30);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  start_proxy_call (&interrupting_cancel, proxy, "CancelPreview", NULL);
  spin_for (30);
  g_assert_true (interrupted_preview.done);
  g_assert_false (interrupting_cancel.done);
  g_autoptr (GError) interrupted_error = NULL;
  g_autoptr (GVariant) interrupted_result
      = await_proxy_call (&interrupted_preview, &interrupted_error);
  g_autofree char *interrupted_remote_error = g_dbus_error_get_remote_error (interrupted_error);
  g_assert_null (interrupted_result);
  g_assert_cmpstr (interrupted_remote_error, ==,
                   "io.github.UntoastedToast.CallDucker.Error.Superseded");
  /* The target Ack may still arrive, but the queued restoration has priority
   * over every remaining target operation. */
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  spin_for (30);
  g_assert_false (interrupting_cancel.done);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  g_autoptr (GVariant) interrupted_cancelled = await_proxy_call (&interrupting_cancel, &error);
  g_assert_no_error (error);
  g_assert_nonnull (interrupted_cancelled);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);

  AsyncCall disabled_preview = { 0 };
  start_proxy_call (&disabled_preview, proxy, "Preview", g_variant_new ("(u)", 500u));
  spin_for (30);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_settings_set_boolean (settings, "enabled", FALSE);
  spin_for (30);
  g_assert_true (disabled_preview.done);
  assert_string_property (proxy, "State", "Disabled");
  g_autoptr (GError) disabled_error = NULL;
  g_autoptr (GVariant) disabled_result = await_proxy_call (&disabled_preview, &disabled_error);
  g_assert_null (disabled_result);
  g_assert_true (g_dbus_error_is_remote_error (disabled_error));
  /* Disable dispatches Restore immediately, superseding every still queued
   * target operation rather than waiting for the whole target batch. */
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  spin_for (30);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  spin_for (30);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);
  assert_uint_property (proxy, "PendingRestorations", 0);
  g_settings_set_boolean (settings, "enabled", TRUE);
  spin_for (30);
  assert_string_property (proxy, "State", "Listening");

  test_audio_backend_add_stream (backend, 12, "application:target", "Target", FALSE, TRUE, "target",
                                 "target", NULL, FALSE, 0.4);
  test_audio_backend_changed (backend);
  spin_for (30);
  AsyncCall failed_preview = { 0 };
  start_proxy_call (&failed_preview, proxy, "Preview", g_variant_new ("(u)", 500u));
  spin_for (30);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  spin_for (30);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, FALSE));
  spin_for (30);
  gboolean saw_rollback = test_audio_backend_get_pending_sets (backend) > 0;
  for (guint attempt = 0; !failed_preview.done && attempt < 100; attempt++)
    {
      if (test_audio_backend_get_pending_sets (backend) > 0)
        g_assert_true (test_audio_backend_complete_next (backend, TRUE));
      spin_for (10);
    }
  g_assert_true (saw_rollback);
  g_autoptr (GError) preview_error = NULL;
  g_autoptr (GVariant) failed_result = await_proxy_call (&failed_preview, &preview_error);
  g_autofree char *preview_remote_error = g_dbus_error_get_remote_error (preview_error);
  g_assert_null (failed_result);
  g_assert_cmpstr (preview_remote_error, ==,
                   "io.github.UntoastedToast.CallDucker.Error.OperationFailed");
  assert_boolean_property (proxy, "PreviewActive", FALSE);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 12), 0.4, 0.001);
  test_audio_backend_set_delayed (backend, FALSE);
  test_audio_backend_changed (backend);
  spin_for (30);

  g_settings_set_double (settings, "background-volume", 1.0);
  gint64 full_started = g_get_monotonic_time ();
  g_autoptr (GVariant) full = call_proxy (proxy, "Preview", g_variant_new ("(u)", 5000u), &error);
  g_assert_no_error (error);
  g_assert_nonnull (full);

  test_audio_backend_add_stream (backend, 11, "application:second", "Second", FALSE, TRUE, "second",
                                 "second", NULL, FALSE, 0.5);
  test_audio_backend_changed (backend);
  spin_for (30);
  g_assert_cmpuint (application_signals, ==, 2);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 11), 1.0, 0.001);
  test_audio_backend_changed (backend);
  spin_for (30);
  g_assert_cmpuint (application_signals, ==, 2);

  /* A backend snapshot during preview must reconcile new streams without
   * treating our own volume event as the end of the five-second preview. */
  spin_until_elapsed (full_started, 4800);
  assert_boolean_property (proxy, "PreviewActive", TRUE);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 1.0, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 11), 1.0, 0.001);
  spin_until_elapsed (full_started, 5250);
  assert_boolean_property (proxy, "PreviewActive", FALSE);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 11), 0.5, 0.001);

  CdAudioStream *call
      = test_audio_backend_add_stream (backend, 20, "application:discord", "Discord", TRUE, TRUE,
                                       "discord", "discord", NULL, FALSE, 1.0);
  test_audio_backend_changed (backend);
  spin_for (100);
  assert_string_property (proxy, "State", "Call active");
  assert_strv_property (proxy, "ActiveTriggerNames", discord_name);
  /* The two Target streams collapse to one application beside Second. */
  assert_strv_property (proxy, "DuckedTargetNames", call_target_names);
  assert_uint_property (proxy, "PendingRestorations", 3);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 1.0, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 11), 1.0, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 12), 1.0, 0.001);
  g_settings_set_double (settings, "background-volume", 0.7);
  spin_for (30);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.7, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 11), 0.7, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 12), 0.7, 0.001);
  call->running = FALSE;
  test_audio_backend_changed (backend);
  spin_for (1100);
  assert_string_property (proxy, "State", "Listening");
  assert_strv_property (proxy, "ActiveTriggerNames", empty);
  assert_strv_property (proxy, "DuckedTargetNames", empty);
  assert_uint_property (proxy, "PendingRestorations", 0);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 10), 0.8, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 11), 0.5, 0.001);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 12), 0.4, 0.001);
  test_audio_backend_fail (backend, "backend failed");
  spin_for (20);
  assert_string_property (proxy, "State", "Audio service unavailable");
  assert_string_property (proxy, "LastError", "backend failed");

  g_bus_unown_name (owner);
  cd_daemon_free (daemon);
  g_clear_object (&proxy);
  g_clear_object (&client_connection);
  g_clear_object (&service_connection);
  g_test_dbus_down (test_bus);
  g_unlink (applications_path);
  g_unlink (restore_path);
  g_rmdir (directory);
}

int
main (int argc, char **argv)
{
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/daemon/dbus-contract", daemon_contract_over_real_bus);
  return g_test_run ();
}
