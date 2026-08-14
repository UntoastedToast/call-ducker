#include "pulse-backend.h"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#define TEST_APP_ID "io.github.UntoastedToast.CallDucker.BackendTest"
#define TEST_RECORD_APP_ID "io.github.UntoastedToast.CallDucker.RecordTest"
#define TEST_PORTAL_ID "org.example.CallDuckerPortalTest"
#define TEST_RESTORE_ID "sink-input-by-application-id:call-ducker-test"

typedef struct _Fixture Fixture;
typedef gboolean (*WaitCondition) (Fixture *fixture);

struct _Fixture
{
  GMainLoop *loop;
  pa_glib_mainloop *pulse_loop;
  pa_context *context;
  pa_stream *playback;
  pa_stream *recording;
  pa_stream *binary_identity;
  pa_stream *name_identity;
  CdPulseBackend *backend;
  WaitCondition condition;
  guint backend_changes;
  guint backend_failures;
  guint minimum_changes;
  guint minimum_failures;
  pa_volume_t expected_volume;
  gboolean operation_done;
  gboolean operation_succeeded;
  gboolean info_done;
  gboolean mute;
  gboolean timed_out;
  GError *error;
};

static gboolean
timeout_cb (gpointer data)
{
  Fixture *fixture = data;

  fixture->timed_out = TRUE;
  g_main_loop_quit (fixture->loop);
  return G_SOURCE_REMOVE;
}

static void
notify (Fixture *fixture)
{
  if (fixture->condition != NULL && fixture->condition (fixture))
    g_main_loop_quit (fixture->loop);
}

static void
wait_until (Fixture *fixture, WaitCondition condition)
{
  guint timeout;

  g_assert_null (fixture->condition);
  fixture->condition = condition;
  fixture->timed_out = FALSE;
  if (!condition (fixture))
    {
      timeout = g_timeout_add_seconds (8, timeout_cb, fixture);
      g_main_loop_run (fixture->loop);
      if (!fixture->timed_out)
        g_source_remove (timeout);
    }
  fixture->condition = NULL;
  g_assert_false (fixture->timed_out);
  g_assert_true (condition (fixture));
}

static gboolean
context_ready (Fixture *fixture)
{
  return fixture->context != NULL && pa_context_get_state (fixture->context) == PA_CONTEXT_READY;
}

static gboolean
streams_ready (Fixture *fixture)
{
  return fixture->playback != NULL && fixture->recording != NULL
         && pa_stream_get_state (fixture->playback) == PA_STREAM_READY
         && pa_stream_get_state (fixture->recording) == PA_STREAM_READY;
}

static gboolean
playback_control_ready (Fixture *fixture)
{
  return fixture->playback != NULL && pa_stream_get_state (fixture->playback) == PA_STREAM_READY;
}

static gboolean
identity_streams_ready (Fixture *fixture)
{
  return fixture->binary_identity != NULL && fixture->name_identity != NULL
         && pa_stream_get_state (fixture->binary_identity) == PA_STREAM_READY
         && pa_stream_get_state (fixture->name_identity) == PA_STREAM_READY;
}

static gboolean
operation_done (Fixture *fixture)
{
  return fixture->operation_done;
}

static gboolean
info_done (Fixture *fixture)
{
  return fixture->info_done;
}

static void
context_state (pa_context *context, void *data)
{
  Fixture *fixture = data;

  (void)context;
  notify (fixture);
}

static void
stream_state (pa_stream *stream, void *data)
{
  Fixture *fixture = data;

  (void)stream;
  notify (fixture);
}

static CdAudioStream *
find_stream (Fixture *fixture, gboolean input, const char *application_id)
{
  GPtrArray *streams = cd_audio_backend_get_streams (CD_AUDIO_BACKEND (fixture->backend));

  for (guint index = 0; index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);

      if (stream->input == input
          && g_strcmp0 (stream->identity.application_id, application_id) == 0)
        return stream;
    }
  return NULL;
}

static CdAudioStream *
find_stable_stream (Fixture *fixture, const char *stable_id)
{
  GPtrArray *streams = cd_audio_backend_get_streams (CD_AUDIO_BACKEND (fixture->backend));

  for (guint index = 0; index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);

      if (!stream->input && g_strcmp0 (stream->stable_id, stable_id) == 0)
        return stream;
    }
  return NULL;
}

