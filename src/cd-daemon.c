#include "cd-daemon.h"

#include "cd-application-registry.h"
#include "cd-dbus-service.h"
#include "cd-engine.h"
#include "cd-match.h"
#include "cd-volume-controller.h"

typedef enum
{
  DAEMON_MODE_RESTORE,
  DAEMON_MODE_PREVIEW,
  DAEMON_MODE_CALL,
} DaemonMode;

typedef struct
{
  CdDaemon *daemon;
  DaemonMode mode;
  guint64 generation;
  GHashTable *target_names;
} ControllerOperation;

struct _CdDaemon
{
  gint refs;
  GSettings *settings;
  CdAudioBackend *backend;
  CdEngine *engine;
  CdDbusService *dbus;
  CdVolumeController *volumes;
  CdApplicationRegistry *registry;
  char *journal_error;
  GHashTable *trigger_snapshot;
  GPtrArray *restore_tasks;
  GTask *preview_task;
  guint preview_timer;
  guint end_timer;
  guint preview_duration_ms;
  guint64 generation;
  gboolean snapshot_ready;
  gboolean backend_ready;
  gboolean journal_blocked;
  gboolean call_present;
  gboolean preview_requested;
  gboolean preview_active;
  guint operations_in_flight;
  gboolean reconcile_pending;
  gboolean restore_barrier;
  gboolean shutting_down;
  gboolean disposed;
};

static void request_reconcile (CdDaemon *self);

static CdDaemon *
daemon_ref (CdDaemon *self)
{
  g_atomic_int_inc (&self->refs);
  return self;
}

static void
daemon_unref (CdDaemon *self)
{
  if (!g_atomic_int_dec_and_test (&self->refs))
    return;
  g_clear_object (&self->preview_task);
  g_clear_pointer (&self->restore_tasks, g_ptr_array_unref);
  g_hash_table_unref (self->trigger_snapshot);
  g_clear_pointer (&self->dbus, cd_dbus_service_free);
  cd_volume_controller_free (self->volumes);
  cd_application_registry_free (self->registry);
  cd_engine_free (self->engine);
  g_free (self->journal_error);
  g_clear_object (&self->backend);
  g_clear_object (&self->settings);
  g_free (self);
}

static void
emit_preview_changed (CdDaemon *self)
{
  if (self->dbus != NULL)
    cd_dbus_service_set_preview_active (self->dbus, self->preview_active);
}

static void
complete_task (GTask *task, const GError *error)
{
  if (error != NULL)
    g_task_return_error (task, g_error_copy (error));
  else
    g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
complete_preview (CdDaemon *self, const GError *error)
{
  GTask *task = g_steal_pointer (&self->preview_task);

  if (task != NULL)
    complete_task (task, error);
}

static void
supersede_preview (CdDaemon *self)
{
  g_autoptr (GError) error = NULL;

  if (self->preview_task == NULL)
    return;
  error = g_error_new_literal (CD_DAEMON_ERROR, CD_DAEMON_ERROR_REQUEST_SUPERSEDED,
                               "The preview request was superseded");
  complete_preview (self, error);
}

static void
complete_restores (CdDaemon *self, const GError *error)
{
  while (self->restore_tasks->len > 0)
    {
      GTask *task = g_ptr_array_steal_index (self->restore_tasks, self->restore_tasks->len - 1);
      complete_task (task, error);
    }
}

static void
set_preview_state (CdDaemon *self, gboolean requested, gboolean active)
{
  gboolean changed = self->preview_active != active;

  if (self->preview_timer != 0 && !active)
    {
      g_source_remove (self->preview_timer);
      self->preview_timer = 0;
    }
  self->preview_requested = requested;
  self->preview_active = active;
  if (changed)
    emit_preview_changed (self);
}

static gboolean
selector_any (char **selectors, const CdAppIdentity *identity)
{
  for (guint index = 0; selectors != NULL && selectors[index] != NULL; index++)
    if (cd_selector_matches (selectors[index], identity))
      return TRUE;
  return FALSE;
}

static GPtrArray *
collect_targets (CdDaemon *self, GHashTable **out_names)
{
  GPtrArray *targets = g_ptr_array_new ();
  GHashTable *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_auto (GStrv) always = NULL;
  g_auto (GStrv) never = NULL;
  GPtrArray *streams;

  always = g_settings_get_strv (self->settings, "always-duck-selectors");
  never = g_settings_get_strv (self->settings, "never-duck-selectors");
  streams = self->backend != NULL ? cd_audio_backend_get_streams (self->backend) : NULL;
  for (guint index = 0; streams != NULL && index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);
      CdPolicy policy;

      if (stream->input || !stream->volume_controllable || stream->stable_id == NULL)
        continue;
      policy = cd_resolve_policy (&stream->identity, (const char *const *)always,
                                  (const char *const *)never,
                                  g_settings_get_boolean (self->settings, "auto-detect-games"));
      if (policy != CD_POLICY_ALWAYS)
        continue;
      g_ptr_array_add (targets, stream);
      g_hash_table_replace (
          names, g_strdup (stream->stable_id),
          g_strdup (stream->display_name != NULL ? stream->display_name : "Unknown application"));
    }
  *out_names = names;
  return targets;
}

