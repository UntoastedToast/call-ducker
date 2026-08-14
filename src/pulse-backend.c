#include "pulse-backend.h"

#include "cd-process.h"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#define SNAPSHOT_DELAY_MS 50
#define RECONNECT_MAX_SECONDS 30

struct _CdPulseBackend
{
  GObject parent_instance;
  pa_glib_mainloop *mainloop;
  pa_context *context;
  pa_operation *subscribe_operation;
  GPtrArray *streams;
  struct _Snapshot *snapshot;
  guint snapshot_source;
  guint reconnect_source;
  guint reconnect_seconds;
  GList *pending_sets;
  gboolean subscribed;
  gboolean snapshot_current;
  gboolean disposed;
};

typedef struct _Snapshot
{
  CdPulseBackend *backend;
  GPtrArray *streams;
  pa_operation *sink_query;
  pa_operation *source_query;
  guint pending;
} Snapshot;

typedef struct
{
  CdPulseBackend *backend;
  GTask *task;
  pa_operation *operation;
  char *expected_stable_id;
  char *expected_restore_id;
  guint32 id;
  pa_channel_map expected_channel_map;
  pa_cvolume volume;
  gboolean saw_info;
  guint validation_source;
  GError *validation_error;
} PendingSet;

static void audio_backend_iface_init (CdAudioBackendInterface *iface);
static void connect_server (CdPulseBackend *self);
static void schedule_snapshot (CdPulseBackend *self);
static void schedule_reconnect (CdPulseBackend *self, const char *message);

G_DEFINE_FINAL_TYPE_WITH_CODE (CdPulseBackend, cd_pulse_backend, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (CD_TYPE_AUDIO_BACKEND,
                                                      audio_backend_iface_init))

static const char *
prop (pa_proplist *properties, const char *key)
{
  return properties != NULL ? pa_proplist_gets (properties, key) : NULL;
}

static char *
dup_prop (pa_proplist *properties, const char *key)
{
  return g_strdup (prop (properties, key));
}

static const char *
primary_identity (pa_proplist *properties, const char **kind)
{
  const char *value = prop (properties, "pipewire.access.portal.app_id");

  if (value != NULL && *value != '\0')
    {
      *kind = "portal";
      return value;
    }
  value = prop (properties, PA_PROP_APPLICATION_ID);
  if (value != NULL && *value != '\0')
    {
      *kind = "application";
      return value;
    }
  value = prop (properties, PA_PROP_APPLICATION_PROCESS_BINARY);
  if (value != NULL && *value != '\0')
    {
      *kind = "binary";
      return value;
    }
  value = prop (properties, PA_PROP_APPLICATION_NAME);
  if (value != NULL && *value != '\0')
    {
      *kind = "name";
      return value;
    }
  *kind = "name";
  return "unknown";
}

static char *
stable_id (pa_proplist *properties)
{
  const char *kind;
  const char *value = primary_identity (properties, &kind);

  return g_strdup_printf ("%s:%s", kind, value);
}

