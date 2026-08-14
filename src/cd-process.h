#pragma once
#include <glib.h>

gboolean cd_environ_has_launcher_game (const char *data, gsize length);
gboolean cd_process_has_launcher_game (guint pid);