static void
set_ducked_names (CdDaemon *self, GHashTable *names)
{
  GHashTableIter iterator;
  gpointer key;
  gpointer value;

  cd_engine_clear_ducked (self->engine);
  if (names == NULL)
    return;
  g_hash_table_iter_init (&iterator, names);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    cd_engine_set_ducked (self->engine, key, value, TRUE);
}

static DaemonMode
desired_mode (CdDaemon *self)
{
  if (self->restore_barrier)
    return DAEMON_MODE_RESTORE;
  if (self->preview_requested || self->preview_active)
    return DAEMON_MODE_PREVIEW;
  if (cd_engine_should_duck (self->engine))
    return DAEMON_MODE_CALL;
  return DAEMON_MODE_RESTORE;
}

static void
update_pending (CdDaemon *self)
{
  if (self->volumes != NULL)
    cd_engine_set_pending (self->engine, cd_volume_controller_get_pending (self->volumes));
}

static gboolean
preview_timeout (gpointer data)
{
  CdDaemon *self = data;

  self->preview_timer = 0;
  self->preview_active = FALSE;
  emit_preview_changed (self);
  request_reconcile (self);
  return G_SOURCE_REMOVE;
}

static void
controller_operation_free (ControllerOperation *operation)
{
  g_clear_pointer (&operation->target_names, g_hash_table_unref);
  daemon_unref (operation->daemon);
  g_free (operation);
}

static void dispatch_reconcile (CdDaemon *self);

static void
controller_done (GObject *object, GAsyncResult *result, gpointer data)
{
  ControllerOperation *operation = data;
  CdDaemon *self = operation->daemon;
  g_autoptr (GError) error = NULL;
  gboolean success;
  gboolean current;
  gboolean cancelled;

  (void)object;
  if (operation->mode == DAEMON_MODE_RESTORE)
    success = cd_volume_controller_restore_all_finish (self->volumes, result, &error);
  else
    success = cd_volume_controller_reconcile_finish (self->volumes, result, &error);
  if (!success && error == NULL)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Volume operation failed");
  g_assert_cmpuint (self->operations_in_flight, >, 0);
  self->operations_in_flight--;
  update_pending (self);
  if (self->disposed)
    {
      controller_operation_free (operation);
      return;
    }

  current = operation->generation == self->generation && operation->mode == desired_mode (self);
  cancelled = !success && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  if (cancelled)
    {
      /* A newer controller generation owns the result. Cancellation is not an
       * audio-service failure. */
      if (current)
        {
          self->generation++;
          self->reconcile_pending = TRUE;
        }
    }
  else if (!success && current && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY))
    {
      /* A stream identity conflict leaves that group untouched. The audio
       * service itself keeps working, so it is reported as a notice. */
      cd_engine_set_notice (self->engine, error->message);
    }
  else if (!success && current)
    {
      self->backend_ready = FALSE;
      cd_engine_set_available (self->engine, FALSE,
                               error != NULL ? error->message : "Volume operation failed");
    }

  if (cancelled)
    {
      /* Keep preview/restore waiters attached to the latest desired state. */
    }
  else if (operation->mode == DAEMON_MODE_RESTORE)
    {
      if (success && current)
        set_ducked_names (self, NULL);
      if (self->restore_barrier && current)
        {
          self->restore_barrier = FALSE;
          complete_restores (self, success ? NULL : error);
          if (!success && self->preview_task != NULL)
            {
              set_preview_state (self, FALSE, FALSE);
              complete_preview (self, error);
            }
          if (desired_mode (self) != DAEMON_MODE_RESTORE)
            {
              self->generation++;
              self->reconcile_pending = TRUE;
            }
        }
    }
  else if (operation->mode == DAEMON_MODE_PREVIEW && !success && current)
    {
      set_preview_state (self, FALSE, FALSE);
      complete_preview (self, error);
      self->reconcile_pending = TRUE;
      self->generation++;
    }
  else if (success && current)
    {
      set_ducked_names (self, operation->target_names);
      if (operation->mode == DAEMON_MODE_PREVIEW && self->preview_task != NULL)
        {
          self->preview_requested = FALSE;
          if (!self->preview_active)
            {
              self->preview_active = TRUE;
              emit_preview_changed (self);
            }
          if (self->preview_timer != 0)
            g_source_remove (self->preview_timer);
          self->preview_timer = g_timeout_add (self->preview_duration_ms, preview_timeout, self);
          complete_preview (self, NULL);
        }
    }

  if (current && operation->mode != desired_mode (self) && !self->reconcile_pending)
    {
      self->generation++;
      self->reconcile_pending = TRUE;
    }
  if (self->reconcile_pending)
    dispatch_reconcile (self);
  controller_operation_free (operation);
}

