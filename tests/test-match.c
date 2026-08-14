#include "cd-match.h"
#include "cd-process.h"
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void
discord (void)
{
  CdAppIdentity a = { .portal_id = "com.discordapp.Discord", .binary = "Discord" };
  g_assert_true (cd_is_discord (&a));
  g_assert_true (cd_selector_matches ("preset:discord", &a));
  g_assert_true (cd_is_protected (&a));
}
static void
games (void)
{
  CdAppIdentity launcher = { .binary = "game", .launcher_game = TRUE };
  g_assert_true (cd_is_game (&launcher));
  CdAppIdentity role = { .media_role = "Game", .binary = "game" };
  g_assert_true (cd_is_game (&role));
  CdAppIdentity cardinal = { .name = "Cardinal", .binary = "wine64-preloader" };
  g_assert_false (cd_is_game (&cardinal));
  CdAppIdentity wine = { .name = "Windows application", .binary = "wine64" };
  g_assert_false (cd_is_game (&wine));
}
static void
launcher_environments (void)
{
  static const char steam_app[] = "X=1\0SteamAppId=813780\0";
  static const char steam_game[] = "SteamGameId=42\0";
  static const char heroic[] = "HEROIC_APP_NAME=example\0";
  static const char lutris[] = "A=B\0LUTRIS_GAME_UUID=1234\0C=D\0";
  static const char empty[] = "SteamAppId=\0SteamGameId=\0HEROIC_APP_NAME=\0LUTRIS_GAME_UUID=\0";
  static const char similar[] = "XSteamAppId=1\0SteamAppIdExtra=2\0steamGameId=3\0";
  g_assert_true (cd_environ_has_launcher_game (steam_app, sizeof steam_app - 1));
  g_assert_true (cd_environ_has_launcher_game (steam_game, sizeof steam_game - 1));
  g_assert_true (cd_environ_has_launcher_game (heroic, sizeof heroic - 1));
  g_assert_true (cd_environ_has_launcher_game (lutris, sizeof lutris - 1));
  g_assert_false (cd_environ_has_launcher_game (empty, sizeof empty - 1));
  g_assert_false (cd_environ_has_launcher_game (similar, sizeof similar - 1));
}
static void
exclusions (void)
{
  CdAppIdentity web = { .binary = "steamwebhelper", .launcher_game = TRUE, .media_role = "Game" };
  g_assert_false (cd_is_game (&web));
  CdAppIdentity music = { .binary = "rhythmbox", .media_role = "Music" };
  g_assert_false (cd_is_game (&music));
  CdAppIdentity discord_app = { .binary = "discord", .launcher_game = TRUE, .media_role = "Game" };
  g_assert_false (cd_is_game (&discord_app));
  CdAppIdentity self
      = { .application_id = "io.github.UntoastedToast.CallDucker", .launcher_game = TRUE };
  g_assert_false (cd_is_game (&self));
}
static void
priority (void)
{
  CdAppIdentity a = { .binary = "game", .launcher_game = TRUE };
  const char *always[] = { "binary:game", NULL };
  const char *never[] = { "binary:game", NULL };
  g_assert_cmpint (cd_resolve_policy (&a, always, never, TRUE), ==, CD_POLICY_NEVER);
  const char *none[] = { NULL };
  g_assert_cmpint (cd_resolve_policy (&a, always, none, FALSE), ==, CD_POLICY_ALWAYS);
  g_assert_cmpint (cd_resolve_policy (&a, none, none, TRUE), ==, CD_POLICY_ALWAYS);
  g_assert_cmpint (cd_resolve_policy (&a, none, never, TRUE), ==, CD_POLICY_NEVER);
}

static int
launcher_parent_helper (void)
{
  pid_t child = fork ();
  if (child < 0)
    return 2;
  if (child == 0)
    {
      char *const envp[] = { "PATH=/usr/bin:/bin", NULL };
      execle ("/proc/self/exe", "test-match", "--plain-child", NULL, envp);
      _exit (3);
    }
  g_print ("%d\n", child);
  fflush (stdout);
  int status = 0;
  waitpid (child, &status, 0);
  return 0;
}
static void
parent_environment (void)
{
  gchar *helper_argv[] = { "/proc/self/exe", "--launcher-parent-helper", NULL };
  g_auto (GStrv) env = g_get_environ ();
  env = g_environ_setenv (env, "SteamAppId", "123", TRUE);
  GPid helper = 0;
  gint output = -1;
  GError *error = NULL;
  g_assert_true (g_spawn_async_with_pipes (NULL, helper_argv, env, G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                                           NULL, &helper, NULL, &output, NULL, &error));
  g_assert_no_error (error);
  FILE *stream = fdopen (output, "r");
  g_assert_nonnull (stream);
  guint child = 0;
  g_assert_cmpint (fscanf (stream, "%u", &child), ==, 1);
  fclose (stream);
  g_assert_true (cd_process_has_launcher_game (child));
  kill ((pid_t)child, SIGTERM);
  int status = 0;
  waitpid (helper, &status, 0);
  g_spawn_close_pid (helper);
}

int
main (int argc, char **argv)
{
  if (argc > 1 && g_str_equal (argv[1], "--plain-child"))
    {
      pause ();
      return 0;
    }
  if (argc > 1 && g_str_equal (argv[1], "--launcher-parent-helper"))
    return launcher_parent_helper ();
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/match/discord", discord);
  g_test_add_func ("/match/games", games);
  g_test_add_func ("/match/launcher-environments", launcher_environments);
  g_test_add_func ("/match/parent-environment", parent_environment);
  g_test_add_func ("/match/exclusions", exclusions);
  g_test_add_func ("/match/priority", priority);
  return g_test_run ();
}
