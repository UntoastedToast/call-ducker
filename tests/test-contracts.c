#include "cd-engine.h"

#include <gio/gio.h>

static void
state_strings (void)
{
  g_assert_cmpstr (cd_state_name (CD_STATE_DISABLED), ==, "Disabled");
  g_assert_cmpstr (cd_state_name (CD_STATE_LISTENING), ==, "Listening");
  g_assert_cmpstr (cd_state_name (CD_STATE_CALL_ACTIVE), ==, "Call active");
  g_assert_cmpstr (cd_state_name (CD_STATE_UNAVAILABLE), ==, "Audio service unavailable");
}

static void
dbus_contract (void)
{
  g_autofree char *path = g_build_filename (
      SOURCE_ROOT, "data", "io.github.UntoastedToast.CallDucker.Daemon1.xml", NULL);
  g_autofree char *xml = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusNodeInfo) node = NULL;
  GDBusInterfaceInfo *interface;
  static const struct
  {
    const char *name;
    const char *signature;
  } properties[] = {
    { "State", "s" },
    { "ActiveTriggerNames", "as" },
    { "DuckedTargetNames", "as" },
    { "PendingRestorations", "u" },
    { "LastError", "s" },
    { "PreviewActive", "b" },
  };
  static const struct
  {
    const char *name;
    const char *in_name;
    const char *in_signature;
    const char *out_name;
    const char *out_signature;
  } methods[] = {
    { "ListApplications", NULL, NULL, "applications", "aa{sv}" },
    { "Preview", "duration_ms", "u", NULL, NULL },
    { "CancelPreview", NULL, NULL, NULL, NULL },
    { "Restore", NULL, NULL, NULL, NULL },
  };
  guint count;

  g_assert_true (g_file_get_contents (path, &xml, NULL, &error));
  node = g_dbus_node_info_new_for_xml (xml, &error);
  g_assert_no_error (error);
  interface = g_dbus_node_info_lookup_interface (node,
                                                 "io.github.UntoastedToast.CallDucker.Daemon1");
  g_assert_nonnull (interface);
  for (count = 0; interface->properties[count] != NULL; count++)
    ;
  g_assert_cmpuint (count, ==, G_N_ELEMENTS (properties));
  for (guint index = 0; index < G_N_ELEMENTS (properties); index++)
    {
      GDBusPropertyInfo *property
          = g_dbus_interface_info_lookup_property (interface, properties[index].name);

      g_assert_nonnull (property);
      g_assert_cmpstr (property->signature, ==, properties[index].signature);
      g_assert_cmpint (property->flags, ==, G_DBUS_PROPERTY_INFO_FLAGS_READABLE);
    }

  for (count = 0; interface->methods[count] != NULL; count++)
    ;
  g_assert_cmpuint (count, ==, G_N_ELEMENTS (methods));
  for (guint index = 0; index < G_N_ELEMENTS (methods); index++)
    {
      GDBusMethodInfo *method
          = g_dbus_interface_info_lookup_method (interface, methods[index].name);
      guint in_count = 0;
      guint out_count = 0;

      g_assert_nonnull (method);
      if (method->in_args != NULL)
        while (method->in_args[in_count] != NULL)
          in_count++;
      if (method->out_args != NULL)
        while (method->out_args[out_count] != NULL)
          out_count++;
      g_assert_cmpuint (in_count, ==, methods[index].in_signature != NULL ? 1 : 0);
      g_assert_cmpuint (out_count, ==, methods[index].out_signature != NULL ? 1 : 0);
      if (in_count == 1)
        {
          g_assert_cmpstr (method->in_args[0]->name, ==, methods[index].in_name);
          g_assert_cmpstr (method->in_args[0]->signature, ==, methods[index].in_signature);
        }
      if (out_count == 1)
        {
          g_assert_cmpstr (method->out_args[0]->name, ==, methods[index].out_name);
          g_assert_cmpstr (method->out_args[0]->signature, ==, methods[index].out_signature);
        }
    }

  for (count = 0; interface->signals[count] != NULL; count++)
    ;
  g_assert_cmpuint (count, ==, 1);
  GDBusSignalInfo *signal = g_dbus_interface_info_lookup_signal (interface, "ApplicationsChanged");
  g_assert_nonnull (signal);
  g_assert_true (signal->args == NULL || signal->args[0] == NULL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/contracts/state-strings", state_strings);
  g_test_add_func ("/contracts/dbus", dbus_contract);
  return g_test_run ();
}