static CdAudioStream *
stream_new (guint32 id, gboolean input, gboolean corked, pa_proplist *properties,
            const pa_cvolume *volume, const pa_channel_map *channel_map, gboolean has_volume,
            gboolean volume_writable)
{
  CdAudioStream *stream = g_new0 (CdAudioStream, 1);
  const char *portal = prop (properties, "pipewire.access.portal.app_id");
  const char *application = prop (properties, PA_PROP_APPLICATION_ID);
  const char *binary = prop (properties, PA_PROP_APPLICATION_PROCESS_BINARY);
  const char *name = prop (properties, PA_PROP_APPLICATION_NAME);
  const char *kind;
  const char *identity;
  const char *pid_text;
  guint pid;

  stream->id = id;
  stream->input = input;
  stream->running = !corked;
  pa_cvolume_init (&stream->volume);
  pa_channel_map_init (&stream->channel_map);
  if (has_volume && volume != NULL && pa_cvolume_valid (volume))
    stream->volume = *volume;
  if (channel_map != NULL && pa_channel_map_valid (channel_map))
    stream->channel_map = *channel_map;
  stream->volume_controllable
      = !input && has_volume && volume_writable && pa_cvolume_valid (&stream->volume)
        && pa_channel_map_valid (&stream->channel_map)
        && pa_cvolume_compatible_with_channel_map (&stream->volume, &stream->channel_map);
  stream->identity.portal_id = g_strdup (portal);
  stream->identity.application_id = g_strdup (application);
  stream->identity.binary = g_strdup (binary);
  stream->identity.name = g_strdup (name);
  stream->identity.media_role = dup_prop (properties, PA_PROP_MEDIA_ROLE);
  pid_text = prop (properties, PA_PROP_APPLICATION_PROCESS_ID);
  pid = pid_text != NULL ? (guint)g_ascii_strtoull (pid_text, NULL, 10) : 0;
  stream->identity.launcher_game = pid != 0 && cd_process_has_launcher_game (pid);
  identity = primary_identity (properties, &kind);
  stream->stable_id = g_strdup_printf ("%s:%s", kind, identity);
  stream->restore_id = dup_prop (properties, "module-stream-restore.id");
  stream->display_name = g_strdup (name != NULL && *name != '\0' ? name : identity);
  stream->icon = dup_prop (properties, PA_PROP_APPLICATION_ICON_NAME);
  return stream;
}

static void
snapshot_free (Snapshot *snapshot)
{
  if (snapshot == NULL)
    return;
  if (snapshot->sink_query != NULL)
    pa_operation_cancel (snapshot->sink_query);
  if (snapshot->source_query != NULL)
    pa_operation_cancel (snapshot->source_query);
  g_clear_pointer (&snapshot->sink_query, pa_operation_unref);
  g_clear_pointer (&snapshot->source_query, pa_operation_unref);
  g_clear_pointer (&snapshot->streams, g_ptr_array_unref);
  g_free (snapshot);
}

static void
cancel_snapshot (CdPulseBackend *self)
{
  Snapshot *snapshot = g_steal_pointer (&self->snapshot);

  snapshot_free (snapshot);
}

static void
query_complete (Snapshot *snapshot, gboolean sink, gboolean success, pa_context *context)
{
  CdPulseBackend *self = snapshot->backend;
  pa_operation **operation = sink ? &snapshot->sink_query : &snapshot->source_query;

  if (self->snapshot != snapshot || self->context != context)
    return;
  g_clear_pointer (operation, pa_operation_unref);
  if (!success)
    {
      g_autofree char *message = g_strdup_printf ("Could not read PulseAudio streams: %s",
                                                  pa_strerror (pa_context_errno (context)));

      schedule_reconnect (self, message);
      return;
    }
  g_assert (snapshot->pending > 0);
  snapshot->pending--;
  if (snapshot->pending != 0)
    return;

  self->snapshot = NULL;
  g_ptr_array_unref (self->streams);
  self->streams = g_steal_pointer (&snapshot->streams);
  snapshot_free (snapshot);
  self->snapshot_current = TRUE;
  self->reconnect_seconds = 1;
  g_signal_emit_by_name (self, "changed");
}

static void
sink_info (pa_context *context, const pa_sink_input_info *info, int eol, void *data)
{
  Snapshot *snapshot = data;

  if (snapshot->backend->snapshot != snapshot || snapshot->backend->context != context)
    return;
  if (eol != 0)
    {
      query_complete (snapshot, TRUE, eol > 0, context);
      return;
    }
  if (info == NULL)
    {
      query_complete (snapshot, TRUE, FALSE, context);
      return;
    }
  g_ptr_array_add (snapshot->streams,
                   stream_new (info->index, FALSE, info->corked, info->proplist, &info->volume,
                               &info->channel_map, info->has_volume, info->volume_writable));
}

static void
source_info (pa_context *context, const pa_source_output_info *info, int eol, void *data)
{
  Snapshot *snapshot = data;

  if (snapshot->backend->snapshot != snapshot || snapshot->backend->context != context)
    return;
  if (eol != 0)
    {
      query_complete (snapshot, FALSE, eol > 0, context);
      return;
    }
  if (info == NULL)
    {
      query_complete (snapshot, FALSE, FALSE, context);
      return;
    }
  g_ptr_array_add (snapshot->streams,
                   stream_new (info->index, TRUE, info->corked, info->proplist, &info->volume,
                               &info->channel_map, info->has_volume, info->volume_writable));
}

