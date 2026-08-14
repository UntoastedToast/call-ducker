#include "cd-volume-controller.h"
#include "fake-audio-backend.h"

#include <glib/gstdio.h>

typedef struct
{
  CdVolumeController *controller;
  gboolean restore;
  gboolean done;
  gboolean succeeded;
  GError *error;
} AsyncResult;

static void
operation_done (GObject *object, GAsyncResult *result, gpointer data)
{
  AsyncResult *wait = data;

  (void)object;
  wait->succeeded
      = wait->restore
            ? cd_volume_controller_restore_all_finish (wait->controller, result, &wait->error)
            : cd_volume_controller_reconcile_finish (wait->controller, result, &wait->error);
  wait->done = TRUE;
}

static void
start_reconcile (AsyncResult *result, CdVolumeController *controller, GPtrArray *targets,
                 double target)
{
  *result = (AsyncResult){ .controller = controller };
  cd_volume_controller_reconcile_async (controller, targets, target, NULL, operation_done, result);
}

static void
start_restore (AsyncResult *result, CdVolumeController *controller)
{
  *result = (AsyncResult){ .controller = controller, .restore = TRUE };
  cd_volume_controller_restore_all_async (controller, NULL, operation_done, result);
}

static void
drain (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
wait_for_result (AsyncResult *result)
{
  gint64 deadline = g_get_monotonic_time () + (2 * G_TIME_SPAN_SECOND);

  while (!result->done && g_get_monotonic_time () < deadline)
    {
      drain ();
      g_usleep (1000);
    }
  g_assert_true (result->done);
}

static void
assert_succeeded (AsyncResult *result)
{
  wait_for_result (result);
  g_assert_no_error (result->error);
  g_assert_true (result->succeeded);
}

static void
async_result_clear (AsyncResult *result)
{
  g_clear_error (&result->error);
}

static char *
temporary_journal (char **directory)
{
  *directory = g_dir_make_tmp ("call-ducker-test-XXXXXX", NULL);
  g_assert_nonnull (*directory);
  return g_build_filename (*directory, "pulse-restore.json", NULL);
}

static void
remove_journal (const char *path, const char *directory)
{
  g_unlink (path);
  g_rmdir (directory);
}

static CdVolumeController *
new_controller (TestAudioBackend *backend, const char *path)
{
  return cd_volume_controller_new (CD_AUDIO_BACKEND (backend), cd_state_store_new_for_path (path),
                                   NULL, NULL);
}

static CdAudioStream *
add_stream_with_layout (TestAudioBackend *backend, guint32 index, const char *selector,
                        const char *restore_id, const pa_channel_map *map, const pa_cvolume *volume)
{
  CdAudioStream *stream = test_audio_backend_add_stream (
      backend, index, selector, "Game", FALSE, TRUE, "game.desktop", "game", "game", TRUE, 1.0);

  g_free (stream->restore_id);
  stream->restore_id = g_strdup (restore_id);
  test_audio_backend_set_stream_layout (backend, index, map, volume);
  return stream;
}

static CdAudioStream *
add_stereo_stream (TestAudioBackend *backend, guint32 index, const char *selector,
                   const char *restore_id, pa_volume_t left, pa_volume_t right)
{
  pa_channel_map map;
  pa_cvolume volume = { .channels = 2, .values = { left, right } };

  pa_channel_map_init_stereo (&map);
  return add_stream_with_layout (backend, index, selector, restore_id, &map, &volume);
}

static const pa_cvolume *
stored_volume (TestAudioBackend *backend, guint32 index)
{
  const pa_cvolume *volume = test_audio_backend_get_volume (backend, index);

  g_assert_nonnull (volume);
  return volume;
}

static void
absolute_target_and_restore (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *loud = add_stereo_stream (backend, 41, "application:game", "restore:game-1",
                                           (pa_volume_t)(0.8 * PA_VOLUME_NORM),
                                           (pa_volume_t)(0.4 * PA_VOLUME_NORM));
  CdAudioStream *quiet = add_stereo_stream (backend, 42, "application:game", "restore:game-2",
                                            (pa_volume_t)(0.4 * PA_VOLUME_NORM),
                                            (pa_volume_t)(0.2 * PA_VOLUME_NORM));
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  g_autofree char *journal = NULL;
  g_autoptr (GError) error = NULL;
  AsyncResult result;

  g_ptr_array_add (targets, loud);
  g_ptr_array_add (targets, quiet);
  start_reconcile (&result, controller, targets, 0.70);
  assert_succeeded (&result);
  async_result_clear (&result);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 41)), ==,
                    (pa_volume_t)(0.70 * PA_VOLUME_NORM + 0.5));
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 42)), ==,
                    (pa_volume_t)(0.70 * PA_VOLUME_NORM + 0.5));
  g_assert_cmpint (ABS ((gint)stored_volume (backend, 41)->values[1] * 2
                        - (gint)stored_volume (backend, 41)->values[0]),
                   <=, 1);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 2);
  g_assert_true (g_file_get_contents (path, &journal, NULL, &error));
  g_assert_nonnull (strstr (journal, "channelMap"));
  g_assert_nonnull (strstr (journal, "front-left"));

  start_reconcile (&result, controller, targets, 0.35);
  assert_succeeded (&result);
  async_result_clear (&result);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 41)), ==,
                    (pa_volume_t)(0.35 * PA_VOLUME_NORM + 0.5));
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 42)), ==,
                    (pa_volume_t)(0.35 * PA_VOLUME_NORM + 0.5));

  start_restore (&result, controller);
  assert_succeeded (&result);
  async_result_clear (&result);
  g_assert_cmpuint (stored_volume (backend, 41)->values[0], ==,
                    (pa_volume_t)(0.8 * PA_VOLUME_NORM));
  g_assert_cmpuint (stored_volume (backend, 41)->values[1], ==,
                    (pa_volume_t)(0.4 * PA_VOLUME_NORM));
  g_assert_cmpuint (stored_volume (backend, 42)->values[0], ==,
                    (pa_volume_t)(0.4 * PA_VOLUME_NORM));
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);

  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