static gboolean
backend_empty (Fixture *fixture)
{
  return fixture->backend_changes >= fixture->minimum_changes
         && cd_audio_backend_get_streams (CD_AUDIO_BACKEND (fixture->backend))->len == 0;
}

static gboolean
backend_has_both_streams (Fixture *fixture)
{
  return fixture->backend_changes >= fixture->minimum_changes
         && find_stream (fixture, FALSE, TEST_APP_ID) != NULL
         && find_stream (fixture, TRUE, TEST_RECORD_APP_ID) != NULL;
}

static gboolean
playback_is_running (Fixture *fixture)
{
  CdAudioStream *stream = find_stream (fixture, FALSE, TEST_APP_ID);

  return fixture->backend_changes >= fixture->minimum_changes && stream != NULL && stream->running;
}

static gboolean
playback_is_corked (Fixture *fixture)
{
  CdAudioStream *stream = find_stream (fixture, FALSE, TEST_APP_ID);

  return fixture->backend_changes >= fixture->minimum_changes && stream != NULL && !stream->running;
}

static gboolean
recording_is_running (Fixture *fixture)
{
  CdAudioStream *stream = find_stream (fixture, TRUE, TEST_RECORD_APP_ID);

  return fixture->backend_changes >= fixture->minimum_changes && stream != NULL && stream->running;
}

static gboolean
recording_is_corked (Fixture *fixture)
{
  CdAudioStream *stream = find_stream (fixture, TRUE, TEST_RECORD_APP_ID);

  return fixture->backend_changes >= fixture->minimum_changes && stream != NULL && !stream->running;
}

static gboolean
backend_has_identity_fallbacks (Fixture *fixture)
{
  return fixture->backend_changes >= fixture->minimum_changes
         && find_stable_stream (fixture, "binary:binary-only") != NULL
         && find_stable_stream (fixture, "name:Name fallback") != NULL;
}

static gboolean
identity_streams_were_removed (Fixture *fixture)
{
  return fixture->backend_changes >= fixture->minimum_changes
         && find_stable_stream (fixture, "binary:binary-only") == NULL
         && find_stable_stream (fixture, "name:Name fallback") == NULL;
}

static gboolean
recording_was_removed (Fixture *fixture)
{
  return fixture->backend_changes >= fixture->minimum_changes
         && find_stream (fixture, FALSE, TEST_APP_ID) != NULL
         && find_stream (fixture, TRUE, TEST_RECORD_APP_ID) == NULL;
}

static gboolean
playback_has_expected_volume (Fixture *fixture)
{
  CdAudioStream *stream = find_stream (fixture, FALSE, TEST_APP_ID);

  return fixture->backend_changes >= fixture->minimum_changes && stream != NULL
         && pa_cvolume_max (&stream->volume) == fixture->expected_volume;
}

static gboolean
backend_failed (Fixture *fixture)
{
  return fixture->backend_failures >= fixture->minimum_failures;
}

static void
backend_changed (CdAudioBackend *backend, gpointer data)
{
  Fixture *fixture = data;

  (void)backend;
  fixture->backend_changes++;
  notify (fixture);
}

static void
backend_failure (CdAudioBackend *backend, const char *message, gpointer data)
{
  Fixture *fixture = data;

  (void)backend;
  g_assert_nonnull (message);
  fixture->backend_failures++;
  notify (fixture);
}

static void
operation_complete (pa_context *context, int success, void *data)
{
  Fixture *fixture = data;

  (void)context;
  fixture->operation_done = TRUE;
  fixture->operation_succeeded = success != 0;
  notify (fixture);
}

static void
stream_operation_complete (pa_stream *stream, int success, void *data)
{
  Fixture *fixture = data;

  (void)stream;
  fixture->operation_done = TRUE;
  fixture->operation_succeeded = success != 0;
  notify (fixture);
}

