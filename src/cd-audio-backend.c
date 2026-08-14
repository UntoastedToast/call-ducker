#include "cd-audio-backend.h"

G_DEFINE_INTERFACE (CdAudioBackend, cd_audio_backend, G_TYPE_OBJECT)

static void
cd_audio_backend_default_init (CdAudioBackendInterface *iface)
{
  (void)iface;
  g_signal_new ("changed", CD_TYPE_AUDIO_BACKEND, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 0);
  g_signal_new ("failed", CD_TYPE_AUDIO_BACKEND, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_STRING);
}

void
cd_audio_stream_free (CdAudioStream *stream)
{
  if (stream == NULL)
    return;
  g_free ((char *)stream->identity.portal_id);
  g_free ((char *)stream->identity.application_id);
  g_free ((char *)stream->identity.binary);
  g_free ((char *)stream->identity.name);
  g_free ((char *)stream->identity.media_role);
  g_free (stream->stable_id);
  g_free (stream->restore_id);
  g_free (stream->display_name);
  g_free (stream->icon);
  g_free (stream);
}

GPtrArray *
cd_audio_backend_get_streams (CdAudioBackend *self)
{
  g_return_val_if_fail (CD_IS_AUDIO_BACKEND (self), NULL);
  return CD_AUDIO_BACKEND_GET_IFACE (self)->get_streams (self);
}

void
cd_audio_backend_set_volume_async (CdAudioBackend *self, guint32 id, const char *expected_stable_id,
                                   const char *expected_restore_id,
                                   const pa_channel_map *expected_channel_map,
                                   const pa_cvolume *volume, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail (CD_IS_AUDIO_BACKEND (self));
  g_return_if_fail (expected_stable_id != NULL && *expected_stable_id != '\0');
  g_return_if_fail (expected_channel_map != NULL && pa_channel_map_valid (expected_channel_map));
  CD_AUDIO_BACKEND_GET_IFACE (self)->set_volume_async (self, id, expected_stable_id,
                                                       expected_restore_id, expected_channel_map,
                                                       volume, cancellable, callback, data);
}

gboolean
cd_audio_backend_set_volume_finish (CdAudioBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (CD_IS_AUDIO_BACKEND (self), FALSE);
  return CD_AUDIO_BACKEND_GET_IFACE (self)->set_volume_finish (self, result, error);
}