boundaries_and_silent_channels (void)
{
  pa_cvolume original = { .channels = 2, .values = { 0, 0 } };
  pa_cvolume result = cd_volume_absolute (&original, 1.0);

  g_assert_cmpuint (result.values[0], ==, PA_VOLUME_NORM);
  g_assert_cmpuint (result.values[1], ==, PA_VOLUME_NORM);
  result = cd_volume_absolute (&original, 0.0);
  g_assert_cmpuint (result.values[0], ==, PA_VOLUME_MUTED);
  g_assert_cmpuint (result.values[1], ==, PA_VOLUME_MUTED);
  original.values[0] = PA_VOLUME_NORM / 2;
  original.values[1] = PA_VOLUME_NORM;
  result = cd_volume_absolute (&original, 1.0);
  g_assert_cmpuint (result.values[0], ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (result.values[1], ==, PA_VOLUME_NORM);
}

static void
external_drift_is_corrected (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 4, "binary:game", "restore:game",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM / 2);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;
  guint calls;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.4);
  assert_succeeded (&result);
  async_result_clear (&result);
  calls = test_audio_backend_get_set_count (backend);
  test_audio_backend_set_stream_volume (backend, 4, 0.9);
  test_audio_backend_changed (backend);

  start_reconcile (&result, controller, targets, 0.4);
  assert_succeeded (&result);
  async_result_clear (&result);
  g_assert_cmpuint (test_audio_backend_get_set_count (backend), ==, calls + 1);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 4), 0.4, 0.0001);

  start_restore (&result, controller);
  assert_succeeded (&result);
  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
