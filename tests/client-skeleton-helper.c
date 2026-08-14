#include "cd-dbus-service.h"

#include <glib-unix.h>

#define BUS_NAME "io.github.UntoastedToast.CallDucker.Daemon"
#define OBJECT_PATH "/io/github/UntoastedToast/CallDucker/Daemon"

typedef struct
{
  GMainLoop *loop;
  CdDbusService *service;
} Helper;

static GVariant *
list_applications (gpointer data)
{
  GVariantBuilder list;
  GVariantBuilder application;

  (void)data;
  g_variant_builder_init (&list, G_VARIANT_TYPE ("aa{sv}"));
  g_variant_builder_init (&application, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&application, "{sv}", "id", g_variant_new_string ("application:test"));
  g_variant_builder_add (&application, "{sv}", "name", g_variant_new_string ("Test App"));
  g_variant_builder_add (&application, "{sv}", "icon",
                         g_variant_new_string ("application-x-executable-symbolic"));
  g_variant_builder_add (&application, "{sv}", "input", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&application, "{sv}", "game", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&application, "{sv}", "available", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&application, "{sv}", "lastSeen", g_variant_new_int64 (1));
  g_variant_builder_add_value (&list, g_variant_builder_end (&application));
  return g_variant_ref_sink (g_variant_builder_end (&list));
}

static void
complete_success (GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, data);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
preview_async (gpointer data, guint duration_ms, GCancellable *cancellable,
               GAsyncReadyCallback callback, gpointer callback_data)
{
  Helper *helper = data;
  const char *empty[] = { NULL };

  (void)duration_ms;
  cd_dbus_service_update (helper->service, "Listening", empty, empty, 0, "", TRUE);
  cd_dbus_service_emit_applications_changed (helper->service);
  complete_success (cancellable, callback, callback_data);
}

static void
cancel_preview_async (gpointer data, GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer callback_data)
{
  Helper *helper = data;
  const char *empty[] = { NULL };

  cd_dbus_service_update (helper->service, "Listening", empty, empty, 0, "", FALSE);
  complete_success (cancellable, callback, callback_data);
}

static void
restore_async (gpointer data, GCancellable *cancellable, GAsyncReadyCallback callback,
               gpointer callback_data)
{
  Helper *helper = data;
  const char *empty[] = { NULL };

  cd_dbus_service_update (helper->service, "Listening", empty, empty, 0, "", FALSE);
  complete_success (cancellable, callback, callback_data);
}

static gboolean
stop (gpointer data)
{
  g_main_loop_quit (((Helper *)data)->loop);
  return G_SOURCE_REMOVE;
}

int
main (void)
{
  const CdDbusServiceHandlers handlers = {
    .list_applications = list_applications,
    .preview_async = preview_async,
    .cancel_preview_async = cancel_preview_async,
    .restore_async = restore_async,
  };
  Helper helper = { .loop = g_main_loop_new (NULL, FALSE) };
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;
  const char *empty[] = { NULL };
  guint owner;
  guint sigterm;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    return 2;
  helper.service = cd_dbus_service_new (&handlers, &helper);
  cd_dbus_service_update (helper.service, "Listening", empty, empty, 0, "", FALSE);
  if (!cd_dbus_service_export (helper.service, connection, OBJECT_PATH, &error))
    return 3;
  owner = g_bus_own_name_on_connection (connection, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
                                        NULL, NULL, NULL);
  sigterm = g_unix_signal_add (SIGTERM, stop, &helper);
  g_main_loop_run (helper.loop);
  g_source_remove (sigterm);
  g_bus_unown_name (owner);
  cd_dbus_service_free (helper.service);
  g_main_loop_unref (helper.loop);
  return 0;
}
