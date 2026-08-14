#include "cd-engine.h"
#include <glib.h>
static guint changes;
static void
changed (CdEngine *e, gpointer d)
{
  (void)e;
  (void)d;
  changes++;
}
static void
lifecycle (void)
{
  CdEngine *e = cd_engine_new (changed, NULL);
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_DISABLED);
  cd_engine_set_enabled (e, TRUE);
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_LISTENING);
  cd_engine_set_trigger (e, "discord:1", "Discord", TRUE);
  cd_engine_set_trigger (e, "discord:2", "Discord", TRUE);
  g_assert_true (cd_engine_should_duck (e));
  cd_engine_set_trigger (e, "discord:1", "Discord", FALSE);
  g_assert_true (cd_engine_should_duck (e));
  cd_engine_pause_for_call (e);
  g_assert_false (cd_engine_should_duck (e));
  cd_engine_set_trigger (e, "discord:2", "Discord", FALSE);
  cd_engine_set_trigger (e, "discord:3", "Discord", TRUE);
  g_assert_true (cd_engine_should_duck (e));
  cd_engine_set_available (e, FALSE, "gone");
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_UNAVAILABLE);
  g_assert_cmpstr (cd_engine_get_error (e), ==, "gone");
  cd_engine_free (e);
}
static void
notice_reports_without_an_outage (void)
{
  CdEngine *e = cd_engine_new (NULL, NULL);

  cd_engine_set_enabled (e, TRUE);
  cd_engine_set_available (e, TRUE, "");
  cd_engine_set_notice (e, "Restore identity is ambiguous");
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_LISTENING);
  g_assert_cmpstr (cd_engine_get_error (e), ==, "Restore identity is ambiguous");
  cd_engine_set_trigger (e, "discord:1", "Discord", TRUE);
  g_assert_true (cd_engine_should_duck (e));
  cd_engine_set_trigger (e, "discord:1", "Discord", FALSE);

  /* A real outage keeps precedence over a notice. */
  cd_engine_set_available (e, FALSE, "gone");
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_UNAVAILABLE);
  g_assert_cmpstr (cd_engine_get_error (e), ==, "gone");

  cd_engine_set_available (e, TRUE, "");
  g_assert_cmpstr (cd_engine_get_error (e), ==, "Restore identity is ambiguous");
  cd_engine_set_notice (e, "");
  g_assert_cmpstr (cd_engine_get_error (e), ==, "");
  cd_engine_free (e);
}

static void
disable_clears_call (void)
{
  CdEngine *e = cd_engine_new (NULL, NULL);
  cd_engine_set_enabled (e, TRUE);
  cd_engine_set_trigger (e, "discord:1", "Discord", TRUE);
  g_assert_true (cd_engine_should_duck (e));
  cd_engine_set_enabled (e, FALSE);
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_DISABLED);
  g_assert_false (cd_engine_should_duck (e));
  g_auto (GStrv) triggers = cd_engine_dup_trigger_names (e);
  g_assert_cmpuint (g_strv_length (triggers), ==, 0);
  cd_engine_set_enabled (e, TRUE);
  g_assert_cmpint (cd_engine_get_state (e), ==, CD_STATE_LISTENING);
  g_assert_false (cd_engine_should_duck (e));
  cd_engine_free (e);
}
static void
targets (void)
{
  CdEngine *e = cd_engine_new (NULL, NULL);
  cd_engine_set_ducked (e, "1", "Game One", TRUE);
  cd_engine_set_ducked (e, "2", "Game Two", TRUE);
  g_auto (GStrv) v = cd_engine_dup_ducked_names (e);
  g_assert_cmpuint (g_strv_length (v), ==, 2);
  cd_engine_set_pending (e, 2);
  g_assert_cmpuint (cd_engine_get_pending (e), ==, 2);
  cd_engine_free (e);
}
static void
deterministic_and_coalesced (void)
{
  CdEngine *e = cd_engine_new (changed, NULL);
  changes = 0;
  cd_engine_set_enabled (e, TRUE);
  g_assert_cmpuint (changes, ==, 1);
  cd_engine_set_enabled (e, TRUE);
  g_assert_cmpuint (changes, ==, 1);
  cd_engine_set_trigger (e, "z", "Zulu", TRUE);
  cd_engine_set_trigger (e, "a", "Alpha", TRUE);
  cd_engine_set_trigger (e, "a", "Alpha", TRUE);
  g_assert_cmpuint (changes, ==, 3);
  g_auto (GStrv) v = cd_engine_dup_trigger_names (e);
  g_assert_cmpstr (v[0], ==, "Alpha");
  g_assert_cmpstr (v[1], ==, "Zulu");
  cd_engine_set_pending (e, 0);
  g_assert_cmpuint (changes, ==, 3);
  cd_engine_free (e);
}
int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/engine/lifecycle", lifecycle);
  g_test_add_func ("/engine/notice-without-outage", notice_reports_without_an_outage);
  g_test_add_func ("/engine/disable-clears-call", disable_clears_call);
  g_test_add_func ("/engine/targets", targets);
  g_test_add_func ("/engine/deterministic-and-coalesced", deterministic_and_coalesced);
  return g_test_run ();
}