static gboolean
snapshot_now (gpointer data)
{
  CdPulseBackend *self = data;
  Snapshot *snapshot;

  self->snapshot_source = 0;
  if (self->context == NULL || pa_context_get_state (self->context) != PA_CONTEXT_READY
      || !self->subscribed)
    return G_SOURCE_REMOVE;
  cancel_snapshot (self);
  snapshot = g_new0 (Snapshot, 1);
  snapshot->backend = self;
  snapshot->streams = g_ptr_array_new_with_free_func ((GDestroyNotify)cd_audio_stream_free);
  snapshot->pending = 2;
  self->snapshot = snapshot;
  snapshot->sink_query = pa_context_get_sink_input_info_list (self->context, sink_info, snapshot);
  if (snapshot->sink_query == NULL)
    {
      g_autofree char *message
          = g_strdup_printf ("Could not request PulseAudio playback streams: %s",
                             pa_strerror (pa_context_errno (self->context)));

      schedule_reconnect (self, message);
      return G_SOURCE_REMOVE;
    }
  snapshot->source_query
      = pa_context_get_source_output_info_list (self->context, source_info, snapshot);
  if (snapshot->source_query == NULL)
    {
      g_autofree char *message
          = g_strdup_printf ("Could not request PulseAudio recording streams: %s",
                             pa_strerror (pa_context_errno (self->context)));

      schedule_reconnect (self, message);
    }
  return G_SOURCE_REMOVE;
}

static void
schedule_snapshot (CdPulseBackend *self)
{
  if (self->snapshot_source == 0)
    self->snapshot_source = g_timeout_add (SNAPSHOT_DELAY_MS, snapshot_now, self);
}

static void
subscription_event (pa_context *context, pa_subscription_event_type_t type, uint32_t index,
                    void *data)
{
  pa_subscription_event_type_t facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  CdPulseBackend *self = data;

  (void)index;
  if (context == self->context && self->subscribed
      && (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT
          || facility == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT))
    schedule_snapshot (self);
}

static gboolean
reconnect_now (gpointer data)
{
  CdPulseBackend *self = data;

  self->reconnect_source = 0;
  connect_server (self);
  return G_SOURCE_REMOVE;
}

static void
pending_set_free (PendingSet *pending)
{
  if (pending->validation_source != 0)
    g_source_remove (pending->validation_source);
  g_clear_pointer (&pending->operation, pa_operation_unref);
  g_clear_object (&pending->task);
  g_clear_error (&pending->validation_error);
  g_free (pending->expected_stable_id);
  g_free (pending->expected_restore_id);
  g_free (pending);
}

static void
pending_set_return_error (PendingSet *pending, GError *error)
{
  pending->backend->pending_sets = g_list_remove (pending->backend->pending_sets, pending);
  g_task_return_error (pending->task, error);
  pending_set_free (pending);
}

static void
disconnect_context (CdPulseBackend *self)
{
  if (self->snapshot_source != 0)
    g_source_remove (self->snapshot_source);
  self->snapshot_source = 0;
  self->subscribed = FALSE;
  self->snapshot_current = FALSE;
  while (self->pending_sets != NULL)
    {
      PendingSet *pending = self->pending_sets->data;

      self->pending_sets = g_list_delete_link (self->pending_sets, self->pending_sets);
      pa_operation_cancel (pending->operation);
      g_clear_pointer (&pending->operation, pa_operation_unref);
      g_task_return_new_error (pending->task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                               "PulseAudio connection was lost");
      pending_set_free (pending);
    }
  cancel_snapshot (self);
  if (self->subscribe_operation != NULL)
    pa_operation_cancel (self->subscribe_operation);
  g_clear_pointer (&self->subscribe_operation, pa_operation_unref);
  if (self->context != NULL)
    {
      pa_context_set_state_callback (self->context, NULL, NULL);
      pa_context_set_subscribe_callback (self->context, NULL, NULL);
      pa_context_disconnect (self->context);
      pa_context_unref (g_steal_pointer (&self->context));
    }
}