static void
dispatch_reconcile (CdDaemon *self)
{
  ControllerOperation *operation;
  DaemonMode mode;

  if (self->disposed || !self->reconcile_pending)
    return;
  self->reconcile_pending = FALSE;
  if (self->volumes == NULL || self->journal_blocked)
    {
      g_autoptr (GError) error = NULL;

      if (self->journal_blocked)
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA, self->journal_error);
      if (self->preview_task != NULL)
        {
          g_autoptr (GError) preview_error = g_error_new_literal (
              CD_DAEMON_ERROR, CD_DAEMON_ERROR_PREVIEW_UNAVAILABLE,
              error != NULL ? error->message : "The audio backend is unavailable");
          set_preview_state (self, FALSE, FALSE);
          complete_preview (self, preview_error);
        }
      if (self->restore_barrier)
        {
          self->restore_barrier = FALSE;
          complete_restores (self, error);
        }
      return;
    }

  mode = desired_mode (self);
  operation = g_new0 (ControllerOperation, 1);
  operation->daemon = daemon_ref (self);
  operation->mode = mode;
  operation->generation = self->generation;
  self->operations_in_flight++;
  if (mode == DAEMON_MODE_RESTORE)
    {
      cd_volume_controller_restore_all_async (self->volumes, NULL, controller_done, operation);
    }
  else
    {
      g_autoptr (GPtrArray) targets = collect_targets (self, &operation->target_names);

      cd_volume_controller_reconcile_async (
          self->volumes, targets, g_settings_get_double (self->settings, "background-volume"), NULL,
          controller_done, operation);
    }
}

static void
request_reconcile (CdDaemon *self)
{
  if (self->disposed)
    return;
  self->generation++;
  self->reconcile_pending = TRUE;
  dispatch_reconcile (self);
}

static void
clear_end_timer (CdDaemon *self)
{
  if (self->end_timer == 0)
    return;
  g_source_remove (self->end_timer);
  self->end_timer = 0;
}

static void
clear_triggers (CdDaemon *self)
{
  g_hash_table_remove_all (self->trigger_snapshot);
  cd_engine_end_call (self->engine);
}

static gboolean
end_call (gpointer data)
{
  CdDaemon *self = data;

  self->end_timer = 0;
  clear_triggers (self);
  request_reconcile (self);
  return G_SOURCE_REMOVE;
}

static void
update_triggers (CdDaemon *self, GHashTable *next)
{
  GHashTableIter iterator;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init (&iterator, self->trigger_snapshot);
  while (g_hash_table_iter_next (&iterator, &key, NULL))
    if (!g_hash_table_contains (next, key))
      {
        cd_engine_set_trigger (self->engine, key, NULL, FALSE);
        g_hash_table_iter_remove (&iterator);
      }
  g_hash_table_iter_init (&iterator, next);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      const char *previous = g_hash_table_lookup (self->trigger_snapshot, key);

      if (g_strcmp0 (previous, value) != 0)
        {
          g_hash_table_replace (self->trigger_snapshot, g_strdup (key), g_strdup (value));
          cd_engine_set_trigger (self->engine, key, value, TRUE);
        }
    }
}