newer_generation_supersedes_waiting_work (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 5, "binary:game", "restore:game",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult old_result;
  AsyncResult new_result;

  g_ptr_array_add (targets, stream);
  test_audio_backend_set_delayed (backend, TRUE);
  start_reconcile (&old_result, controller, targets, 0.7);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  start_reconcile (&new_result, controller, targets, 0.3);
  drain ();
  g_assert_true (old_result.done);
  g_assert_false (old_result.succeeded);
  g_assert_error (old_result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  drain ();
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  assert_succeeded (&new_result);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 5), 0.3, 0.0001);

  async_result_clear (&old_result);
  async_result_clear (&new_result);
  test_audio_backend_set_delayed (backend, FALSE);
  AsyncResult restore_result;
  start_restore (&restore_result, controller);
  assert_succeeded (&restore_result);
  async_result_clear (&restore_result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
restore_supersedes_active_target (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 6, "binary:game", "restore:game",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM / 2);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult target_result;
  AsyncResult restore_result;

  g_ptr_array_add (targets, stream);
  test_audio_backend_set_delayed (backend, TRUE);
  start_reconcile (&target_result, controller, targets, 0.2);
  start_restore (&restore_result, controller);
  drain ();
  g_assert_error (target_result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  drain ();
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  assert_succeeded (&restore_result);
  g_assert_cmpuint (stored_volume (backend, 6)->values[0], ==, PA_VOLUME_NORM);
  g_assert_cmpuint (stored_volume (backend, 6)->values[1], ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);

  async_result_clear (&target_result);
  async_result_clear (&restore_result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
index_reuse_is_rejected_at_completion (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 7, "binary:game", "restore:old",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, stream);
  test_audio_backend_set_delayed (backend, TRUE);
  start_reconcile (&result, controller, targets, 0.2);
  test_audio_backend_clear (backend);
  add_stereo_stream (backend, 7, "binary:other", "restore:new", PA_VOLUME_NORM / 2,
                     PA_VOLUME_NORM / 2);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  wait_for_result (&result);
  g_assert_false (result.succeeded);
  g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 7)), ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 1);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
