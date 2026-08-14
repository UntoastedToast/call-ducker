#pragma once

#include "cd-audio-backend.h"

G_BEGIN_DECLS

#define TEST_TYPE_AUDIO_BACKEND (test_audio_backend_get_type ())
G_DECLARE_FINAL_TYPE (TestAudioBackend, test_audio_backend, TEST, AUDIO_BACKEND, GObject)

TestAudioBackend *test_audio_backend_new (void);
CdAudioStream *test_audio_backend_add_stream (TestAudioBackend *self, guint32 id,
                                              const char *stable_id, const char *name,
                                              gboolean input, gboolean running,
                                              const char *application_id, const char *binary,
                                              const char *media_role, gboolean launcher_game,
                                              double volume);
void test_audio_backend_clear (TestAudioBackend *self);
void test_audio_backend_changed (TestAudioBackend *self);
void test_audio_backend_fail (TestAudioBackend *self, const char *message);
double test_audio_backend_get_main_volume (TestAudioBackend *self, guint32 id);
const pa_cvolume *test_audio_backend_get_volume (TestAudioBackend *self, guint32 id);
void test_audio_backend_set_delayed (TestAudioBackend *self, gboolean delayed);
guint test_audio_backend_get_pending_sets (TestAudioBackend *self);
guint test_audio_backend_get_set_count (TestAudioBackend *self);
gboolean test_audio_backend_complete_next (TestAudioBackend *self, gboolean succeed);
void test_audio_backend_fail_next_set (TestAudioBackend *self, const char *message);
void test_audio_backend_set_stream_volume (TestAudioBackend *self, guint32 id, double volume);
void test_audio_backend_set_stream_layout (TestAudioBackend *self, guint32 id,
                                           const pa_channel_map *channel_map,
                                           const pa_cvolume *volume);

G_END_DECLS
