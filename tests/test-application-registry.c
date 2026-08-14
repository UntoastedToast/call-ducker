#include "cd-application-registry.h"
#include "fake-audio-backend.h"

#include <glib/gstdio.h>

typedef struct
{
  char *directory;
  char *path;
  GSettings *settings;
  TestAudioBackend *backend;
  CdApplicationRegistry *registry;
} Fixture;

static void
setup (Fixture *fixture, gconstpointer data)
{
  (void)data;
  fixture->directory = g_dir_make_tmp ("call-ducker-registry-XXXXXX", NULL);
  fixture->path = g_build_filename (fixture->directory, "applications.json", NULL);
  fixture->settings = g_settings_new ("io.github.UntoastedToast.CallDucker");
  fixture->backend = test_audio_backend_new ();
  fixture->registry
      = cd_application_registry_new (fixture->settings, CD_AUDIO_BACKEND (fixture->backend),
                                     cd_state_store_new_for_path (fixture->path));
}

static void
teardown (Fixture *fixture, gconstpointer data)
{
  (void)data;
  cd_application_registry_free (fixture->registry);
  g_object_unref (fixture->backend);
  g_object_unref (fixture->settings);
  g_unlink (fixture->path);
  g_rmdir (fixture->directory);
  g_free (fixture->path);
  g_free (fixture->directory);
}

static GVariant *
application_at (GVariant *applications, gsize index)
{
  return g_variant_get_child_value (applications, index);
}