static void
volume_set (GObject *object, GAsyncResult *result, gpointer data)
{
  Fixture *fixture = data;

  g_clear_error (&fixture->error);
  fixture->operation_succeeded
      = cd_audio_backend_set_volume_finish (CD_AUDIO_BACKEND (object), result, &fixture->error);
  fixture->operation_done = TRUE;
  notify (fixture);
}

static void
mute_info (pa_context *context, const pa_sink_input_info *info, int eol, void *data)
{
  Fixture *fixture = data;

  (void)context;
  if (eol < 0)
    {
      fixture->info_done = TRUE;
      notify (fixture);
      return;
    }
  if (info != NULL)
    {
      fixture->mute = info->mute != 0;
      fixture->info_done = TRUE;
      notify (fixture);
    }
}

static void
reset_operation (Fixture *fixture)
{
  fixture->operation_done = FALSE;
  fixture->operation_succeeded = FALSE;
  g_clear_error (&fixture->error);
}

static void
wait_success_operation (Fixture *fixture, pa_operation *operation)
{
  g_assert_nonnull (operation);
  pa_operation_unref (operation);
  wait_until (fixture, operation_done);
  g_assert_true (fixture->operation_succeeded);
}

static void
connect_control (Fixture *fixture)
{
  fixture->context
      = pa_context_new (pa_glib_mainloop_get_api (fixture->pulse_loop), "backend-test-control");
  g_assert_nonnull (fixture->context);
  pa_context_set_state_callback (fixture->context, context_state, fixture);
  g_assert_cmpint (pa_context_connect (fixture->context, NULL, PA_CONTEXT_NOFLAGS, NULL), ==, 0);
  wait_until (fixture, context_ready);
}

static void
clear_stream (pa_stream **stream)
{
  if (*stream == NULL)
    return;
  pa_stream_set_state_callback (*stream, NULL, NULL);
  pa_stream_disconnect (*stream);
  pa_stream_unref (g_steal_pointer (stream));
}

static void
clear_control (Fixture *fixture)
{
  clear_stream (&fixture->playback);
  clear_stream (&fixture->recording);
  clear_stream (&fixture->binary_identity);
  clear_stream (&fixture->name_identity);
  if (fixture->context != NULL)
    {
      pa_context_set_state_callback (fixture->context, NULL, NULL);
      pa_context_disconnect (fixture->context);
      pa_context_unref (g_steal_pointer (&fixture->context));
    }
}

static pa_stream *
create_identity_playback (Fixture *fixture, const char *binary, const char *name)
{
  pa_proplist *properties = pa_proplist_new ();
  pa_sample_spec sample_spec = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 1 };
  pa_stream *stream;

  pa_proplist_sets (properties, "pipewire.access.portal.app_id", "");
  pa_proplist_sets (properties, PA_PROP_APPLICATION_ID, "");
  pa_proplist_sets (properties, PA_PROP_APPLICATION_PROCESS_BINARY, binary);
  pa_proplist_sets (properties, PA_PROP_APPLICATION_NAME, name);
  stream = pa_stream_new_with_proplist (fixture->context, name, &sample_spec, NULL, properties);
  pa_proplist_free (properties);
  g_assert_nonnull (stream);
  pa_stream_set_state_callback (stream, stream_state, fixture);
  g_assert_cmpint (
      pa_stream_connect_playback (stream, NULL, NULL, PA_STREAM_START_CORKED, NULL, NULL), ==, 0);
  return stream;
}

static void
create_playback (Fixture *fixture, pa_stream_flags_t flags)
{
  pa_proplist *properties = pa_proplist_new ();
  pa_sample_spec sample_spec = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2 };

  pa_proplist_sets (properties, "pipewire.access.portal.app_id", TEST_PORTAL_ID);
  pa_proplist_sets (properties, PA_PROP_APPLICATION_ID, TEST_APP_ID);
  pa_proplist_sets (properties, PA_PROP_APPLICATION_PROCESS_BINARY, "identity-fallback");
  pa_proplist_sets (properties, PA_PROP_APPLICATION_NAME, "CallDucker backend playback test");
  pa_proplist_sets (properties, PA_PROP_APPLICATION_ICON_NAME, "audio-x-generic");
  pa_proplist_sets (properties, PA_PROP_MEDIA_ROLE, "phone");
  pa_proplist_sets (properties, "module-stream-restore.id", TEST_RESTORE_ID);
  fixture->playback = pa_stream_new_with_proplist (fixture->context, "test playback", &sample_spec,
                                                   NULL, properties);
  pa_proplist_free (properties);
  g_assert_nonnull (fixture->playback);
  pa_stream_set_state_callback (fixture->playback, stream_state, fixture);
  g_assert_cmpint (pa_stream_connect_playback (fixture->playback, NULL, NULL, flags, NULL, NULL),
                   ==, 0);
}