static void
evaluate_snapshot (CdDaemon *self, gboolean allow_end_delay)
{
  g_autoptr (GHashTable) calls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_auto (GStrv) selectors = g_settings_get_strv (self->settings, "call-app-selectors");
  GPtrArray *streams = self->backend != NULL ? cd_audio_backend_get_streams (self->backend) : NULL;
  gboolean was_present = self->call_present;

  if (!g_settings_get_boolean (self->settings, "enabled"))
    {
      self->call_present = FALSE;
      clear_end_timer (self);
      clear_triggers (self);
      request_reconcile (self);
      return;
    }

  for (guint index = 0; streams != NULL && index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);
      gboolean communication
          = stream->identity.media_role != NULL
            && (g_ascii_strcasecmp (stream->identity.media_role, "communication") == 0
                || g_ascii_strcasecmp (stream->identity.media_role, "phone") == 0);

      if (stream->stable_id != NULL && !cd_is_game (&stream->identity) && stream->running
          && (stream->input || communication) && selector_any (selectors, &stream->identity))
        g_hash_table_replace (
            calls, g_strdup (stream->stable_id),
            g_strdup (stream->display_name != NULL ? stream->display_name : "Unknown application"));
    }
  self->call_present = g_hash_table_size (calls) > 0;
  if (self->call_present)
    {
      clear_end_timer (self);
      if (self->preview_requested || self->preview_active)
        {
          supersede_preview (self);
          set_preview_state (self, FALSE, FALSE);
        }
      update_triggers (self, calls);
      request_reconcile (self);
      return;
    }

  if (self->end_timer != 0 && allow_end_delay)
    return;
  if (was_present && allow_end_delay && g_settings_get_boolean (self->settings, "enabled"))
    {
      if (self->end_timer == 0)
        self->end_timer = g_timeout_add (750, end_call, self);
      return;
    }
  clear_end_timer (self);
  clear_triggers (self);
  request_reconcile (self);
}

static void
engine_changed (CdEngine *engine, gpointer data)
{
  CdDaemon *self = data;
  g_auto (GStrv) triggers = NULL;
  g_auto (GStrv) ducked = NULL;

  if (self->dbus == NULL || self->disposed)
    return;
  triggers = cd_engine_dup_trigger_names (engine);
  ducked = cd_engine_dup_ducked_names (engine);
  cd_dbus_service_update (self->dbus, cd_state_name (cd_engine_get_state (engine)),
                          (const char *const *)triggers, (const char *const *)ducked,
                          cd_engine_get_pending (engine), cd_engine_get_error (engine),
                          self->preview_active);
}

static GVariant *
list_applications (gpointer data)
{
  return cd_application_registry_dup_applications (((CdDaemon *)data)->registry);
}

static void
dbus_preview_async (gpointer data, guint duration_ms, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer callback_data)
{
  cd_daemon_preview_async (data, duration_ms, cancellable, callback, callback_data);
}

static void
dbus_cancel_preview_async (gpointer data, GCancellable *cancellable, GAsyncReadyCallback callback,
                           gpointer callback_data)
{
  cd_daemon_cancel_preview_async (data, cancellable, callback, callback_data);
}

static void
dbus_restore_async (gpointer data, GCancellable *cancellable, GAsyncReadyCallback callback,
                    gpointer callback_data)
{
  cd_daemon_restore_async (data, cancellable, callback, callback_data);
}

static void
volume_changed (CdVolumeController *controller, const GError *error, gpointer data)
{
  CdDaemon *self = data;

  (void)error;
  if (self->disposed)
    return;
  cd_engine_set_pending (self->engine, cd_volume_controller_get_pending (controller));
  cd_engine_set_notice (self->engine, cd_volume_controller_get_notice (controller));
}

static void
refresh_registry (CdDaemon *self)
{
  gboolean changed = FALSE;
  g_autoptr (GError) error = NULL;

  if (!cd_application_registry_refresh (self->registry, &changed, &error))
    {
      if (!self->journal_blocked)
        cd_engine_set_available (self->engine, FALSE, error->message);
      return;
    }
  if (changed && self->dbus != NULL)
    cd_dbus_service_emit_applications_changed (self->dbus);
}

