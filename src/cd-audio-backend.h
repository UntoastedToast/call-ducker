#pragma once

#include "cd-match.h"

#include <gio/gio.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>

G_BEGIN_DECLS

typedef struct
{
  guint32 id;
  gboolean input;
  gboolean running;
  CdAppIdentity identity;
  char *stable_id;
  char *restore_id;
  char *display_name;
  char *icon;
  pa_cvolume volume;
  pa_channel_map channel_map;
  gboolean volume_controllable;
} CdAudioStream;

void cd_audio_stream_free (CdAudioStream *stream);

#define CD_TYPE_AUDIO_BACKEND (cd_audio_backend_get_type ())
G_DECLARE_INTERFACE (CdAudioBackend, cd_audio_backend, CD, AUDIO_BACKEND, GObject)

struct _CdAudioBackendInterface
{
  GTypeInterface parent_iface;

  GPtrArray *(*get_streams) (CdAudioBackend *self);
  void (*set_volume_async) (CdAudioBackend *self, guint32 id, const char *expected_stable_id,
                            const char *expected_restore_id,
                            const pa_channel_map *expected_channel_map, const pa_cvolume *volume,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data);
  gboolean (*set_volume_finish) (CdAudioBackend *self, GAsyncResult *result, GError **error);
};

GPtrArray *cd_audio_backend_get_streams (CdAudioBackend *self);
void cd_audio_backend_set_volume_async (CdAudioBackend *self, guint32 id,
                                        const char *expected_stable_id,
                                        const char *expected_restore_id,
                                        const pa_channel_map *expected_channel_map,
                                        const pa_cvolume *volume, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer data);
gboolean cd_audio_backend_set_volume_finish (CdAudioBackend *self, GAsyncResult *result,
                                             GError **error);

G_END_DECLS