static void
create_recording (Fixture *fixture)
{
  pa_proplist *properties = pa_proplist_new ();
  pa_sample_spec sample_spec = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2 };

  pa_proplist_sets (properties, PA_PROP_APPLICATION_ID, TEST_RECORD_APP_ID);
  pa_proplist_sets (properties, PA_PROP_APPLICATION_PROCESS_BINARY, "record-fallback");
  pa_proplist_sets (properties, PA_PROP_APPLICATION_NAME, "CallDucker backend recording test");
  pa_proplist_sets (properties, PA_PROP_MEDIA_ROLE, "communication");
  fixture->recording = pa_stream_new_with_proplist (fixture->context, "test recording",
                                                    &sample_spec, NULL, properties);
  pa_proplist_free (properties);
  g_assert_nonnull (fixture->recording);
  pa_stream_set_state_callback (fixture->recording, stream_state, fixture);
  g_assert_cmpint (pa_stream_connect_record (fixture->recording, "call_ducker_test.monitor", NULL,
                                             PA_STREAM_NOFLAGS),
                   ==, 0);
}

static void
wait_for_restart_file (const char *control_directory)
{
  g_autofree char *ready = g_build_filename (control_directory, "restarted", NULL);
  gint64 deadline = g_get_monotonic_time () + 8 * G_TIME_SPAN_SECOND;

  while (!g_file_test (ready, G_FILE_TEST_EXISTS) && g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (10 * 1000);
    }
  g_assert_true (g_file_test (ready, G_FILE_TEST_EXISTS));
}