static void
settings_changed (GSettings *settings, char *key, gpointer data)
{
  CdDaemon *self = data;
  gboolean enabled = g_settings_get_boolean (settings, "enabled");

  cd_engine_set_enabled (self->engine, enabled);
  if (!enabled)
    {
      clear_end_timer (self);
      supersede_preview (self);
      set_preview_state (self, FALSE, FALSE);
      g_hash_table_remove_all (self->trigger_snapshot);
      self->call_present = FALSE;
      request_reconcile (self);
    }
  else if (g_str_equal (key, "enabled") && self->snapshot_ready)
    {
      evaluate_snapshot (self, FALSE);
    }
  else if (g_str_equal (key, "call-app-selectors"))
    {
      evaluate_snapshot (self, FALSE);
    }
  else
    {
      request_reconcile (self);
    }
  if (self->snapshot_ready
      && (g_str_equal (key, "call-app-selectors") || g_str_equal (key, "always-duck-selectors")
          || g_str_equal (key, "never-duck-selectors")))
    refresh_registry (self);
}

static void
backend_changed (CdAudioBackend *backend, gpointer data)
{
  CdDaemon *self = data;

  (void)backend;
  self->snapshot_ready = TRUE;
  self->backend_ready = TRUE;
  if (!self->journal_blocked)
    cd_engine_set_available (self->engine, TRUE, "");
  else
    cd_engine_set_available (self->engine, FALSE, self->journal_error);
  refresh_registry (self);
  evaluate_snapshot (self, TRUE);
}

static void
backend_failed (CdAudioBackend *backend, const char *message, gpointer data)
{
  CdDaemon *self = data;

  (void)backend;
  self->backend_ready = FALSE;
  if (!self->journal_blocked)
    cd_engine_set_available (self->engine, FALSE, message);
}

static gboolean
is_missing_file_error (const GError *error)
{
  return g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)
         || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

CdDaemon *
cd_daemon_new (GSettings *settings, CdAudioBackend *backend, CdStateStore *applications_store,
               CdStateStore *restore_store)
{
  static const CdDbusServiceHandlers handlers = {
    .list_applications = list_applications,
    .preview_async = dbus_preview_async,
    .cancel_preview_async = dbus_cancel_preview_async,
    .restore_async = dbus_restore_async,
  };
  CdDaemon *self;
  g_autoptr (GError) error = NULL;

  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (backend == NULL || CD_IS_AUDIO_BACKEND (backend), NULL);
  g_return_val_if_fail (applications_store != NULL, NULL);
  g_return_val_if_fail (restore_store != NULL, NULL);
  self = g_new0 (CdDaemon, 1);
  self->refs = 1;
  self->settings = g_object_ref (settings);
  self->backend = backend != NULL ? g_object_ref (backend) : NULL;
  self->engine = cd_engine_new (engine_changed, self);
  self->registry = cd_application_registry_new (settings, backend, applications_store);
  self->dbus = cd_dbus_service_new (&handlers, self);
  self->trigger_snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->restore_tasks = g_ptr_array_new_with_free_func (g_object_unref);
  cd_engine_set_enabled (self->engine, g_settings_get_boolean (settings, "enabled"));
  if (!cd_application_registry_load (self->registry, &error))
    {
      if (!is_missing_file_error (error))
        g_warning ("Could not load application registry: %s", error->message);
      g_clear_error (&error);
    }
  if (backend != NULL)
    {
      self->volumes = cd_volume_controller_new (backend, restore_store, volume_changed, self);
      if (!cd_volume_controller_load (self->volumes, &error))
        {
          if (!is_missing_file_error (error))
            {
              self->journal_blocked = TRUE;
              self->journal_error = g_strdup (error->message);
              cd_engine_set_available (self->engine, FALSE, self->journal_error);
            }
          g_clear_error (&error);
        }
      update_pending (self);
      g_signal_connect (backend, "changed", G_CALLBACK (backend_changed), self);
      g_signal_connect (backend, "failed", G_CALLBACK (backend_failed), self);
      if (!self->journal_blocked)
        cd_engine_set_available (self->engine, FALSE, "Waiting for the audio service");
    }
  else
    {
      cd_state_store_free (restore_store);
      cd_engine_set_available (self->engine, FALSE, "Audio backend is unavailable");
    }
  g_signal_connect (settings, "changed", G_CALLBACK (settings_changed), self);
  return self;
}

void
cd_daemon_free (CdDaemon *self)
{
  g_autoptr (GError) cancelled = NULL;

  if (self == NULL || self->disposed)
    return;
  self->disposed = TRUE;
  cancelled = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "The daemon is shutting down");
  if (self->preview_timer != 0)
    g_source_remove (self->preview_timer);
  if (self->end_timer != 0)
    g_source_remove (self->end_timer);
  self->preview_timer = 0;
  self->end_timer = 0;
  g_signal_handlers_disconnect_by_data (self->settings, self);
  if (self->backend != NULL)
    g_signal_handlers_disconnect_by_data (self->backend, self);
  complete_preview (self, cancelled);
  complete_restores (self, cancelled);
  g_clear_pointer (&self->dbus, cd_dbus_service_free);
  daemon_unref (self);
}