static void
schedule_reconnect (CdPulseBackend *self, const char *message)
{
  if (self->disposed || self->reconnect_source != 0)
    return;
  g_signal_emit_by_name (self, "failed", message);
  disconnect_context (self);
  self->reconnect_source = g_timeout_add_seconds (self->reconnect_seconds, reconnect_now, self);
  self->reconnect_seconds = MIN (self->reconnect_seconds * 2, RECONNECT_MAX_SECONDS);
}

static void
subscription_done (pa_context *context, int success, void *data)
{
  CdPulseBackend *self = data;

  if (self->context != context)
    return;
  g_clear_pointer (&self->subscribe_operation, pa_operation_unref);
  if (!success)
    {
      g_autofree char *message
          = g_strdup_printf ("Could not subscribe to PulseAudio stream changes: %s",
                             pa_strerror (pa_context_errno (context)));

      schedule_reconnect (self, message);
      return;
    }
  self->subscribed = TRUE;
  schedule_snapshot (self);
}

static void
context_state (pa_context *context, void *data)
{
  CdPulseBackend *self = data;
  pa_context_state_t state = pa_context_get_state (context);

  if (state == PA_CONTEXT_READY)
    {
      pa_context_set_subscribe_callback (context, subscription_event, self);
      self->subscribe_operation = pa_context_subscribe (
          context, PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT,
          subscription_done, self);
      if (self->subscribe_operation == NULL)
        {
          g_autofree char *message
              = g_strdup_printf ("Could not request PulseAudio subscriptions: %s",
                                 pa_strerror (pa_context_errno (context)));

          schedule_reconnect (self, message);
        }
    }
  else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
    {
      const char *message = state == PA_CONTEXT_FAILED ? pa_strerror (pa_context_errno (context))
                                                       : "PulseAudio connection terminated";
      schedule_reconnect (self, message);
    }
}

static void
connect_server (CdPulseBackend *self)
{
  pa_proplist *properties;

  disconnect_context (self);
  properties = pa_proplist_new ();
  pa_proplist_sets (properties, PA_PROP_APPLICATION_NAME, "CallDucker");
  pa_proplist_sets (properties, PA_PROP_APPLICATION_ID, "io.github.UntoastedToast.CallDucker");
  self->context = pa_context_new_with_proplist (pa_glib_mainloop_get_api (self->mainloop),
                                                "CallDucker", properties);
  pa_proplist_free (properties);
  if (self->context == NULL)
    {
      schedule_reconnect (self, "Could not create PulseAudio context");
      return;
    }
  pa_context_set_state_callback (self->context, context_state, self);
  if (pa_context_connect (self->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
    schedule_reconnect (self, pa_strerror (pa_context_errno (self->context)));
}

static GPtrArray *
backend_get_streams (CdAudioBackend *backend)
{
  return CD_PULSE_BACKEND (backend)->streams;
}

static CdAudioStream *
find_playback_stream (CdPulseBackend *self, guint32 id)
{
  for (guint index = 0; index < self->streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (self->streams, index);

      if (!stream->input && stream->id == id)
        return stream;
    }
  return NULL;
}

static const char *
nonempty (const char *value)
{
  return value != NULL && *value != '\0' ? value : NULL;
}

static void
set_volume_done (pa_context *context, int success, void *data)
{
  PendingSet *pending = data;

  g_clear_pointer (&pending->operation, pa_operation_unref);
  pending->backend->pending_sets = g_list_remove (pending->backend->pending_sets, pending);
  if (success)
    g_task_return_boolean (pending->task, TRUE);
  else
    g_task_return_new_error (pending->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "PulseAudio rejected volume: %s",
                             pa_strerror (pa_context_errno (context)));
  pending_set_free (pending);
}

static void set_stream_info (pa_context *context, const pa_sink_input_info *info, int eol,
                             void *data);

static gboolean
dispatch_validated_set (gpointer data)
{
  PendingSet *pending = data;
  pa_operation_state_t state = pa_operation_get_state (pending->operation);

  if (state == PA_OPERATION_RUNNING)
    return G_SOURCE_CONTINUE;
  pending->validation_source = 0;
  g_clear_pointer (&pending->operation, pa_operation_unref);
  if (state == PA_OPERATION_CANCELLED && pending->validation_error == NULL)
    g_set_error_literal (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         "PulseAudio stream identity check was cancelled");
  if (!pending->saw_info && pending->validation_error == NULL)
    g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "PulseAudio playback stream %u no longer exists", pending->id);
  if (pending->validation_error != NULL)
    {
      GError *error = g_steal_pointer (&pending->validation_error);

      pending_set_return_error (pending, error);
      return G_SOURCE_REMOVE;
    }
  if (g_task_return_error_if_cancelled (pending->task))
    {
      pending->backend->pending_sets = g_list_remove (pending->backend->pending_sets, pending);
      pending_set_free (pending);
      return G_SOURCE_REMOVE;
    }
  if (pending->backend->context == NULL
      || pa_context_get_state (pending->backend->context) != PA_CONTEXT_READY)
    {
      pending_set_return_error (pending, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                                              "PulseAudio connection was lost"));
      return G_SOURCE_REMOVE;
    }
  pending->operation = pa_context_set_sink_input_volume (
      pending->backend->context, pending->id, &pending->volume, set_volume_done, pending);
  if (pending->operation == NULL)
    pending_set_return_error (
        pending, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "Could not request volume change: %s",
                              pa_strerror (pa_context_errno (pending->backend->context))));
  return G_SOURCE_REMOVE;
}

