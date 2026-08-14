#include "cd-application-registry.h"

#include "cd-match.h"

typedef struct
{
  char *name;
  char *icon;
  gint64 seen;
  gboolean input;
  gboolean game;
  gboolean available;
} Recent;

struct _CdApplicationRegistry
{
  GSettings *settings;
  CdAudioBackend *backend;
  CdStateStore *store;
  GHashTable *recent;
};

static void
recent_free (gpointer data)
{
  Recent *recent = data;

  g_free (recent->name);
  g_free (recent->icon);
  g_free (recent);
}

static Recent *
recent_copy (const Recent *source)
{
  Recent *copy = g_new0 (Recent, 1);

  copy->name = g_strdup (source->name);
  copy->icon = g_strdup (source->icon);
  copy->seen = source->seen;
  copy->input = source->input;
  copy->game = source->game;
  copy->available = source->available;
  return copy;
}

static gboolean
recent_equal (const Recent *left, const Recent *right)
{
  return g_strcmp0 (left->name, right->name) == 0 && g_strcmp0 (left->icon, right->icon) == 0
         && left->input == right->input && left->game == right->game
         && left->available == right->available;
}

static gboolean
catalog_equal (GHashTable *left, GHashTable *right)
{
  GHashTableIter iterator;
  gpointer key;
  gpointer value;

  if (g_hash_table_size (left) != g_hash_table_size (right))
    return FALSE;
  g_hash_table_iter_init (&iterator, left);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      Recent *other = g_hash_table_lookup (right, key);
      if (other == NULL || !recent_equal (value, other))
        return FALSE;
    }
  return TRUE;
}

static gboolean
selector_configured (CdApplicationRegistry *self, const char *id, gboolean input)
{
  g_auto (GStrv) primary = g_settings_get_strv (self->settings, input ? "call-app-selectors"
                                                                      : "always-duck-selectors");

  for (guint index = 0; primary != NULL && primary[index] != NULL; index++)
    if (g_str_equal (primary[index], id))
      return TRUE;
  if (input)
    return FALSE;
  g_auto (GStrv) never = g_settings_get_strv (self->settings, "never-duck-selectors");
  for (guint index = 0; never != NULL && never[index] != NULL; index++)
    if (g_str_equal (never[index], id))
      return TRUE;
  return FALSE;
}

static const char *
entry_id (const char *key)
{
  const char *separator = strchr (key, '|');

  return separator != NULL ? separator + 1 : key;
}

static gboolean
recent_is_protected (const char *id, const Recent *recent)
{
  CdAppIdentity identity = { .name = recent->name };

  if (g_str_has_prefix (id, "portal:"))
    identity.portal_id = id + strlen ("portal:");
  else if (g_str_has_prefix (id, "application:"))
    identity.application_id = id + strlen ("application:");
  else if (g_str_has_prefix (id, "binary:"))
    identity.binary = id + strlen ("binary:");
  return cd_is_protected (&identity) || (recent->input && recent->game);
}

static JsonNode *
serialize (GHashTable *catalog)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  GHashTableIter iterator;
  gpointer key;
  gpointer value;

  json_builder_begin_object (builder);
  g_hash_table_iter_init (&iterator, catalog);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      Recent *recent = value;

      json_builder_set_member_name (builder, key);
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "name");
      json_builder_add_string_value (builder, recent->name);
      json_builder_set_member_name (builder, "icon");
      json_builder_add_string_value (builder, recent->icon);
      json_builder_set_member_name (builder, "lastSeen");
      json_builder_add_int_value (builder, recent->seen);
      json_builder_set_member_name (builder, "input");
      json_builder_add_boolean_value (builder, recent->input);
      json_builder_set_member_name (builder, "game");
      json_builder_add_boolean_value (builder, recent->game);
      json_builder_end_object (builder);
    }
  json_builder_end_object (builder);
  return json_builder_get_root (builder);
}

CdApplicationRegistry *
cd_application_registry_new (GSettings *settings, CdAudioBackend *backend, CdStateStore *store)
{
  CdApplicationRegistry *self;

  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (backend == NULL || CD_IS_AUDIO_BACKEND (backend), NULL);
  g_return_val_if_fail (store != NULL, NULL);
  self = g_new0 (CdApplicationRegistry, 1);
  self->settings = g_object_ref (settings);
  self->backend = backend != NULL ? g_object_ref (backend) : NULL;
  self->store = store;
  self->recent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, recent_free);
  return self;
}

void
cd_application_registry_free (CdApplicationRegistry *self)
{
  if (self == NULL)
    return;
  g_hash_table_unref (self->recent);
  cd_state_store_free (self->store);
  g_clear_object (&self->backend);
  g_object_unref (self->settings);
  g_free (self);
}