channel_layout_change_is_rejected_at_completion (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 8, "binary:game", "restore:layout-race",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM / 4);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;
  pa_channel_map reversed = {
    .channels = 2,
    .map = { PA_CHANNEL_POSITION_FRONT_RIGHT, PA_CHANNEL_POSITION_FRONT_LEFT },
  };
  pa_cvolume replacement = {
    .channels = 2,
    .values = { PA_VOLUME_NORM / 2, PA_VOLUME_NORM / 2 },
  };

  g_ptr_array_add (targets, stream);
  test_audio_backend_set_delayed (backend, TRUE);
  start_reconcile (&result, controller, targets, 0.2);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);

  test_audio_backend_set_stream_layout (backend, 8, &reversed, &replacement);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  drain ();
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_cmpuint (stored_volume (backend, 8)->values[0], ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (stored_volume (backend, 8)->values[1], ==, PA_VOLUME_NORM / 2);

  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  wait_for_result (&result);
  g_assert_false (result.succeeded);
  g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpuint (stored_volume (backend, 8)->values[0], ==, PA_VOLUME_NORM / 4);
  g_assert_cmpuint (stored_volume (backend, 8)->values[1], ==, PA_VOLUME_NORM);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
partial_failure_rolls_back_batch (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *first = add_stereo_stream (backend, 10, "application:a", "restore:a",
                                            PA_VOLUME_NORM, PA_VOLUME_NORM / 2);
  CdAudioStream *second = add_stereo_stream (backend, 11, "application:b", "restore:b",
                                             PA_VOLUME_NORM / 2, PA_VOLUME_NORM / 4);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, first);
  g_ptr_array_add (targets, second);
  test_audio_backend_set_delayed (backend, TRUE);
  start_reconcile (&result, controller, targets, 0.25);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  drain ();
  g_assert_true (test_audio_backend_complete_next (backend, FALSE));
  drain ();
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  drain ();
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  wait_for_result (&result);
  g_assert_false (result.succeeded);
  g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (stored_volume (backend, 10)->values[0], ==, PA_VOLUME_NORM);
  g_assert_cmpuint (stored_volume (backend, 10)->values[1], ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (stored_volume (backend, 11)->values[0], ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (stored_volume (backend, 11)->values[1], ==, PA_VOLUME_NORM / 4);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
restore_journal_updates_as_one_batch (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *first = add_stereo_stream (backend, 20, "application:a", "restore:a",
                                            PA_VOLUME_NORM, PA_VOLUME_NORM);
  CdAudioStream *second = add_stereo_stream (backend, 21, "application:b", "restore:b",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, first);
  g_ptr_array_add (targets, second);
  start_reconcile (&result, controller, targets, 0.3);
  assert_succeeded (&result);
  async_result_clear (&result);

  test_audio_backend_set_delayed (backend, TRUE);
  start_restore (&result, controller);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  drain ();
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 2);
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  assert_succeeded (&result);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
failed_restore_stays_pending (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 22, "application:a", "restore:a",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM / 2);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  g_autofree char *journal = NULL;
  g_autoptr (GError) error = NULL;
  AsyncResult result;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  test_audio_backend_fail_next_set (backend, "restore failed");
  start_restore (&result, controller);
  wait_for_result (&result);
  g_assert_false (result.succeeded);
  g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 1);
  g_assert_true (g_file_get_contents (path, &journal, NULL, &error));
  g_assert_nonnull (strstr (journal, "application:a"));

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
stale_restore_snapshot_reuses_original (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 23, "application:a", "restore:a",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM / 2);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  start_restore (&result, controller);
  assert_succeeded (&result);
  async_result_clear (&result);

  /* Model a restore ACK arriving before the subscription snapshot. */
  pa_cvolume_set (&stream->volume, 2, (pa_volume_t)(0.2 * PA_VOLUME_NORM + 0.5));
  start_reconcile (&result, controller, targets, 0.4);
  assert_succeeded (&result);
  async_result_clear (&result);
  start_restore (&result, controller);
  assert_succeeded (&result);
  g_assert_cmpuint (stored_volume (backend, 23)->values[0], ==, PA_VOLUME_NORM);
  g_assert_cmpuint (stored_volume (backend, 23)->values[1], ==, PA_VOLUME_NORM / 2);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
post_restore_mixer_value_becomes_new_original (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 24, "application:a", "restore:a",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  start_restore (&result, controller);
  assert_succeeded (&result);
  async_result_clear (&result);

  test_audio_backend_set_stream_volume (backend, 24, 0.6);
  test_audio_backend_changed (backend);
  start_reconcile (&result, controller, targets, 0.3);
  assert_succeeded (&result);
  async_result_clear (&result);
  start_restore (&result, controller);
  assert_succeeded (&result);
  g_assert_cmpfloat_with_epsilon (test_audio_backend_get_main_volume (backend, 24), 0.6, 0.0001);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
unique_restore_id_remaps_and_persists_index (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 30, "application:game", "restore:unique",
                                             PA_VOLUME_NORM / 2, PA_VOLUME_NORM / 4);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  g_autofree char *journal = NULL;
  g_autoptr (GError) error = NULL;
  AsyncResult result;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  test_audio_backend_clear (backend);
  add_stereo_stream (backend, 31, "application:game", "restore:unique", PA_VOLUME_NORM / 8,
                     PA_VOLUME_NORM / 8);
  test_audio_backend_set_delayed (backend, TRUE);
  start_restore (&result, controller);
  g_assert_cmpuint (test_audio_backend_get_pending_sets (backend), ==, 1);
  g_assert_true (g_file_get_contents (path, &journal, NULL, &error));
  g_assert_nonnull (strstr (journal, "\"pulseIndex\":31"));
  g_assert_true (test_audio_backend_complete_next (backend, TRUE));
  assert_succeeded (&result);
  g_assert_cmpuint (stored_volume (backend, 31)->values[0], ==, PA_VOLUME_NORM / 2);
  g_assert_cmpuint (stored_volume (backend, 31)->values[1], ==, PA_VOLUME_NORM / 4);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
ambiguous_restore_ids_are_never_remapped (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *first = add_stereo_stream (backend, 40, "application:game", "restore:shared",
                                            PA_VOLUME_NORM, PA_VOLUME_NORM);
  CdAudioStream *second = add_stereo_stream (backend, 41, "application:game", "restore:shared",
                                             PA_VOLUME_NORM / 2, PA_VOLUME_NORM / 2);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, first);
  g_ptr_array_add (targets, second);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 2);

  test_audio_backend_clear (backend);
  add_stereo_stream (backend, 42, "application:game", "restore:shared", PA_VOLUME_NORM / 8,
                     PA_VOLUME_NORM / 8);
  start_restore (&result, controller);
  assert_succeeded (&result);
  async_result_clear (&result);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 42)), ==, PA_VOLUME_NORM / 8);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 2);

  test_audio_backend_clear (backend);
  add_stereo_stream (backend, 43, "application:game", "restore:shared", PA_VOLUME_NORM / 8,
                     PA_VOLUME_NORM / 8);
  add_stereo_stream (backend, 44, "application:game", "restore:shared", PA_VOLUME_NORM / 4,
                     PA_VOLUME_NORM / 4);
  start_restore (&result, controller);
  assert_succeeded (&result);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 43)), ==, PA_VOLUME_NORM / 8);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 44)), ==, PA_VOLUME_NORM / 4);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 2);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
