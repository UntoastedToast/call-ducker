#include "cd-process.h"
#include <string.h>
#include <unistd.h>

#define MAX_PARENT_DEPTH 16

static gboolean
entry_has_marker (const char *entry, gsize length)
{
  static const char *const keys[]
      = { "SteamAppId", "SteamGameId", "HEROIC_APP_NAME", "LUTRIS_GAME_UUID" };

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      gsize key_length = strlen (keys[i]);
      if (length > key_length + 1 && entry[key_length] == '='
          && memcmp (entry, keys[i], key_length) == 0)
        return TRUE;
    }
  return FALSE;
}

gboolean
cd_environ_has_launcher_game (const char *data, gsize length)
{
  if (!data)
    return FALSE;

  for (gsize offset = 0; offset < length;)
    {
      gsize remaining = length - offset;
      const char *end = memchr (data + offset, '\0', remaining);
      gsize entry_length = end ? (gsize)(end - data - offset) : remaining;
      if (entry_has_marker (data + offset, entry_length))
        return TRUE;
      offset += entry_length + (end != NULL);
    }
  return FALSE;
}

static gboolean
read_proc_file (guint pid, const char *leaf, char **data, gsize *length)
{
  g_autofree char *path = g_strdup_printf ("/proc/%u/%s", pid, leaf);
  return g_file_get_contents (path, data, length, NULL);
}

static gboolean
process_details (guint pid, guint *parent, gboolean *same_user)
{
  g_autofree char *status = NULL;
  gsize length = 0;
  if (!read_proc_file (pid, "status", &status, &length))
    return FALSE;

  *parent = 0;
  *same_user = FALSE;
  for (char *line = status; line && *line;)
    {
      char *next = strchr (line, '\n');
      if (g_str_has_prefix (line, "PPid:"))
        *parent = (guint)g_ascii_strtoull (line + 5, NULL, 10);
      else if (g_str_has_prefix (line, "Uid:"))
        *same_user = (uid_t)g_ascii_strtoull (line + 4, NULL, 10) == getuid ();
      if (!next)
        break;
      line = next + 1;
    }
  return TRUE;
}

gboolean
cd_process_has_launcher_game (guint pid)
{
  for (guint depth = 0; pid > 1 && depth <= MAX_PARENT_DEPTH; depth++)
    {
      guint parent = 0;
      gboolean same_user = FALSE;
      if (!process_details (pid, &parent, &same_user) || !same_user)
        return FALSE;

      g_autofree char *environment = NULL;
      gsize length = 0;
      if (read_proc_file (pid, "environ", &environment, &length)
          && cd_environ_has_launcher_game (environment, length))
        return TRUE;

      if (parent <= 1 || parent == pid)
        break;
      pid = parent;
    }
  return FALSE;
}
