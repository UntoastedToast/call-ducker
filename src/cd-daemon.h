#pragma once

#include "cd-audio-backend.h"
#include "cd-daemon-error.h"
#include "cd-state-store.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _CdDaemon CdDaemon;

CdDaemon *cd_daemon_new (GSettings *settings, CdAudioBackend *backend,
                         CdStateStore *applications_store, CdStateStore *restore_store);
void cd_daemon_free (CdDaemon *self);
gboolean cd_daemon_export (CdDaemon *self, GDBusConnection *connection, const char *object_path,
                           GError **error);
void cd_daemon_unexport (CdDaemon *self);
void cd_daemon_prepare_shutdown (CdDaemon *self);
void cd_daemon_start (CdDaemon *self);
void cd_daemon_preview_async (CdDaemon *self, guint duration_ms, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer data);
gboolean cd_daemon_preview_finish (CdDaemon *self, GAsyncResult *result, GError **error);
void cd_daemon_cancel_preview_async (CdDaemon *self, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer data);
gboolean cd_daemon_cancel_preview_finish (CdDaemon *self, GAsyncResult *result, GError **error);
void cd_daemon_restore_async (CdDaemon *self, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer data);
gboolean cd_daemon_restore_finish (CdDaemon *self, GAsyncResult *result, GError **error);

G_END_DECLS