unwritable_identity_makes_remap_ambiguous (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 45, "application:game", "restore:shared",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  test_audio_backend_clear (backend);
  add_stereo_stream (backend, 46, "application:game", "restore:shared", PA_VOLUME_NORM / 4,
                     PA_VOLUME_NORM / 4);
  stream = add_stereo_stream (backend, 47, "application:game", "restore:shared", PA_VOLUME_NORM / 8,
                              PA_VOLUME_NORM / 8);
  stream->volume_controllable = FALSE;

  start_restore (&result, controller);
  assert_succeeded (&result);
  g_assert_cmpuint (pa_cvolume_max (stored_volume (backend, 46)), ==, PA_VOLUME_NORM / 4);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 1);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
channel_map_is_remapped_before_restore (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 50, "application:game", "restore:layout",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM / 4);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;
  pa_channel_map reversed = {
    .channels = 2,
    .map = { PA_CHANNEL_POSITION_FRONT_RIGHT, PA_CHANNEL_POSITION_FRONT_LEFT },
  };
  pa_cvolume current = {
    .channels = 2,
    .values = { PA_VOLUME_NORM / 8, PA_VOLUME_NORM / 8 },
  };

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  async_result_clear (&result);
  test_audio_backend_clear (backend);
  add_stream_with_layout (backend, 51, "application:game", "restore:layout", &reversed, &current);

  start_restore (&result, controller);
  assert_succeeded (&result);
  g_assert_cmpuint (stored_volume (backend, 51)->values[0], ==, PA_VOLUME_NORM / 4);
  g_assert_cmpuint (stored_volume (backend, 51)->values[1], ==, PA_VOLUME_NORM);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

static void
restart_loads_raw_channels_and_restores (void)
{
  g_autoptr (TestAudioBackend) first_backend = test_audio_backend_new ();
  g_autoptr (TestAudioBackend) second_backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *first_controller = new_controller (first_backend, path);
  CdAudioStream *stream = add_stereo_stream (first_backend, 60, "binary:game", "restore:restart",
                                             PA_VOLUME_NORM * 3 / 4, PA_VOLUME_NORM / 3);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;
  g_autoptr (GError) error = NULL;

  g_ptr_array_add (targets, stream);
  start_reconcile (&result, first_controller, targets, 0.1);
  assert_succeeded (&result);
  async_result_clear (&result);
  cd_volume_controller_free (first_controller);

  add_stereo_stream (second_backend, 60, "binary:game", "restore:restart", PA_VOLUME_NORM / 10,
                     PA_VOLUME_NORM / 10);
  CdVolumeController *second_controller = new_controller (second_backend, path);
  g_assert_true (cd_volume_controller_load (second_controller, &error));
  start_restore (&result, second_controller);
  assert_succeeded (&result);
  g_assert_cmpuint (stored_volume (second_backend, 60)->values[0], ==, PA_VOLUME_NORM * 3 / 4);
  g_assert_cmpuint (stored_volume (second_backend, 60)->values[1], ==, PA_VOLUME_NORM / 3);

  async_result_clear (&result);
  cd_volume_controller_free (second_controller);
  remove_journal (path, directory);
}

