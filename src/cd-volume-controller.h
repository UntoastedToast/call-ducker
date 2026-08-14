#pragma once

#include "cd-audio-backend.h"
#include "cd-state-store.h"

G_BEGIN_DECLS

typedef struct _CdVolumeController CdVolumeController;
typedef void (*CdVolumeControllerChanged) (CdVolumeController *self, const GError *error,
                                           gpointer data);

CdVolumeController *cd_volume_controller_new (CdAudioBackend *backend, CdStateStore *store,
                                              CdVolumeControllerChanged changed, gpointer data);
void cd_volume_controller_free (CdVolumeController *self);
gboolean cd_volume_controller_load (CdVolumeController *self, GError **error);

void cd_volume_controller_reconcile_async (CdVolumeController *self, GPtrArray *targets,
                                           double target, GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer data);
gboolean cd_volume_controller_reconcile_finish (CdVolumeController *self, GAsyncResult *result,
                                                GError **error);
void cd_volume_controller_restore_all_async (CdVolumeController *self, GCancellable *cancellable,
                                             GAsyncReadyCallback callback, gpointer data);
gboolean cd_volume_controller_restore_all_finish (CdVolumeController *self, GAsyncResult *result,
                                                  GError **error);

guint cd_volume_controller_get_pending (CdVolumeController *self);
pa_cvolume cd_volume_absolute (const pa_cvolume *original, double target);

G_END_DECLS
