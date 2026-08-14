#include "cd-match.h"
#include <string.h>

static gboolean
contains_ci (const char *haystack, const char *needle)
{
  if (!haystack || !needle)
    return FALSE;
  g_autofree char *h = g_utf8_strdown (haystack, -1);
  g_autofree char *n = g_utf8_strdown (needle, -1);
  return strstr (h, n) != NULL;
}

gboolean
cd_is_discord (const CdAppIdentity *a)
{
  return contains_ci (a->portal_id, "com.discordapp.discord")
         || contains_ci (a->application_id, "discord")
         || (a->binary
             && (g_ascii_strcasecmp (a->binary, "discord") == 0
                 || g_ascii_strcasecmp (a->binary, "vesktop") == 0));
}

gboolean
cd_is_protected (const CdAppIdentity *a)
{
  return contains_ci (a->application_id, "callDucker") || contains_ci (a->binary, "call-ducker")
         || cd_is_discord (a)
         || (a->binary && g_ascii_strcasecmp (a->binary, "steamwebhelper") == 0);
}

gboolean
cd_selector_matches (const char *s, const CdAppIdentity *a)
{
  if (!s || !a)
    return FALSE;
  if (g_str_equal (s, "preset:discord"))
    return cd_is_discord (a);
  if (g_str_has_prefix (s, "portal:"))
    return a->portal_id && g_ascii_strcasecmp (s + 7, a->portal_id) == 0;
  if (g_str_has_prefix (s, "application:"))
    return a->application_id && g_ascii_strcasecmp (s + 12, a->application_id) == 0;
  if (g_str_has_prefix (s, "binary:"))
    return a->binary && g_ascii_strcasecmp (s + 7, a->binary) == 0;
  if (g_str_has_prefix (s, "name:"))
    return a->name && g_ascii_strcasecmp (s + 5, a->name) == 0;
  return FALSE;
}

static gboolean
any_selector (const char *const *selectors, const CdAppIdentity *a)
{
  if (!selectors)
    return FALSE;
  for (guint i = 0; selectors[i]; i++)
    if (cd_selector_matches (selectors[i], a))
      return TRUE;
  return FALSE;
}

gboolean
cd_is_game (const CdAppIdentity *a)
{
  if (!a || cd_is_protected (a))
    return FALSE;
  if (a->launcher_game)
    return TRUE;
  if (a->media_role && g_ascii_strcasecmp (a->media_role, "Game") == 0)
    return TRUE;
  return FALSE;
}

CdPolicy
cd_resolve_policy (const CdAppIdentity *a, const char *const *always, const char *const *never,
                   gboolean auto_games)
{
  if (cd_is_protected (a) || any_selector (never, a))
    return CD_POLICY_NEVER;
  if (any_selector (always, a))
    return CD_POLICY_ALWAYS;
  return auto_games && cd_is_game (a) ? CD_POLICY_ALWAYS : CD_POLICY_AUTOMATIC;
}
