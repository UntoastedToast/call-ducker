#pragma once
#include <glib.h>

typedef enum
{
  CD_POLICY_AUTOMATIC,
  CD_POLICY_ALWAYS,
  CD_POLICY_NEVER
} CdPolicy;

typedef struct
{
  const char *portal_id;
  const char *application_id;
  const char *binary;
  const char *name;
  const char *media_role;
  gboolean launcher_game;
} CdAppIdentity;

gboolean cd_selector_matches (const char *selector, const CdAppIdentity *app);
CdPolicy cd_resolve_policy (const CdAppIdentity *app, const char *const *always,
                            const char *const *never, gboolean auto_detect_games);
gboolean cd_is_discord (const CdAppIdentity *app);
gboolean cd_is_game (const CdAppIdentity *app);
gboolean cd_is_protected (const CdAppIdentity *app);