static void
private_server_backend_contract (void)
{
  Fixture fixture = { .expected_volume = PA_VOLUME_INVALID };
  CdAudioStream *playback;
  CdAudioStream *recording;
  pa_cvolume target;
  pa_operation *operation;
  guint32 playback_id;
  const char *control_directory = g_getenv ("PULSE_TEST_CONTROL_DIR");

  g_assert_nonnull (control_directory);
  fixture.loop = g_main_loop_new (NULL, FALSE);
  fixture.pulse_loop = pa_glib_mainloop_new (NULL);
  connect_control (&fixture);

  fixture.backend = cd_pulse_backend_new ();
  g_signal_connect (fixture.backend, "changed", G_CALLBACK (backend_changed), &fixture);
  g_signal_connect (fixture.backend, "failed", G_CALLBACK (backend_failure), &fixture);
  cd_pulse_backend_start (fixture.backend);
  fixture.minimum_changes = 1;
  wait_until (&fixture, backend_empty);

  fixture.minimum_changes = fixture.backend_changes + 1;
  create_playback (&fixture, PA_STREAM_START_CORKED);
  create_recording (&fixture);
  wait_until (&fixture, streams_ready);
  wait_until (&fixture, backend_has_both_streams);

  playback = find_stream (&fixture, FALSE, TEST_APP_ID);
  recording = find_stream (&fixture, TRUE, TEST_RECORD_APP_ID);
  g_assert_nonnull (playback);
  g_assert_nonnull (recording);
  g_assert_cmpstr (playback->stable_id, ==, "portal:" TEST_PORTAL_ID);
  g_assert_cmpstr (playback->restore_id, ==, TEST_RESTORE_ID);
  g_assert_cmpstr (playback->identity.media_role, ==, "phone");
  g_assert_false (playback->running);
  g_assert_true (playback->volume_controllable);
  g_assert_true (pa_channel_map_valid (&playback->channel_map));
  g_assert_cmpuint (playback->channel_map.channels, ==, 2);
  g_assert_true (
      pa_cvolume_compatible_with_channel_map (&playback->volume, &playback->channel_map));
  g_assert_cmpstr (recording->stable_id, ==, "application:" TEST_RECORD_APP_ID);
  g_assert_cmpstr (recording->identity.media_role, ==, "communication");
  g_assert_true (recording->running);
  g_assert_false (recording->volume_controllable);
  playback_id = playback->id;

  fixture.minimum_changes = fixture.backend_changes + 1;
  fixture.binary_identity
      = create_identity_playback (&fixture, "binary-only", "Binary fallback name");
  fixture.name_identity = create_identity_playback (&fixture, "", "Name fallback");
  wait_until (&fixture, identity_streams_ready);
  wait_until (&fixture, backend_has_identity_fallbacks);
  g_assert_cmpstr (find_stable_stream (&fixture, "binary:binary-only")->display_name, ==,
                   "Binary fallback name");
  g_assert_cmpstr (find_stable_stream (&fixture, "name:Name fallback")->display_name, ==,
                   "Name fallback");
  fixture.minimum_changes = fixture.backend_changes + 1;
  clear_stream (&fixture.binary_identity);
  clear_stream (&fixture.name_identity);
  wait_until (&fixture, identity_streams_were_removed);

  fixture.minimum_changes = fixture.backend_changes + 1;
  reset_operation (&fixture);
  operation = pa_stream_cork (fixture.recording, 1, stream_operation_complete, &fixture);
  wait_success_operation (&fixture, operation);
  wait_until (&fixture, recording_is_corked);
  fixture.minimum_changes = fixture.backend_changes + 1;
  reset_operation (&fixture);
  operation = pa_stream_cork (fixture.recording, 0, stream_operation_complete, &fixture);
  wait_success_operation (&fixture, operation);
  wait_until (&fixture, recording_is_running);

  fixture.minimum_changes = fixture.backend_changes + 1;
  reset_operation (&fixture);
  operation = pa_stream_cork (fixture.playback, 0, stream_operation_complete, &fixture);
  wait_success_operation (&fixture, operation);
  wait_until (&fixture, playback_is_running);
  fixture.minimum_changes = fixture.backend_changes + 1;
  reset_operation (&fixture);
  operation = pa_stream_cork (fixture.playback, 1, stream_operation_complete, &fixture);
  wait_success_operation (&fixture, operation);
  wait_until (&fixture, playback_is_corked);

  reset_operation (&fixture);
  operation = pa_context_set_sink_input_mute (fixture.context, playback_id, 1, operation_complete,
                                              &fixture);
  wait_success_operation (&fixture, operation);
  playback = find_stream (&fixture, FALSE, TEST_APP_ID);
  g_assert_nonnull (playback);
  fixture.expected_volume = (pa_volume_t)(0.37 * PA_VOLUME_NORM + 0.5);
  pa_cvolume_set (&target, playback->channel_map.channels, fixture.expected_volume);
  fixture.minimum_changes = fixture.backend_changes + 1;
  reset_operation (&fixture);
  cd_audio_backend_set_volume_async (CD_AUDIO_BACKEND (fixture.backend), playback_id,
                                     "portal:" TEST_PORTAL_ID, TEST_RESTORE_ID,
                                     &playback->channel_map, &target, NULL, volume_set, &fixture);
  wait_until (&fixture, operation_done);
  g_assert_true (fixture.operation_succeeded);
  g_assert_no_error (fixture.error);
  wait_until (&fixture, playback_has_expected_volume);
  playback = find_stream (&fixture, FALSE, TEST_APP_ID);
  g_assert_nonnull (playback);

  fixture.info_done = FALSE;
  operation = pa_context_get_sink_input_info (fixture.context, playback_id, mute_info, &fixture);
  g_assert_nonnull (operation);
  pa_operation_unref (operation);
  wait_until (&fixture, info_done);
  g_assert_true (fixture.mute);

  reset_operation (&fixture);
  cd_audio_backend_set_volume_async (CD_AUDIO_BACKEND (fixture.backend), playback_id,
                                     "application:wrong", TEST_RESTORE_ID, &playback->channel_map,
                                     &target, NULL, volume_set, &fixture);
  wait_until (&fixture, operation_done);
  g_assert_false (fixture.operation_succeeded);
  g_assert_error (fixture.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  playback = find_stream (&fixture, FALSE, TEST_APP_ID);
  g_assert_nonnull (playback);
  {
    pa_channel_map unexpected_map = playback->channel_map;
    pa_channel_position_t first = unexpected_map.map[0];

    unexpected_map.map[0] = unexpected_map.map[1];
    unexpected_map.map[1] = first;
    reset_operation (&fixture);
    cd_audio_backend_set_volume_async (CD_AUDIO_BACKEND (fixture.backend), playback_id,
                                       "portal:" TEST_PORTAL_ID, TEST_RESTORE_ID, &unexpected_map,
                                       &target, NULL, volume_set, &fixture);
    wait_until (&fixture, operation_done);
    g_assert_false (fixture.operation_succeeded);
    g_assert_error (fixture.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  }

  {
    pa_proplist *properties = pa_proplist_new ();

    pa_proplist_sets (properties, "pipewire.access.portal.app_id", "org.example.ChangedIdentity");
    reset_operation (&fixture);
    operation = pa_stream_proplist_update (fixture.playback, PA_UPDATE_REPLACE, properties,
                                           stream_operation_complete, &fixture);
    pa_proplist_free (properties);
    wait_success_operation (&fixture, operation);
  }
  playback = find_stream (&fixture, FALSE, TEST_APP_ID);
  g_assert_nonnull (playback);
  g_assert_cmpstr (playback->stable_id, ==, "portal:" TEST_PORTAL_ID);
  reset_operation (&fixture);
  cd_audio_backend_set_volume_async (CD_AUDIO_BACKEND (fixture.backend), playback_id,
                                     "portal:" TEST_PORTAL_ID, TEST_RESTORE_ID,
                                     &playback->channel_map, &target, NULL, volume_set, &fixture);
  wait_until (&fixture, operation_done);
  g_assert_false (fixture.operation_succeeded);
  g_assert_error (fixture.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  fixture.minimum_changes = fixture.backend_changes + 1;
  clear_stream (&fixture.recording);
  wait_until (&fixture, recording_was_removed);

  fixture.minimum_failures = fixture.backend_failures + 1;
  fixture.minimum_changes = fixture.backend_changes + 1;
  {
    g_autofree char *restart = g_build_filename (control_directory, "restart", NULL);
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (restart, "restart\n", -1, &error));
    g_assert_no_error (error);
  }
  wait_until (&fixture, backend_failed);
  wait_for_restart_file (control_directory);
  clear_control (&fixture);
  connect_control (&fixture);
  wait_until (&fixture, backend_empty);

  fixture.minimum_changes = fixture.backend_changes + 1;
  create_playback (&fixture, PA_STREAM_NOFLAGS);
  fixture.recording = NULL;
  wait_until (&fixture, playback_control_ready);
  wait_until (&fixture, playback_is_running);
  playback = find_stream (&fixture, FALSE, TEST_APP_ID);
  g_assert_nonnull (playback);
  fixture.expected_volume = (pa_volume_t)(0.63 * PA_VOLUME_NORM + 0.5);
  pa_cvolume_set (&target, playback->channel_map.channels, fixture.expected_volume);
  fixture.minimum_changes = fixture.backend_changes + 1;
  reset_operation (&fixture);
  cd_audio_backend_set_volume_async (CD_AUDIO_BACKEND (fixture.backend), playback->id,
                                     playback->stable_id, playback->restore_id,
                                     &playback->channel_map, &target, NULL, volume_set, &fixture);
  wait_until (&fixture, operation_done);
  g_assert_true (fixture.operation_succeeded);
  g_assert_no_error (fixture.error);
  wait_until (&fixture, playback_has_expected_volume);

  g_clear_error (&fixture.error);
  g_clear_object (&fixture.backend);
  clear_control (&fixture);
  pa_glib_mainloop_free (fixture.pulse_loop);
  g_main_loop_unref (fixture.loop);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/pulse/private-server-backend-contract", private_server_backend_contract);
  return g_test_run ();
}
