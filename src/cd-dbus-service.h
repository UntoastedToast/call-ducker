#pragma once

#include "cd-dbus.h"

typedef struct _CdDbusService CdDbusService;

typedef struct
{
  GVariant *(*list_applications) (gpointer data);
  void (*preview_async) (gpointer data, guint duration_ms, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer callback_data);
  void (*cancel_preview_async) (gpointer data, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer callback_data);
  void (*restore_async) (gpointer data, GCancellable *cancellable, GAsyncReadyCallback callback,
                         gpointer callback_data);
} CdDbusServiceHandlers;

CdDbusService *cd_dbus_service_new (const CdDbusServiceHandlers *handlers, gpointer data);
void cd_dbus_service_free (CdDbusService *self);
gboolean cd_dbus_service_export (CdDbusService *self, GDBusConnection *connection,
                                 const char *object_path, GError **error);
void cd_dbus_service_unexport (CdDbusService *self);
void cd_dbus_service_update (CdDbusService *self, const char *state,
                             const char *const *trigger_names, const char *const *ducked_names,
                             guint pending, const char *last_error, gboolean preview_active);
void cd_dbus_service_set_preview_active (CdDbusService *self, gboolean active);
void cd_dbus_service_emit_applications_changed (CdDbusService *self);
