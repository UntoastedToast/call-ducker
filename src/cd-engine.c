#include "cd-engine.h"

struct _CdEngine
{
  gboolean enabled;
  gboolean available;
  gboolean paused;
  GHashTable *triggers;
  GHashTable *ducked;
  char *error;
  guint pending;
  CdEngineChanged changed;
  gpointer data;
};

static void
notify (CdEngine *self)
{
  if (self->changed != NULL)
    self->changed (self, self->data);
}

CdEngine *
cd_engine_new (CdEngineChanged changed, gpointer data)
{
  CdEngine *self = g_new0 (CdEngine, 1);

  self->available = TRUE;
  self->changed = changed;
  self->data = data;
  self->triggers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->ducked = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return self;
}

void
cd_engine_free (CdEngine *self)
{
  g_hash_table_unref (self->triggers);
  g_hash_table_unref (self->ducked);
  g_free (self->error);
  g_free (self);
}

void
cd_engine_set_enabled (CdEngine *self, gboolean enabled)
{
  if (self->enabled == enabled)
    return;
  self->enabled = enabled;
  if (!enabled)
    {
      g_hash_table_remove_all (self->triggers);
      self->paused = FALSE;
    }
  notify (self);
}

void
cd_engine_set_available (CdEngine *self, gboolean available, const char *error)
{
  const char *message = error != NULL ? error : "";

  if (self->available == available && g_strcmp0 (self->error, message) == 0)
    return;
  self->available = available;
  g_free (self->error);
  self->error = g_strdup (message);
  notify (self);
}

void
cd_engine_set_trigger (CdEngine *self, const char *id, const char *name, gboolean active)
{
  gboolean changed;

  if (active)
    {
      const char *previous = g_hash_table_lookup (self->triggers, id);
      changed = g_strcmp0 (previous, name) != 0;
      if (changed)
        g_hash_table_replace (self->triggers, g_strdup (id), g_strdup (name));
    }
  else
    {
      changed = g_hash_table_remove (self->triggers, id);
    }
  if (g_hash_table_size (self->triggers) == 0)
    self->paused = FALSE;
  if (changed)
    notify (self);
}

void
cd_engine_end_call (CdEngine *self)
{
  gboolean changed = self->paused || g_hash_table_size (self->triggers) > 0;

  g_hash_table_remove_all (self->triggers);
  self->paused = FALSE;
  if (changed)
    notify (self);
}

void
cd_engine_set_ducked (CdEngine *self, const char *id, const char *name, gboolean ducked)
{
  gboolean changed;

  if (ducked)
    {
      const char *previous = g_hash_table_lookup (self->ducked, id);
      changed = g_strcmp0 (previous, name) != 0;
      if (changed)
        g_hash_table_replace (self->ducked, g_strdup (id), g_strdup (name));
    }
  else
    {
      changed = g_hash_table_remove (self->ducked, id);
    }
  if (changed)
    notify (self);
}

void
cd_engine_clear_ducked (CdEngine *self)
{
  if (g_hash_table_size (self->ducked) == 0)
    return;
  g_hash_table_remove_all (self->ducked);
  notify (self);
}

void
cd_engine_pause_for_call (CdEngine *self)
{
  if (self->paused)
    return;
  self->paused = TRUE;
  notify (self);
}

void
cd_engine_set_pending (CdEngine *self, guint count)
{
  if (self->pending == count)
    return;
  self->pending = count;
  notify (self);
}

CdState
cd_engine_get_state (CdEngine *self)
{
  if (!self->enabled)
    return CD_STATE_DISABLED;
  if (!self->available)
    return CD_STATE_UNAVAILABLE;
  return g_hash_table_size (self->triggers) > 0 ? CD_STATE_CALL_ACTIVE : CD_STATE_LISTENING;
}

const char *
cd_state_name (CdState state)
{
  static const char *names[] = {
    "Disabled",
    "Listening",
    "Call active",
    "Audio service unavailable",
  };

  g_return_val_if_fail (state >= CD_STATE_DISABLED && state <= CD_STATE_UNAVAILABLE, "");
  return names[state];
}

static gint
compare_strings (gconstpointer left, gconstpointer right)
{
  return g_strcmp0 (*(const char *const *)left, *(const char *const *)right);
}

static char **
dup_values (GHashTable *table)
{
  GPtrArray *values = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_ptr_array_add (values, g_strdup (value));
  g_ptr_array_sort (values, compare_strings);
  g_ptr_array_add (values, NULL);
  return (char **)g_ptr_array_free (values, FALSE);
}

char **
cd_engine_dup_trigger_names (CdEngine *self)
{
  return dup_values (self->triggers);
}

char **
cd_engine_dup_ducked_names (CdEngine *self)
{
  return dup_values (self->ducked);
}

const char *
cd_engine_get_error (CdEngine *self)
{
  return self->error != NULL ? self->error : "";
}

guint
cd_engine_get_pending (CdEngine *self)
{
  return self->pending;
}

gboolean
cd_engine_should_duck (CdEngine *self)
{
  return self->enabled && self->available && !self->paused
         && g_hash_table_size (self->triggers) > 0;
}
