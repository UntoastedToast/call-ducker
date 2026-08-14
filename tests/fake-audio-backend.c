#include "fake-audio-backend.h"

struct _TestAudioBackend
{
  GObject parent_instance;
  GPtrArray *streams;
  GHashTable *volumes;
  GQueue pending_sets;
  char *next_failure;
  guint set_count;
  gboolean delayed;
};

typedef struct
{
  GTask *task;
  guint32 id;
  char *expected_stable_id;
  char *expected_restore_id;
  pa_channel_map expected_channel_map;
  pa_cvolume volume;
} PendingSet;

static void audio_backend_iface_init (CdAudioBackendInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (TestAudioBackend, test_audio_backend, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (CD_TYPE_AUDIO_BACKEND,
                                                      audio_backend_iface_init))

static GPtrArray *
get_streams (CdAudioBackend *backend)
{
  return TEST_AUDIO_BACKEND (backend)->streams;
}

static gboolean
set_volume_finish (CdAudioBackend *backend, GAsyncResult *result, GError **error)
{
  (void)backend;
  return g_task_propagate_boolean (G_TASK (result), error);
}

static const char *
normalized_restore_id (const char *restore_id)
{
  return restore_id != NULL ? restore_id : "";
}

static void
pending_set_free (PendingSet *set)
{
  g_object_unref (set->task);
  g_free (set->expected_stable_id);
  g_free (set->expected_restore_id);
  g_free (set);
}

static CdAudioStream *
find_stream (TestAudioBackend *self, guint32 id)
{
  for (guint index = 0; index < self->streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (self->streams, index);

      if (!stream->input && stream->id == id)
        return stream;
    }
  return NULL;
}

static void
complete_set (TestAudioBackend *self, PendingSet *set, gboolean succeed)
{
  CdAudioStream *matched = find_stream (self, set->id);

  if (g_task_return_error_if_cancelled (set->task))
    return;
  if (self->next_failure != NULL)
    {
      g_task_return_new_error (set->task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", self->next_failure);
      g_clear_pointer (&self->next_failure, g_free);
      return;
    }
  if (!succeed)
    {
      g_task_return_new_error (set->task, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected volume failure");
      return;
    }
  if (matched == NULL || !matched->volume_controllable
      || g_strcmp0 (matched->stable_id, set->expected_stable_id) != 0
      || g_strcmp0 (normalized_restore_id (matched->restore_id),
                    normalized_restore_id (set->expected_restore_id))
             != 0
      || !pa_channel_map_equal (&matched->channel_map, &set->expected_channel_map))
    {
      g_task_return_new_error (set->task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Audio stream changed identity");
      return;
    }

  pa_cvolume *stored = g_new (pa_cvolume, 1);

  *stored = set->volume;
  matched->volume = set->volume;
  g_hash_table_replace (self->volumes, GUINT_TO_POINTER (set->id), stored);
  g_task_return_boolean (set->task, TRUE);
}

static void
set_volume_async (CdAudioBackend *backend, guint32 id, const char *expected_stable_id,
                  const char *expected_restore_id, const pa_channel_map *expected_channel_map,
                  const pa_cvolume *volume, GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer data)
{
  TestAudioBackend *self = TEST_AUDIO_BACKEND (backend);
  PendingSet *set = g_new0 (PendingSet, 1);

  set->task = g_task_new (backend, cancellable, callback, data);
  set->id = id;
  set->expected_stable_id = g_strdup (expected_stable_id);
  set->expected_restore_id = g_strdup (normalized_restore_id (expected_restore_id));
  set->expected_channel_map = *expected_channel_map;
  set->volume = *volume;
  self->set_count++;
  if (self->delayed)
    g_queue_push_tail (&self->pending_sets, set);
  else
    {
      complete_set (self, set, TRUE);
      pending_set_free (set);
    }
}

static void
audio_backend_iface_init (CdAudioBackendInterface *iface)
{
  iface->get_streams = get_streams;
  iface->set_volume_async = set_volume_async;
  iface->set_volume_finish = set_volume_finish;
}

static void
test_audio_backend_finalize (GObject *object)
{
  TestAudioBackend *self = TEST_AUDIO_BACKEND (object);

  g_ptr_array_unref (self->streams);
  g_hash_table_unref (self->volumes);
  g_queue_clear_full (&self->pending_sets, (GDestroyNotify)pending_set_free);
  g_free (self->next_failure);
  G_OBJECT_CLASS (test_audio_backend_parent_class)->finalize (object);
}

static void
test_audio_backend_class_init (TestAudioBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_audio_backend_finalize;
}

static void
test_audio_backend_init (TestAudioBackend *self)
{
  self->streams = g_ptr_array_new_with_free_func ((GDestroyNotify)cd_audio_stream_free);
  self->volumes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}

TestAudioBackend *
test_audio_backend_new (void)
{
  return g_object_new (TEST_TYPE_AUDIO_BACKEND, NULL);
}

static pa_cvolume *
make_volume (double value)
{
  pa_cvolume *volume = g_new (pa_cvolume, 1);

  pa_cvolume_set (volume, 1, (pa_volume_t)(value * PA_VOLUME_NORM + 0.5));
  return volume;
}

CdAudioStream *
test_audio_backend_add_stream (TestAudioBackend *self, guint32 id, const char *stable_id,
                               const char *name, gboolean input, gboolean running,
                               const char *application_id, const char *binary,
                               const char *media_role, gboolean launcher_game, double volume)
{
  CdAudioStream *stream = g_new0 (CdAudioStream, 1);

  stream->id = id;
  stream->stable_id = g_strdup (stable_id);
  stream->display_name = g_strdup (name);
  stream->icon = g_strdup ("application-x-executable-symbolic");
  stream->input = input;
  stream->running = running;
  stream->identity.application_id = g_strdup (application_id);
  stream->identity.binary = g_strdup (binary);
  stream->identity.name = g_strdup (name);
  stream->identity.media_role = g_strdup (media_role);
  stream->identity.launcher_game = launcher_game;
  pa_cvolume_set (&stream->volume, 1, (pa_volume_t)(volume * PA_VOLUME_NORM + 0.5));
  pa_channel_map_init_mono (&stream->channel_map);
  stream->volume_controllable = !input;
  g_ptr_array_add (self->streams, stream);
  g_hash_table_replace (self->volumes, GUINT_TO_POINTER (id), make_volume (volume));
  return stream;
}

void
test_audio_backend_clear (TestAudioBackend *self)
{
  g_ptr_array_set_size (self->streams, 0);
}

void
test_audio_backend_changed (TestAudioBackend *self)
{
  g_signal_emit_by_name (self, "changed");
}

void
test_audio_backend_fail (TestAudioBackend *self, const char *message)
{
  g_signal_emit_by_name (self, "failed", message);
}

double
test_audio_backend_get_main_volume (TestAudioBackend *self, guint32 id)
{
  pa_cvolume *volume = g_hash_table_lookup (self->volumes, GUINT_TO_POINTER (id));

  g_assert_nonnull (volume);
  return (double)pa_cvolume_max (volume) / PA_VOLUME_NORM;
}

const pa_cvolume *
test_audio_backend_get_volume (TestAudioBackend *self, guint32 id)
{
  return g_hash_table_lookup (self->volumes, GUINT_TO_POINTER (id));
}

void
test_audio_backend_set_delayed (TestAudioBackend *self, gboolean delayed)
{
  self->delayed = delayed;
}

guint
test_audio_backend_get_pending_sets (TestAudioBackend *self)
{
  return self->pending_sets.length;
}

guint
test_audio_backend_get_set_count (TestAudioBackend *self)
{
  return self->set_count;
}

gboolean
test_audio_backend_complete_next (TestAudioBackend *self, gboolean succeed)
{
  PendingSet *set = g_queue_pop_head (&self->pending_sets);

  if (set == NULL)
    return FALSE;
  complete_set (self, set, succeed);
  pending_set_free (set);
  return TRUE;
}

void
test_audio_backend_fail_next_set (TestAudioBackend *self, const char *message)
{
  g_free (self->next_failure);
  self->next_failure = g_strdup (message != NULL ? message : "Injected volume failure");
}

void
test_audio_backend_set_stream_volume (TestAudioBackend *self, guint32 id, double volume)
{
  CdAudioStream *stream = find_stream (self, id);
  pa_cvolume *stored;

  g_return_if_fail (stream != NULL);
  stored = g_new (pa_cvolume, 1);
  pa_cvolume_set (stored, stream->volume.channels, (pa_volume_t)(volume * PA_VOLUME_NORM + 0.5));
  stream->volume = *stored;
  g_hash_table_replace (self->volumes, GUINT_TO_POINTER (id), stored);
}

void
test_audio_backend_set_stream_layout (TestAudioBackend *self, guint32 id,
                                      const pa_channel_map *channel_map, const pa_cvolume *volume)
{
  CdAudioStream *stream = find_stream (self, id);
  pa_cvolume *stored;

  g_return_if_fail (stream != NULL);
  g_return_if_fail (pa_channel_map_valid (channel_map));
  g_return_if_fail (pa_cvolume_valid (volume));
  g_return_if_fail (channel_map->channels == volume->channels);
  stored = g_new (pa_cvolume, 1);
  *stored = *volume;
  stream->channel_map = *channel_map;
  stream->volume = *volume;
  g_hash_table_replace (self->volumes, GUINT_TO_POINTER (id), stored);
}