static void
set_stream_info (pa_context *context, const pa_sink_input_info *info, int eol, void *data)
{
  PendingSet *pending = data;

  if (eol == 0)
    {
      g_autofree char *actual_stable_id = NULL;
      const char *actual_restore_id;

      if (info == NULL || pending->saw_info)
        {
          if (pending->validation_error == NULL)
            g_set_error_literal (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "PulseAudio returned invalid stream information");
          goto schedule_dispatch;
        }
      pending->saw_info = TRUE;
      actual_stable_id = stable_id (info->proplist);
      actual_restore_id = nonempty (prop (info->proplist, "module-stream-restore.id"));
      if (info->index != pending->id
          || g_strcmp0 (actual_stable_id, pending->expected_stable_id) != 0
          || g_strcmp0 (actual_restore_id, nonempty (pending->expected_restore_id)) != 0)
        g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "PulseAudio playback stream %u changed identity", pending->id);
      else if (!info->has_volume || !info->volume_writable)
        g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "PulseAudio playback stream %u has no writable volume", pending->id);
      else if (!pa_cvolume_valid (&info->volume) || !pa_channel_map_valid (&info->channel_map)
               || !pa_cvolume_compatible_with_channel_map (&info->volume, &info->channel_map))
        g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "PulseAudio playback stream %u has an invalid channel layout", pending->id);
      else if (!pa_channel_map_equal (&info->channel_map, &pending->expected_channel_map))
        g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "PulseAudio playback stream %u changed channel layout", pending->id);
      else if (!pa_cvolume_compatible_with_channel_map (&pending->volume, &info->channel_map))
        g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Volume does not match PulseAudio playback stream %u", pending->id);
      goto schedule_dispatch;
    }
  if (eol < 0)
    {
      if (pending->validation_error == NULL)
        g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Could not inspect PulseAudio playback stream %u: %s", pending->id,
                     pa_strerror (pa_context_errno (context)));
    }
  else if (!pending->saw_info && pending->validation_error == NULL)
    g_set_error (&pending->validation_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "PulseAudio playback stream %u no longer exists", pending->id);

schedule_dispatch:
  if (pending->validation_source == 0)
    pending->validation_source = g_idle_add (dispatch_validated_set, pending);
}