static void
invalid_file_is_replaced_only_by_snapshot (Fixture *fixture, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gboolean changed = FALSE;

  (void)data;
  g_assert_true (g_file_set_contents (fixture->path, "{broken", -1, &error));
  g_assert_false (cd_application_registry_load (fixture->registry, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_assert_true (g_file_get_contents (fixture->path, &contents, NULL, &error));
  g_assert_cmpstr (contents, ==, "{broken");
  test_audio_backend_add_stream (fixture->backend, 1, "application:game", "Game", FALSE, TRUE,
                                 "game", "game", NULL, TRUE, 0.8);
  g_assert_true (cd_application_registry_refresh (fixture->registry, &changed, &error));
  g_assert_true (changed);
  g_clear_pointer (&contents, g_free);
  g_assert_true (g_file_get_contents (fixture->path, &contents, NULL, &error));
  g_assert_true (g_str_has_prefix (contents, "{"));
  g_assert_nonnull (strstr (contents, "application:game"));
}

static void
filters_prunes_and_keeps_configured (Fixture *fixture, gconstpointer data)
{
  const char *always[] = { "application:offline", NULL };
  const char *calls[] = { "application:call", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) applications = NULL;
  gboolean changed = FALSE;

  (void)data;
  g_settings_set_strv (fixture->settings, "always-duck-selectors", always);
  g_settings_set_strv (fixture->settings, "call-app-selectors", calls);
  g_assert_true (g_file_set_contents (fixture->path,
                                      "{\"o|application:offline\":{\"name\":\"Offline\",\"icon\":"
                                      "\"x\",\"lastSeen\":1,\"input\":false,\"game\":false},"
                                      "\"o|application:stale\":{\"name\":\"Stale\",\"icon\":\"x\","
                                      "\"lastSeen\":1,\"input\":false,\"game\":false}}",
                                      -1, &error));
  g_assert_true (cd_application_registry_load (fixture->registry, &error));
  test_audio_backend_add_stream (fixture->backend, 1, "application:call", "Call", TRUE, TRUE,
                                 "call", "call", NULL, FALSE, 0.5);
  test_audio_backend_add_stream (fixture->backend, 2, "application:game-input", "Game", TRUE, TRUE,
                                 "game-input", "game", NULL, TRUE, 0.5);
  test_audio_backend_add_stream (fixture->backend, 3, "application:call-ducker", "CallDucker",
                                 FALSE, TRUE, "io.github.UntoastedToast.CallDucker", "call-ducker",
                                 NULL, FALSE, 0.5);
  g_assert_true (cd_application_registry_refresh (fixture->registry, &changed, &error));
  applications = cd_application_registry_dup_applications (fixture->registry);
  g_assert_cmpuint (g_variant_n_children (applications), ==, 2);
  g_autoptr (GVariant) first = application_at (applications, 0);
  g_autoptr (GVariant) second = application_at (applications, 1);
  const char *first_id = NULL;
  const char *second_id = NULL;
  gboolean available = TRUE;
  g_assert_true (g_variant_lookup (first, "id", "&s", &first_id));
  g_assert_true (g_variant_lookup (second, "id", "&s", &second_id));
  g_assert_cmpstr (first_id, ==, "application:call");
  g_assert_cmpstr (second_id, ==, "application:offline");
  g_assert_true (g_variant_lookup (second, "available", "b", &available));
  g_assert_false (available);
}

static void
deduplicates_and_orders_deterministically (Fixture *fixture, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) applications = NULL;
  gboolean changed = FALSE;

  (void)data;
  test_audio_backend_add_stream (fixture->backend, 1, "application:z", "Z", FALSE, TRUE, "z", "z",
                                 NULL, FALSE, 0.5);
  test_audio_backend_add_stream (fixture->backend, 2, "application:a", "A", FALSE, TRUE, "a", "a",
                                 NULL, FALSE, 0.5);
  test_audio_backend_add_stream (fixture->backend, 3, "application:a", "A newer", FALSE, TRUE, "a",
                                 "a", NULL, FALSE, 0.5);
  test_audio_backend_add_stream (fixture->backend, 4, "application:a", "A input", TRUE, TRUE, "a",
                                 "a", NULL, FALSE, 0.5);
  g_assert_true (cd_application_registry_refresh (fixture->registry, &changed, &error));
  applications = cd_application_registry_dup_applications (fixture->registry);
  g_assert_cmpuint (g_variant_n_children (applications), ==, 3);
  g_autoptr (GVariant) first = application_at (applications, 0);
  g_autoptr (GVariant) second = application_at (applications, 1);
  gboolean input = FALSE;
  const char *name = NULL;
  g_assert_true (g_variant_lookup (first, "input", "b", &input));
  g_assert_true (input);
  g_assert_true (g_variant_lookup (second, "name", "&s", &name));
  g_assert_cmpstr (name, ==, "A newer");
  changed = TRUE;
  g_assert_true (cd_application_registry_refresh (fixture->registry, &changed, &error));
  g_assert_false (changed);
}

static void
uncontrollable_playback_is_not_available (Fixture *fixture, gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) applications = NULL;
  CdAudioStream *stream;
  gboolean changed = FALSE;

  (void)data;
  stream = test_audio_backend_add_stream (fixture->backend, 9, "application:locked", "Locked",
                                          FALSE, TRUE, "locked", "locked", NULL, FALSE, 0.5);
  stream->volume_controllable = FALSE;
  g_assert_true (cd_application_registry_refresh (fixture->registry, &changed, &error));
  applications = cd_application_registry_dup_applications (fixture->registry);
  g_assert_cmpuint (g_variant_n_children (applications), ==, 0);
}

int
main (int argc, char **argv)
{
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/registry/invalid-recovery", Fixture, NULL, setup,
              invalid_file_is_replaced_only_by_snapshot, teardown);
  g_test_add ("/registry/filter-prune-configured", Fixture, NULL, setup,
              filters_prunes_and_keeps_configured, teardown);
  g_test_add ("/registry/duplicates-order", Fixture, NULL, setup,
              deduplicates_and_orders_deterministically, teardown);
  g_test_add ("/registry/uncontrollable-playback", Fixture, NULL, setup,
              uncontrollable_playback_is_not_available, teardown);
  return g_test_run ();
}