gboolean
cd_application_registry_load (CdApplicationRegistry *self, GError **error)
{
  g_autoptr (JsonNode) root = cd_state_store_load (self->store, error);
  JsonObject *object;
  g_autoptr (GList) members = NULL;
  g_autoptr (GHashTable) loaded
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, recent_free);

  if (root == NULL)
    return FALSE;
  if (!JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Application registry root is not an object");
      return FALSE;
    }
  object = json_node_get_object (root);
  members = json_object_get_members (object);
  for (GList *member = members; member != NULL; member = member->next)
    {
      const char *key = member->data;
      JsonNode *node = json_object_get_member (object, key);
      JsonObject *entry;
      Recent *recent;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Application registry entry '%s' is not an object", key);
          return FALSE;
        }
      entry = json_node_get_object (node);
      recent = g_new0 (Recent, 1);
      recent->name = g_strdup (
          json_object_get_string_member_with_default (entry, "name", "Unknown application"));
      recent->icon = g_strdup (json_object_get_string_member_with_default (
          entry, "icon", "application-x-executable-symbolic"));
      recent->seen = json_object_get_int_member_with_default (entry, "lastSeen", 0);
      recent->input = json_object_get_boolean_member_with_default (entry, "input", FALSE);
      CdAppIdentity identity = { .name = recent->name };
      recent->game = json_object_get_boolean_member_with_default (entry, "game", FALSE)
                     || cd_is_game (&identity);
      recent->available = FALSE;
      g_hash_table_replace (loaded, g_strdup (key), recent);
    }
  g_hash_table_remove_all (self->recent);
  GHashTableIter iterator;
  gpointer key;
  gpointer value;
  g_hash_table_iter_init (&iterator, loaded);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      g_hash_table_iter_steal (&iterator);
      g_hash_table_insert (self->recent, key, value);
    }
  return TRUE;
}

gboolean
cd_application_registry_refresh (CdApplicationRegistry *self, gboolean *changed, GError **error)
{
  g_autoptr (GHashTable) next
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, recent_free);
  GHashTableIter iterator;
  gpointer key;
  gpointer value;
  GPtrArray *streams;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  g_hash_table_iter_init (&iterator, self->recent);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      Recent *copy = recent_copy (value);
      copy->available = FALSE;
      g_hash_table_insert (next, g_strdup (key), copy);
    }
  streams = self->backend != NULL ? cd_audio_backend_get_streams (self->backend) : NULL;
  for (guint index = 0; streams != NULL && index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);
      gboolean game = cd_is_game (&stream->identity);
      g_autofree char *catalog_key = NULL;
      Recent *recent;

      if (cd_is_protected (&stream->identity) || (stream->input && game)
          || (!stream->input && !stream->volume_controllable) || stream->stable_id == NULL)
        continue;
      catalog_key = g_strdup_printf ("%c|%s", stream->input ? 'i' : 'o', stream->stable_id);
      recent = g_new0 (Recent, 1);
      recent->name
          = g_strdup (stream->display_name != NULL ? stream->display_name : "Unknown application");
      recent->icon
          = g_strdup (stream->icon != NULL ? stream->icon : "application-x-executable-symbolic");
      recent->seen = now;
      recent->input = stream->input;
      recent->game = game;
      recent->available = TRUE;
      g_hash_table_replace (next, g_steal_pointer (&catalog_key), recent);
    }
  g_hash_table_iter_init (&iterator, next);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      Recent *recent = value;
      const char *id = entry_id (key);

      if (recent_is_protected (id, recent)
          || (!recent->available && !selector_configured (self, id, recent->input)))
        g_hash_table_iter_remove (&iterator);
    }
  if (changed != NULL)
    *changed = !catalog_equal (self->recent, next);
  g_autoptr (JsonNode) root = serialize (next);
  if (!cd_state_store_save (self->store, root, error))
    return FALSE;
  g_hash_table_remove_all (self->recent);
  g_hash_table_iter_init (&iterator, next);
  while (g_hash_table_iter_next (&iterator, &key, &value))
    {
      g_hash_table_iter_steal (&iterator);
      g_hash_table_insert (self->recent, key, value);
    }
  return TRUE;
}

static gint
compare_keys (gconstpointer left, gconstpointer right)
{
  return g_strcmp0 (*(const char *const *)left, *(const char *const *)right);
}

GVariant *
cd_application_registry_dup_applications (CdApplicationRegistry *self)
{
  GVariantBuilder output;
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter iterator;
  gpointer key;

  g_hash_table_iter_init (&iterator, self->recent);
  while (g_hash_table_iter_next (&iterator, &key, NULL))
    g_ptr_array_add (keys, key);
  g_ptr_array_sort (keys, compare_keys);
  g_variant_builder_init (&output, G_VARIANT_TYPE ("aa{sv}"));
  for (guint index = 0; index < keys->len; index++)
    {
      const char *catalog_key = g_ptr_array_index (keys, index);
      Recent *recent = g_hash_table_lookup (self->recent, catalog_key);
      GVariantBuilder application;

      g_variant_builder_init (&application, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&application, "{sv}", "id",
                             g_variant_new_string (entry_id (catalog_key)));
      g_variant_builder_add (&application, "{sv}", "name", g_variant_new_string (recent->name));
      g_variant_builder_add (&application, "{sv}", "icon", g_variant_new_string (recent->icon));
      g_variant_builder_add (&application, "{sv}", "input", g_variant_new_boolean (recent->input));
      g_variant_builder_add (&application, "{sv}", "game", g_variant_new_boolean (recent->game));
      g_variant_builder_add (&application, "{sv}", "available",
                             g_variant_new_boolean (recent->available));
      g_variant_builder_add (&application, "{sv}", "lastSeen", g_variant_new_int64 (recent->seen));
      g_variant_builder_add_value (&output, g_variant_builder_end (&application));
    }
  return g_variant_ref_sink (g_variant_builder_end (&output));
}
