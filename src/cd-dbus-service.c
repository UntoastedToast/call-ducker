#include "cd-dbus-service.h"
#include "cd-daemon-error.h"

struct _CdDbusService
{
  CdDaemon1 *skeleton;
  CdDbusServiceHandlers handlers;
  gpointer data;
};

typedef enum
{
  METHOD_PREVIEW,
  METHOD_CANCEL_PREVIEW,
  METHOD_RESTORE,
} AsyncMethod;

typedef struct
{
  CdDaemon1 *object;
  GDBusMethodInvocation *invocation;
  AsyncMethod method;
} AsyncInvocation;

static void
async_invocation_free (AsyncInvocation *request)
{
  g_object_unref (request->invocation);
  g_object_unref (request->object);
  g_free (request);
}

static const char *
dbus_error_name (const GError *error)
{
  if (error == NULL)
    return "io.github.UntoastedToast.CallDucker.Error.OperationFailed";
  if (g_error_matches (error, CD_DAEMON_ERROR, CD_DAEMON_ERROR_PREVIEW_UNAVAILABLE))
    return "io.github.UntoastedToast.CallDucker.Error.PreviewUnavailable";
  if (g_error_matches (error, CD_DAEMON_ERROR, CD_DAEMON_ERROR_REQUEST_SUPERSEDED))
    return "io.github.UntoastedToast.CallDucker.Error.Superseded";
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return "io.github.UntoastedToast.CallDucker.Error.Superseded";
  return "io.github.UntoastedToast.CallDucker.Error.OperationFailed";
}

static void
async_method_done (GObject *object, GAsyncResult *result, gpointer data)
{
  AsyncInvocation *request = data;
  g_autoptr (GError) error = NULL;

  (void)object;
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    g_dbus_method_invocation_return_dbus_error (
        request->invocation, dbus_error_name (error),
        error != NULL ? error->message : "The operation could not be completed");
  else if (request->method == METHOD_PREVIEW)
    cd_daemon1_complete_preview (request->object, request->invocation);
  else if (request->method == METHOD_CANCEL_PREVIEW)
    cd_daemon1_complete_cancel_preview (request->object, request->invocation);
  else
    cd_daemon1_complete_restore (request->object, request->invocation);
  async_invocation_free (request);
}

static AsyncInvocation *
async_invocation_new (CdDaemon1 *object, GDBusMethodInvocation *invocation, AsyncMethod method)
{
  AsyncInvocation *request = g_new0 (AsyncInvocation, 1);

  request->object = g_object_ref (object);
  request->invocation = g_object_ref (invocation);
  request->method = method;
  return request;
}

static gboolean
handle_list_applications (CdDaemon1 *object, GDBusMethodInvocation *invocation, gpointer data)
{
  CdDbusService *self = data;
  g_autoptr (GVariant) applications = self->handlers.list_applications (self->data);

  cd_daemon1_complete_list_applications (object, invocation, applications);
  return TRUE;
}

static gboolean
handle_preview (CdDaemon1 *object, GDBusMethodInvocation *invocation, guint duration_ms,
                gpointer data)
{
  CdDbusService *self = data;

  self->handlers.preview_async (self->data, duration_ms, NULL, async_method_done,
                                async_invocation_new (object, invocation, METHOD_PREVIEW));
  return TRUE;
}

static gboolean
handle_cancel_preview (CdDaemon1 *object, GDBusMethodInvocation *invocation, gpointer data)
{
  CdDbusService *self = data;

  self->handlers.cancel_preview_async (
      self->data, NULL, async_method_done,
      async_invocation_new (object, invocation, METHOD_CANCEL_PREVIEW));
  return TRUE;
}

static gboolean
handle_restore (CdDaemon1 *object, GDBusMethodInvocation *invocation, gpointer data)
{
  CdDbusService *self = data;

  self->handlers.restore_async (self->data, NULL, async_method_done,
                                async_invocation_new (object, invocation, METHOD_RESTORE));
  return TRUE;
}

CdDbusService *
cd_dbus_service_new (const CdDbusServiceHandlers *handlers, gpointer data)
{
  CdDbusService *self = g_new0 (CdDbusService, 1);

  self->skeleton = cd_daemon1_skeleton_new ();
  self->handlers = *handlers;
  self->data = data;
  g_signal_connect (self->skeleton, "handle-list-applications",
                    G_CALLBACK (handle_list_applications), self);
  g_signal_connect (self->skeleton, "handle-preview", G_CALLBACK (handle_preview), self);
  g_signal_connect (self->skeleton, "handle-cancel-preview", G_CALLBACK (handle_cancel_preview),
                    self);
  g_signal_connect (self->skeleton, "handle-restore", G_CALLBACK (handle_restore), self);
  return self;
}

void
cd_dbus_service_free (CdDbusService *self)
{
  if (self == NULL)
    return;
  cd_dbus_service_unexport (self);
  g_object_unref (self->skeleton);
  g_free (self);
}

gboolean
cd_dbus_service_export (CdDbusService *self, GDBusConnection *connection, const char *object_path,
                        GError **error)
{
  return g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton), connection,
                                           object_path, error);
}

void
cd_dbus_service_unexport (CdDbusService *self)
{
  GDBusInterfaceSkeleton *skeleton = G_DBUS_INTERFACE_SKELETON (self->skeleton);

  /* Shutdown unexports early; freeing the service must not repeat it. */
  if (g_dbus_interface_skeleton_get_connection (skeleton) == NULL)
    return;
  g_dbus_interface_skeleton_unexport (skeleton);
}

void
cd_dbus_service_update (CdDbusService *self, const char *state, const char *const *trigger_names,
                        const char *const *ducked_names, guint pending, const char *last_error,
                        gboolean preview_active)
{
  g_object_freeze_notify (G_OBJECT (self->skeleton));
  cd_daemon1_set_state (self->skeleton, state);
  cd_daemon1_set_active_trigger_names (self->skeleton, trigger_names);
  cd_daemon1_set_ducked_target_names (self->skeleton, ducked_names);
  cd_daemon1_set_pending_restorations (self->skeleton, pending);
  cd_daemon1_set_last_error (self->skeleton, last_error);
  cd_daemon1_set_preview_active (self->skeleton, preview_active);
  g_object_thaw_notify (G_OBJECT (self->skeleton));
}

void
cd_dbus_service_set_preview_active (CdDbusService *self, gboolean active)
{
  cd_daemon1_set_preview_active (self->skeleton, active);
}

void
cd_dbus_service_emit_applications_changed (CdDbusService *self)
{
  cd_daemon1_emit_applications_changed (self->skeleton);
}