static void
backend_set_volume_async (CdAudioBackend *backend, guint32 id, const char *expected_stable_id,
                          const char *expected_restore_id,
                          const pa_channel_map *expected_channel_map, const pa_cvolume *volume,
                          GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  CdPulseBackend *self = CD_PULSE_BACKEND (backend);
  GTask *task = g_task_new (backend, cancellable, callback, data);
  PendingSet *pending;
  CdAudioStream *stream;

  if (g_task_return_error_if_cancelled (task))
    {
      g_object_unref (task);
      return;
    }
  if (self->context == NULL || pa_context_get_state (self->context) != PA_CONTEXT_READY
      || !self->snapshot_current)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                               "PulseAudio server is not connected");
      g_object_unref (task);
      return;
    }
  stream = find_playback_stream (self, id);
  if (stream == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "PulseAudio playback stream %u no longer exists", id);
      g_object_unref (task);
      return;
    }
  if (expected_stable_id == NULL || *expected_stable_id == '\0'
      || g_strcmp0 (stream->stable_id, expected_stable_id) != 0
      || g_strcmp0 (nonempty (stream->restore_id), nonempty (expected_restore_id)) != 0)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "PulseAudio playback stream %u changed identity", id);
      g_object_unref (task);
      return;
    }
  if (!stream->volume_controllable)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "PulseAudio playback stream %u has no writable volume", id);
      g_object_unref (task);
      return;
    }
  if (expected_channel_map == NULL || !pa_channel_map_valid (expected_channel_map))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "Expected channel map is invalid for PulseAudio playback stream %u",
                               id);
      g_object_unref (task);
      return;
    }
  if (!pa_channel_map_equal (&stream->channel_map, expected_channel_map))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "PulseAudio playback stream %u changed channel layout", id);
      g_object_unref (task);
      return;
    }
  if (volume == NULL || !pa_cvolume_valid (volume)
      || !pa_cvolume_compatible_with_channel_map (volume, expected_channel_map))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "Volume does not match PulseAudio playback stream %u", id);
      g_object_unref (task);
      return;
    }
  pending = g_new0 (PendingSet, 1);
  pending->backend = self;
  pending->task = task;
  pending->id = id;
  pending->expected_stable_id = g_strdup (expected_stable_id);
  pending->expected_restore_id = g_strdup (expected_restore_id);
  pending->expected_channel_map = *expected_channel_map;
  pending->volume = *volume;
  pending->operation = pa_context_get_sink_input_info (self->context, id, set_stream_info, pending);
  if (pending->operation == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Could not request stream identity check: %s",
                               pa_strerror (pa_context_errno (self->context)));
      pending_set_free (pending);
      return;
    }
  self->pending_sets = g_list_prepend (self->pending_sets, pending);
}

static gboolean
backend_set_volume_finish (CdAudioBackend *backend, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, backend), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
audio_backend_iface_init (CdAudioBackendInterface *iface)
{
  iface->get_streams = backend_get_streams;
  iface->set_volume_async = backend_set_volume_async;
  iface->set_volume_finish = backend_set_volume_finish;
}

static void
cd_pulse_backend_dispose (GObject *object)
{
  CdPulseBackend *self = CD_PULSE_BACKEND (object);

  self->disposed = TRUE;
  if (self->snapshot_source != 0)
    g_source_remove (self->snapshot_source);
  if (self->reconnect_source != 0)
    g_source_remove (self->reconnect_source);
  self->snapshot_source = 0;
  self->reconnect_source = 0;
  disconnect_context (self);
  G_OBJECT_CLASS (cd_pulse_backend_parent_class)->dispose (object);
}

static void
cd_pulse_backend_finalize (GObject *object)
{
  CdPulseBackend *self = CD_PULSE_BACKEND (object);

  g_ptr_array_unref (self->streams);
  pa_glib_mainloop_free (self->mainloop);
  G_OBJECT_CLASS (cd_pulse_backend_parent_class)->finalize (object);
}

static void
cd_pulse_backend_class_init (CdPulseBackendClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cd_pulse_backend_dispose;
  object_class->finalize = cd_pulse_backend_finalize;
}

static void
cd_pulse_backend_init (CdPulseBackend *self)
{
  self->streams = g_ptr_array_new_with_free_func ((GDestroyNotify)cd_audio_stream_free);
  self->mainloop = pa_glib_mainloop_new (NULL);
  self->reconnect_seconds = 1;
}

CdPulseBackend *
cd_pulse_backend_new (void)
{
  CdPulseBackend *self = g_object_new (CD_TYPE_PULSE_BACKEND, NULL);

  connect_server (self);
  return self;
}
