#include "cd-state-store.h"

#include <glib/gstdio.h>

static JsonNode *
make_document (gint64 value)
{
  JsonObject *object = json_object_new ();
  JsonNode *root = json_node_new (JSON_NODE_OBJECT);

  json_object_set_int_member (object, "value", value);
  json_node_take_object (root, object);
  return root;
}

static void
durable_round_trip_is_private (void)
{
  g_autofree char *directory = g_dir_make_tmp ("call-ducker-store-XXXXXX", NULL);
  g_autofree char *state_directory = g_build_filename (directory, "nested", NULL);
  g_autofree char *path = g_build_filename (state_directory, "state.json", NULL);
  g_autoptr (JsonNode) first = make_document (17);
  g_autoptr (JsonNode) second = make_document (29);
  g_autoptr (JsonNode) loaded = NULL;
  g_autoptr (GError) error = NULL;
  CdStateStore *store = cd_state_store_new_for_path (path);
  GStatBuf stat_buffer;

  g_assert_true (cd_state_store_save (store, first, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_stat (path, &stat_buffer), ==, 0);
  g_assert_cmpuint (stat_buffer.st_mode & 0777, ==, 0600);

  g_assert_true (cd_state_store_save (store, second, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_stat (path, &stat_buffer), ==, 0);
  g_assert_cmpuint (stat_buffer.st_mode & 0777, ==, 0600);

  loaded = cd_state_store_load (store, &error);
  g_assert_no_error (error);
  g_assert_nonnull (loaded);
  g_assert_true (JSON_NODE_HOLDS_OBJECT (loaded));
  g_assert_cmpint (json_object_get_int_member (json_node_get_object (loaded), "value"), ==, 29);

  cd_state_store_free (store);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_rmdir (state_directory), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
failed_save_does_not_replace_existing_file (void)
{
  g_autofree char *directory = g_dir_make_tmp ("call-ducker-store-XXXXXX", NULL);
  g_autofree char *blocker = g_build_filename (directory, "not-a-directory", NULL);
  g_autofree char *path = g_build_filename (blocker, "state.json", NULL);
  g_autoptr (JsonNode) root = make_document (1);
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  CdStateStore *store = cd_state_store_new_for_path (path);

  g_assert_true (g_file_set_contents (blocker, "unchanged", -1, &error));
  g_assert_no_error (error);
  g_assert_false (cd_state_store_save (store, root, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_true (g_file_get_contents (blocker, &contents, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (contents, ==, "unchanged");

  cd_state_store_free (store);
  g_assert_cmpint (g_unlink (blocker), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/state-store/durable-private-round-trip", durable_round_trip_is_private);
  g_test_add_func ("/state-store/failed-save-preserves-existing",
                   failed_save_does_not_replace_existing_file);
  return g_test_run ();
}