gboolean
cd_daemon_export (CdDaemon *self, GDBusConnection *connection, const char *object_path,
                  GError **error)
{
  engine_changed (self->engine, self);
  return cd_dbus_service_export (self->dbus, connection, object_path, error);
}

void
cd_daemon_unexport (CdDaemon *self)
{
  g_return_if_fail (self != NULL);
  if (self->dbus != NULL)
    cd_dbus_service_unexport (self->dbus);
}

void
cd_daemon_prepare_shutdown (CdDaemon *self)
{
  g_return_if_fail (self != NULL);
  if (self->shutting_down)
    return;
  self->shutting_down = TRUE;
  cd_daemon_unexport (self);
  g_signal_handlers_disconnect_by_data (self->settings, self);
  if (self->backend != NULL)
    g_signal_handlers_disconnect_by_data (self->backend, self);
  clear_end_timer (self);
  supersede_preview (self);
  set_preview_state (self, FALSE, FALSE);
  self->call_present = FALSE;
  clear_triggers (self);
}

void
cd_daemon_start (CdDaemon *self)
{
  g_return_if_fail (self != NULL);
  if (self->snapshot_ready)
    evaluate_snapshot (self, FALSE);
}

void
cd_daemon_preview_async (CdDaemon *self, guint duration_ms, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GHashTable) names = NULL;
  g_autoptr (GPtrArray) targets = NULL;
  GTask *task;

  g_return_if_fail (self != NULL);
  task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_source_tag (task, cd_daemon_preview_async);
  if (self->disposed || self->shutting_down || self->journal_blocked || !self->backend_ready
      || !g_settings_get_boolean (self->settings, "enabled") || self->call_present
      || cd_engine_get_state (self->engine) != CD_STATE_LISTENING || self->end_timer != 0
      || self->volumes == NULL)
    {
      g_task_return_new_error (task, CD_DAEMON_ERROR, CD_DAEMON_ERROR_PREVIEW_UNAVAILABLE,
                               "No adjustable application is currently available");
      g_object_unref (task);
      return;
    }
  targets = collect_targets (self, &names);
  if (targets->len == 0)
    {
      g_task_return_new_error (task, CD_DAEMON_ERROR, CD_DAEMON_ERROR_PREVIEW_UNAVAILABLE,
                               "No adjustable application is currently available");
      g_object_unref (task);
      return;
    }
  supersede_preview (self);
  if (self->preview_timer != 0)
    {
      g_source_remove (self->preview_timer);
      self->preview_timer = 0;
    }
  self->preview_task = task;
  self->preview_duration_ms = CLAMP (duration_ms, 100, 5000);
  self->preview_requested = TRUE;
  request_reconcile (self);
}

gboolean
cd_daemon_preview_finish (CdDaemon *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == cd_daemon_preview_async, FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cd_daemon_cancel_preview_async (CdDaemon *self, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer data)
{
  GTask *task;
  gboolean had_preview;

  g_return_if_fail (self != NULL);
  task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_source_tag (task, cd_daemon_cancel_preview_async);
  had_preview = self->preview_requested || self->preview_active || self->preview_timer != 0;
  if (!had_preview && !self->restore_barrier)
    {
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }
  supersede_preview (self);
  set_preview_state (self, FALSE, FALSE);
  g_ptr_array_add (self->restore_tasks, task);
  self->restore_barrier = TRUE;
  request_reconcile (self);
}

gboolean
cd_daemon_cancel_preview_finish (CdDaemon *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == cd_daemon_cancel_preview_async,
                        FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cd_daemon_restore_async (CdDaemon *self, GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer data)
{
  GTask *task;

  g_return_if_fail (self != NULL);
  task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_source_tag (task, cd_daemon_restore_async);
  supersede_preview (self);
  set_preview_state (self, FALSE, FALSE);
  if (self->call_present)
    cd_engine_pause_for_call (self->engine);
  g_ptr_array_add (self->restore_tasks, task);
  self->restore_barrier = TRUE;
  request_reconcile (self);
}

gboolean
cd_daemon_restore_finish (CdDaemon *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == cd_daemon_restore_async, FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}