static void
invalid_journal_stays_fail_closed (void)
{
  static const char *invalid_documents[] = {
    "{\"version\":2,\"streams\":[]}",
    "{\"version\":1,\"streams\":[{\"selector\":7,\"pulseIndex\":1,"
    "\"restoreId\":\"r\",\"name\":\"n\",\"channelVolumes\":[1],"
    "\"channelMap\":[\"mono\"]}]}",
    "{\"version\":1,\"streams\":[{\"selector\":\"s\",\"pulseIndex\":1,"
    "\"restoreId\":\"r\",\"name\":\"n\",\"channelVolumes\":[1],"
    "\"channelMap\":[\"mono\"]},{\"selector\":\"s\",\"pulseIndex\":1,"
    "\"restoreId\":\"r\",\"name\":\"n\",\"channelVolumes\":[2],"
    "\"channelMap\":[\"mono\"]}]}",
    "{\"version\":1,\"streams\":[{\"selector\":\"s\",\"pulseIndex\":1,"
    "\"restoreId\":\"r\",\"name\":\"n\",\"channelVolumes\":[1,2],"
    "\"channelMap\":[\"front-left\",\"front-left\"]}]}",
  };

  for (guint test = 0; test < G_N_ELEMENTS (invalid_documents); test++)
    {
      g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
      g_autofree char *directory = NULL;
      g_autofree char *path = temporary_journal (&directory);
      g_autofree char *after = NULL;
      g_autoptr (GError) error = NULL;
      CdVolumeController *controller;
      CdAudioStream *stream;
      g_autoptr (GPtrArray) targets = g_ptr_array_new ();
      AsyncResult result;

      g_assert_true (g_file_set_contents (path, invalid_documents[test], -1, &error));
      controller = new_controller (backend, path);
      g_assert_false (cd_volume_controller_load (controller, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_clear_error (&error);
      stream = add_stereo_stream (backend, 70, "binary:game", "restore:blocked", PA_VOLUME_NORM,
                                  PA_VOLUME_NORM);
      g_ptr_array_add (targets, stream);
      start_reconcile (&result, controller, targets, 0.2);
      wait_for_result (&result);
      g_assert_false (result.succeeded);
      g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_assert_cmpuint (test_audio_backend_get_set_count (backend), ==, 0);
      g_assert_true (g_file_get_contents (path, &after, NULL, &error));
      g_assert_cmpstr (after, ==, invalid_documents[test]);

      async_result_clear (&result);
      cd_volume_controller_free (controller);
      remove_journal (path, directory);
    }
}

static void
uncontrollable_stream_is_not_journaled (void)
{
  g_autoptr (TestAudioBackend) backend = test_audio_backend_new ();
  g_autofree char *directory = NULL;
  g_autofree char *path = temporary_journal (&directory);
  CdVolumeController *controller = new_controller (backend, path);
  CdAudioStream *stream = add_stereo_stream (backend, 80, "binary:game", "restore:no",
                                             PA_VOLUME_NORM, PA_VOLUME_NORM);
  g_autoptr (GPtrArray) targets = g_ptr_array_new ();
  AsyncResult result;

  stream->volume_controllable = FALSE;
  g_ptr_array_add (targets, stream);
  start_reconcile (&result, controller, targets, 0.2);
  assert_succeeded (&result);
  g_assert_cmpuint (cd_volume_controller_get_pending (controller), ==, 0);
  g_assert_cmpuint (test_audio_backend_get_set_count (backend), ==, 0);

  async_result_clear (&result);
  cd_volume_controller_free (controller);
  remove_journal (path, directory);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/volume/absolute-and-restore", absolute_target_and_restore);
  g_test_add_func ("/volume/boundaries-and-silent", boundaries_and_silent_channels);
  g_test_add_func ("/volume/external-drift", external_drift_is_corrected);
  g_test_add_func ("/volume/generation-supersedes", newer_generation_supersedes_waiting_work);
  g_test_add_func ("/volume/target-to-restore", restore_supersedes_active_target);
  g_test_add_func ("/volume/index-reuse", index_reuse_is_rejected_at_completion);
  g_test_add_func ("/volume/channel-layout-race", channel_layout_change_is_rejected_at_completion);
  g_test_add_func ("/volume/partial-failure-rollback", partial_failure_rolls_back_batch);
  g_test_add_func ("/volume/restore-batch-journal", restore_journal_updates_as_one_batch);
  g_test_add_func ("/volume/failed-restore", failed_restore_stays_pending);
  g_test_add_func ("/volume/stale-restore-snapshot", stale_restore_snapshot_reuses_original);
  g_test_add_func ("/volume/post-restore-mixer", post_restore_mixer_value_becomes_new_original);
  g_test_add_func ("/volume/unique-remap", unique_restore_id_remaps_and_persists_index);
  g_test_add_func ("/volume/ambiguous-remap", ambiguous_restore_ids_are_never_remapped);
  g_test_add_func ("/volume/unwritable-remap-ambiguity", unwritable_identity_makes_remap_ambiguous);
  g_test_add_func ("/volume/channel-map-remap", channel_map_is_remapped_before_restore);
  g_test_add_func ("/volume/restart-raw-channels", restart_loads_raw_channels_and_restores);
  g_test_add_func ("/volume/journal-fail-closed", invalid_journal_stays_fail_closed);
  g_test_add_func ("/volume/uncontrollable", uncontrollable_stream_is_not_journaled);
  return g_test_run ();
}
