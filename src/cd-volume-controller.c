#include "cd-volume-controller.h"

typedef struct
{
  char *selector;
  guint32 pulse_index;
  char *restore_id;
} RestoreKey;

typedef struct
{
  RestoreKey key;
  char *name;
  pa_cvolume original;
  pa_channel_map channel_map;
  gboolean last_set_valid;
  guint32 last_set_index;
  guint64 last_set_epoch;
  pa_cvolume last_set;
} RestoreEntry;

typedef struct
{
  guint32 pulse_index;
  char *selector;
  char *restore_id;
} TargetIdentity;

typedef enum
{
  REQUEST_RECONCILE,
  REQUEST_RESTORE,
} RequestKind;

typedef struct
{
  RestoreEntry *entry;
  gboolean restore;
  gboolean force;
} Operation;

typedef struct
{
  RestoreEntry *entry;
  guint32 old_index;
} RemapRecord;

typedef struct
{
  RequestKind kind;
  guint64 generation;
  GTask *task;
  GPtrArray *targets;
  double target;
  GQueue operations;
  GHashTable *target_entries;
  GHashTable *restored_entries;
  GPtrArray *new_entries;
  GPtrArray *revived_entries;
  GArray *remaps;
  GError *error;
  gboolean rolling_back;
  gboolean superseded;
  gboolean task_returned;
} ControllerRequest;

struct _CdVolumeController
{
  gint refs;
  CdAudioBackend *backend;
  CdStateStore *store;
  GHashTable *entries;
  GHashTable *tombstones;
  ControllerRequest *current;
  ControllerRequest *pending_restore;
  ControllerRequest *pending_reconcile;
  CdVolumeControllerChanged changed;
  gpointer changed_data;
  GError *journal_error;
  guint64 generation;
  guint64 snapshot_epoch;
  gulong backend_changed_id;
  gboolean set_active;
  gboolean journal_blocked;
  gboolean disposed;
};

typedef enum
{
  RESOLVE_MISSING,
  RESOLVE_FOUND,
  RESOLVE_AMBIGUOUS,
} ResolveResult;

typedef struct
{
  CdVolumeController *controller;
  ControllerRequest *request;
  Operation *operation;
  guint32 index;
  pa_cvolume volume;
} SetCall;

static void process_current (CdVolumeController *self);
static void start_next_request (CdVolumeController *self);
static gboolean stream_is_eligible (CdAudioStream *stream);
static gboolean valid_stream_layout (CdAudioStream *stream);
static gboolean remap_volume (RestoreEntry *entry, CdAudioStream *stream, pa_cvolume *volume,
                              GError **error);

static const char *
normalized_restore_id (const char *restore_id)
{
  return restore_id != NULL ? restore_id : "";
}

static guint
restore_key_hash (gconstpointer data)
{
  const RestoreKey *key = data;
  guint hash = g_str_hash (key->selector);

  hash = (hash * 33U) ^ key->pulse_index;
  return (hash * 33U) ^ g_str_hash (normalized_restore_id (key->restore_id));
}

static gboolean
restore_key_equal (gconstpointer first, gconstpointer second)
{
  const RestoreKey *a = first;
  const RestoreKey *b = second;

  return a->pulse_index == b->pulse_index && g_str_equal (a->selector, b->selector)
         && g_str_equal (normalized_restore_id (a->restore_id),
                         normalized_restore_id (b->restore_id));
}

static void
restore_entry_free (RestoreEntry *entry)
{
  if (entry == NULL)
    return;
  g_free (entry->key.selector);
  g_free (entry->key.restore_id);
  g_free (entry->name);
  g_free (entry);
}

static void
target_identity_free (TargetIdentity *target)
{
  g_free (target->selector);
  g_free (target->restore_id);
  g_free (target);
}

static void
operation_free (Operation *operation)
{
  g_free (operation);
}

static GHashTable *
entries_new (void)
{
  return g_hash_table_new_full (restore_key_hash, restore_key_equal, NULL,
                                (GDestroyNotify)restore_entry_free);
}

static CdVolumeController *
controller_ref (CdVolumeController *self)
{
  g_atomic_int_inc (&self->refs);
  return self;
}

static void
request_free (ControllerRequest *request)
{
  if (request == NULL)
    return;
  g_queue_clear_full (&request->operations, (GDestroyNotify)operation_free);
  g_clear_pointer (&request->targets, g_ptr_array_unref);
  g_hash_table_unref (request->target_entries);
  g_hash_table_unref (request->restored_entries);
  g_ptr_array_unref (request->new_entries);
  g_ptr_array_unref (request->revived_entries);
  g_array_unref (request->remaps);
  g_clear_error (&request->error);
  g_object_unref (request->task);
  g_free (request);
}

static void
controller_unref (CdVolumeController *self)
{
  if (!g_atomic_int_dec_and_test (&self->refs))
    return;
  request_free (self->current);
  request_free (self->pending_restore);
  request_free (self->pending_reconcile);
  g_clear_error (&self->journal_error);
  g_hash_table_unref (self->tombstones);
  g_hash_table_unref (self->entries);
  cd_state_store_free (self->store);
  g_object_unref (self->backend);
  g_free (self);
}

static void
backend_changed (CdAudioBackend *backend, gpointer data)
{
  CdVolumeController *self = data;
  GHashTableIter iterator;
  gpointer value;
  GPtrArray *streams;

  (void)backend;
  self->snapshot_epoch++;
  streams = cd_audio_backend_get_streams (self->backend);
  g_hash_table_iter_init (&iterator, self->tombstones);
  while (g_hash_table_iter_next (&iterator, NULL, &value))
    {
      RestoreEntry *entry = value;
      guint matches = 0;

      for (guint index = 0; streams != NULL && index < streams->len; index++)
        {
          CdAudioStream *stream = g_ptr_array_index (streams, index);

          if (!stream->input && stream->id == entry->key.pulse_index
              && g_strcmp0 (stream->stable_id, entry->key.selector) == 0
              && g_strcmp0 (normalized_restore_id (stream->restore_id),
                            normalized_restore_id (entry->key.restore_id))
                     == 0)
            matches++;
        }
      if (matches == 0)
        {
          g_hash_table_iter_remove (&iterator);
          continue;
        }
      /* The first complete snapshot after the restore acknowledgement is
       * authoritative. It may already contain a user's subsequent mixer
       * change, which must become the next call's new original. */
      if (matches == 1 && self->snapshot_epoch > entry->last_set_epoch)
        g_hash_table_iter_remove (&iterator);
    }
}

CdVolumeController *
cd_volume_controller_new (CdAudioBackend *backend, CdStateStore *store,
                          CdVolumeControllerChanged changed, gpointer data)
{
  CdVolumeController *self;

  g_return_val_if_fail (CD_IS_AUDIO_BACKEND (backend), NULL);
  g_return_val_if_fail (store != NULL, NULL);
  self = g_new0 (CdVolumeController, 1);
  self->refs = 1;
  self->backend = g_object_ref (backend);
  self->store = store;
  self->changed = changed;
  self->changed_data = data;
  self->entries = entries_new ();
  self->tombstones = entries_new ();
  self->backend_changed_id
      = g_signal_connect (backend, "changed", G_CALLBACK (backend_changed), self);
  return self;
}

static void
return_superseded (ControllerRequest *request, const char *message)
{
  request->superseded = TRUE;
  if (request->task_returned)
    return;
  request->task_returned = TRUE;
  g_task_return_new_error (request->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s", message);
}

void
cd_volume_controller_free (CdVolumeController *self)
{
  if (self == NULL)
    return;
  self->disposed = TRUE;
  self->changed = NULL;
  if (self->backend_changed_id != 0)
    g_signal_handler_disconnect (self->backend, self->backend_changed_id);
  self->backend_changed_id = 0;

  if (self->pending_restore != NULL)
    {
      return_superseded (self->pending_restore, "Volume controller was disposed");
      g_clear_pointer (&self->pending_restore, request_free);
    }
  if (self->pending_reconcile != NULL)
    {
      return_superseded (self->pending_reconcile, "Volume controller was disposed");
      g_clear_pointer (&self->pending_reconcile, request_free);
    }
  if (self->current != NULL)
    {
      return_superseded (self->current, "Volume controller was disposed");
      g_queue_clear_full (&self->current->operations, (GDestroyNotify)operation_free);
      if (!self->set_active)
        g_clear_pointer (&self->current, request_free);
    }
  controller_unref (self);
}

pa_cvolume
cd_volume_absolute (const pa_cvolume *original, double target)
{
  pa_cvolume result = *original;
  pa_volume_t value = (pa_volume_t)(CLAMP (target, 0.0, 1.0) * PA_VOLUME_NORM + 0.5);

  if (!pa_cvolume_valid (&result))
    return result;
  if (value > PA_VOLUME_MUTED && pa_cvolume_max (&result) == PA_VOLUME_MUTED)
    pa_cvolume_set (&result, result.channels, value);
  else
    pa_cvolume_scale (&result, value);
  return result;
}

static gint
compare_entries (gconstpointer first, gconstpointer second)
{
  const RestoreEntry *a = first;
  const RestoreEntry *b = second;
  gint compared = g_strcmp0 (a->key.selector, b->key.selector);

  if (compared != 0)
    return compared;
  if (a->key.pulse_index < b->key.pulse_index)
    return -1;
  if (a->key.pulse_index > b->key.pulse_index)
    return 1;
  return g_strcmp0 (normalized_restore_id (a->key.restore_id),
                    normalized_restore_id (b->key.restore_id));
}

static GList *
sorted_entries (CdVolumeController *self)
{
  return g_list_sort (g_hash_table_get_values (self->entries), compare_entries);
}

static gboolean
save (CdVolumeController *self, GError **error)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (GList) entries = sorted_entries (self);

  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "version");
  json_builder_add_int_value (builder, 1);
  json_builder_set_member_name (builder, "streams");
  json_builder_begin_array (builder);
  for (GList *item = entries; item != NULL; item = item->next)
    {
      RestoreEntry *entry = item->data;

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "selector");
      json_builder_add_string_value (builder, entry->key.selector);
      json_builder_set_member_name (builder, "pulseIndex");
      json_builder_add_int_value (builder, entry->key.pulse_index);
      json_builder_set_member_name (builder, "restoreId");
      json_builder_add_string_value (builder, normalized_restore_id (entry->key.restore_id));
      json_builder_set_member_name (builder, "name");
      json_builder_add_string_value (builder, entry->name != NULL ? entry->name : "");
      json_builder_set_member_name (builder, "channelVolumes");
      json_builder_begin_array (builder);
      for (guint channel = 0; channel < entry->original.channels; channel++)
        json_builder_add_int_value (builder, entry->original.values[channel]);
      json_builder_end_array (builder);
      json_builder_set_member_name (builder, "channelMap");
      json_builder_begin_array (builder);
      for (guint channel = 0; channel < entry->channel_map.channels; channel++)
        json_builder_add_string_value (
            builder, pa_channel_position_to_string (entry->channel_map.map[channel]));
      json_builder_end_array (builder);
      json_builder_end_object (builder);
    }
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  root = json_builder_get_root (builder);
  return cd_state_store_save (self->store, root, error);
}

static gboolean
node_has_type (JsonNode *node, GType type)
{
  return node != NULL && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == type;
}

static JsonNode *
required_member (JsonObject *object, const char *name)
{
  return json_object_has_member (object, name) ? json_object_get_member (object, name) : NULL;
}

static gboolean
parse_entry (JsonNode *node, RestoreEntry **out_entry)
{
  JsonObject *object;
  JsonNode *selector_node;
  JsonNode *index_node;
  JsonNode *restore_node;
  JsonNode *name_node;
  JsonNode *volumes_node;
  JsonNode *map_node;
  JsonArray *volumes;
  JsonArray *map;
  RestoreEntry *entry;
  gint64 pulse_index;
  guint64 used_positions = 0;

  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return FALSE;
  object = json_node_get_object (node);
  selector_node = required_member (object, "selector");
  index_node = required_member (object, "pulseIndex");
  restore_node = required_member (object, "restoreId");
  name_node = required_member (object, "name");
  volumes_node = required_member (object, "channelVolumes");
  map_node = required_member (object, "channelMap");
  if (!node_has_type (selector_node, G_TYPE_STRING) || !node_has_type (index_node, G_TYPE_INT64)
      || !node_has_type (restore_node, G_TYPE_STRING) || !node_has_type (name_node, G_TYPE_STRING)
      || volumes_node == NULL || !JSON_NODE_HOLDS_ARRAY (volumes_node) || map_node == NULL
      || !JSON_NODE_HOLDS_ARRAY (map_node))
    return FALSE;

  pulse_index = json_node_get_int (index_node);
  volumes = json_node_get_array (volumes_node);
  map = json_node_get_array (map_node);
  if (*json_node_get_string (selector_node) == '\0' || pulse_index < 0 || pulse_index > G_MAXUINT32
      || json_array_get_length (volumes) == 0 || json_array_get_length (volumes) > PA_CHANNELS_MAX
      || json_array_get_length (map) != json_array_get_length (volumes))
    return FALSE;

  entry = g_new0 (RestoreEntry, 1);
  entry->key.selector = g_strdup (json_node_get_string (selector_node));
  entry->key.pulse_index = (guint32)pulse_index;
  entry->key.restore_id = g_strdup (json_node_get_string (restore_node));
  entry->name = g_strdup (json_node_get_string (name_node));
  entry->original.channels = (guint8)json_array_get_length (volumes);
  entry->channel_map.channels = entry->original.channels;
  for (guint channel = 0; channel < entry->original.channels; channel++)
    {
      JsonNode *volume_node = json_array_get_element (volumes, channel);
      JsonNode *position_node = json_array_get_element (map, channel);
      gint64 raw;
      pa_channel_position_t position;
      guint64 position_bit;

      if (!node_has_type (volume_node, G_TYPE_INT64)
          || !node_has_type (position_node, G_TYPE_STRING))
        goto invalid;
      raw = json_node_get_int (volume_node);
      position = pa_channel_position_from_string (json_node_get_string (position_node));
      if (raw < PA_VOLUME_MUTED || raw > PA_VOLUME_MAX || position < 0
          || position >= PA_CHANNEL_POSITION_MAX)
        goto invalid;
      position_bit = G_GUINT64_CONSTANT (1) << (guint)position;
      if ((used_positions & position_bit) != 0)
        goto invalid;
      used_positions |= position_bit;
      entry->original.values[channel] = (pa_volume_t)raw;
      entry->channel_map.map[channel] = position;
    }
  if (!pa_cvolume_valid (&entry->original) || !pa_channel_map_valid (&entry->channel_map))
    goto invalid;
  *out_entry = entry;
  return TRUE;

invalid:
  restore_entry_free (entry);
  return FALSE;
}

static gboolean
set_journal_error (CdVolumeController *self, GError *cause, GError **error)
{
  self->journal_blocked = TRUE;
  g_clear_error (&self->journal_error);
  self->journal_error = g_error_copy (cause);
  g_propagate_error (error, cause);
  return FALSE;
}

gboolean
cd_volume_controller_load (CdVolumeController *self, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autoptr (GHashTable) parsed = entries_new ();
  JsonObject *object;
  JsonNode *version_node;
  JsonNode *streams_node;
  JsonArray *streams;

  g_return_val_if_fail (self != NULL, FALSE);
  root = cd_state_store_load (self->store, &local_error);
  if (root == NULL)
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      return set_journal_error (self, g_steal_pointer (&local_error), error);
    }
  if (!JSON_NODE_HOLDS_OBJECT (root))
    goto invalid;
  object = json_node_get_object (root);
  version_node = required_member (object, "version");
  streams_node = required_member (object, "streams");
  if (!node_has_type (version_node, G_TYPE_INT64) || json_node_get_int (version_node) != 1
      || streams_node == NULL || !JSON_NODE_HOLDS_ARRAY (streams_node))
    goto invalid;
  streams = json_node_get_array (streams_node);
  for (guint index = 0; index < json_array_get_length (streams); index++)
    {
      RestoreEntry *entry = NULL;

      if (!parse_entry (json_array_get_element (streams, index), &entry)
          || g_hash_table_contains (parsed, &entry->key))
        {
          restore_entry_free (entry);
          goto invalid;
        }
      g_hash_table_insert (parsed, &entry->key, entry);
    }

  g_hash_table_unref (self->entries);
  self->entries = g_steal_pointer (&parsed);
  self->journal_blocked = FALSE;
  g_clear_error (&self->journal_error);
  return TRUE;

invalid:
  g_set_error_literal (&local_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Unsupported or invalid PulseAudio restore journal");
  return set_journal_error (self, g_steal_pointer (&local_error), error);
}

static gboolean
stream_is_eligible (CdAudioStream *stream)
{
  return stream != NULL && !stream->input && stream->volume_controllable
         && stream->stable_id != NULL && *stream->stable_id != '\0';
}

static gboolean
stream_matches_identity_group (CdAudioStream *stream, const char *selector, const char *restore_id)
{
  return stream != NULL && !stream->input && stream->stable_id != NULL
         && g_str_equal (stream->stable_id, selector)
         && g_str_equal (normalized_restore_id (stream->restore_id),
                         normalized_restore_id (restore_id));
}

static guint
count_entry_group (CdVolumeController *self, const char *selector, const char *restore_id,
                   RestoreEntry **candidate)
{
  GHashTableIter iterator;
  gpointer value;
  guint count = 0;

  *candidate = NULL;
  g_hash_table_iter_init (&iterator, self->entries);
  while (g_hash_table_iter_next (&iterator, NULL, &value))
    {
      RestoreEntry *entry = value;

      if (g_str_equal (entry->key.selector, selector)
          && g_str_equal (normalized_restore_id (entry->key.restore_id),
                          normalized_restore_id (restore_id)))
        {
          *candidate = entry;
          count++;
        }
    }
  return count;
}

static guint
count_stream_group (CdVolumeController *self, const char *selector, const char *restore_id,
                    CdAudioStream **candidate)
{
  GPtrArray *streams = cd_audio_backend_get_streams (self->backend);
  guint count = 0;

  *candidate = NULL;
  for (guint index = 0; streams != NULL && index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);

      if (stream_matches_identity_group (stream, selector, restore_id))
        {
          *candidate = stream;
          count++;
        }
    }
  return count;
}

static ResolveResult
resolve_entry (CdVolumeController *self, RestoreEntry *entry, CdAudioStream **out_stream,
               gboolean *out_remapped)
{
  GPtrArray *streams = cd_audio_backend_get_streams (self->backend);
  CdAudioStream *exact = NULL;
  guint exact_count = 0;

  *out_stream = NULL;
  *out_remapped = FALSE;
  for (guint index = 0; streams != NULL && index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);

      if (stream_matches_identity_group (stream, entry->key.selector, entry->key.restore_id)
          && stream->id == entry->key.pulse_index)
        {
          exact = stream;
          exact_count++;
        }
    }
  if (exact_count == 1 && stream_is_eligible (exact))
    {
      *out_stream = exact;
      return RESOLVE_FOUND;
    }
  if (exact_count > 1)
    return RESOLVE_AMBIGUOUS;
  if (*normalized_restore_id (entry->key.restore_id) == '\0')
    return RESOLVE_MISSING;

  RestoreEntry *entry_candidate = NULL;
  CdAudioStream *stream_candidate = NULL;
  guint entry_count
      = count_entry_group (self, entry->key.selector, entry->key.restore_id, &entry_candidate);
  guint stream_count
      = count_stream_group (self, entry->key.selector, entry->key.restore_id, &stream_candidate);

  if (entry_count == 1 && entry_candidate == entry && stream_count == 1
      && stream_is_eligible (stream_candidate))
    {
      *out_stream = stream_candidate;
      *out_remapped = stream_candidate->id != entry->key.pulse_index;
      return RESOLVE_FOUND;
    }
  return stream_count == 0 ? RESOLVE_MISSING : RESOLVE_AMBIGUOUS;
}

static CdAudioStream *
find_target_stream (CdVolumeController *self, TargetIdentity *target, gboolean *ambiguous)
{
  GPtrArray *streams = cd_audio_backend_get_streams (self->backend);
  CdAudioStream *exact = NULL;
  guint exact_count = 0;

  *ambiguous = FALSE;
  for (guint index = 0; streams != NULL && index < streams->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (streams, index);

      if (stream_matches_identity_group (stream, target->selector, target->restore_id)
          && stream->id == target->pulse_index)
        exact = stream, exact_count++;
    }
  if (exact_count == 1 && stream_is_eligible (exact))
    return exact;
  if (exact_count > 1)
    {
      *ambiguous = TRUE;
      return NULL;
    }
  if (*normalized_restore_id (target->restore_id) != '\0')
    {
      CdAudioStream *candidate = NULL;
      guint count = count_stream_group (self, target->selector, target->restore_id, &candidate);

      if (count == 1 && stream_is_eligible (candidate))
        return candidate;
      *ambiguous = count > 1;
    }
  return NULL;
}

static void
move_entry_index (CdVolumeController *self, RestoreEntry *entry, guint32 pulse_index)
{
  if (entry->key.pulse_index == pulse_index)
    return;
  g_assert_true (g_hash_table_steal (self->entries, &entry->key));
  entry->key.pulse_index = pulse_index;
  g_hash_table_insert (self->entries, &entry->key, entry);
}

static gboolean
request_has_remap (ControllerRequest *request, RestoreEntry *entry)
{
  for (guint index = 0; index < request->remaps->len; index++)
    {
      RemapRecord *record = &g_array_index (request->remaps, RemapRecord, index);

      if (record->entry == entry)
        return TRUE;
    }
  return FALSE;
}

static void
request_remap_entry (CdVolumeController *self, ControllerRequest *request, RestoreEntry *entry,
                     guint32 pulse_index)
{
  if (entry->key.pulse_index == pulse_index)
    return;
  if (!request_has_remap (request, entry))
    {
      RemapRecord record = { .entry = entry, .old_index = entry->key.pulse_index };

      g_array_append_val (request->remaps, record);
    }
  move_entry_index (self, entry, pulse_index);
}

static void
undo_staged_journal (CdVolumeController *self, ControllerRequest *request)
{
  for (guint index = request->remaps->len; index > 0; index--)
    {
      RemapRecord *record = &g_array_index (request->remaps, RemapRecord, index - 1);

      move_entry_index (self, record->entry, record->old_index);
    }
  for (guint index = 0; index < request->new_entries->len; index++)
    {
      RestoreEntry *entry = g_ptr_array_index (request->new_entries, index);

      if (g_hash_table_steal (self->entries, &entry->key))
        restore_entry_free (entry);
    }
  for (guint index = 0; index < request->revived_entries->len; index++)
    {
      RestoreEntry *entry = g_ptr_array_index (request->revived_entries, index);

      if (g_hash_table_steal (self->entries, &entry->key))
        g_hash_table_replace (self->tombstones, &entry->key, entry);
    }
  g_array_set_size (request->remaps, 0);
  g_ptr_array_set_size (request->new_entries, 0);
  g_ptr_array_set_size (request->revived_entries, 0);
}

static RestoreEntry *
entry_for_stream (CdVolumeController *self, ControllerRequest *request, CdAudioStream *stream,
                  gboolean *ambiguous)
{
  RestoreKey exact = {
    .selector = stream->stable_id,
    .pulse_index = stream->id,
    .restore_id = (char *)normalized_restore_id (stream->restore_id),
  };
  RestoreEntry *entry = g_hash_table_lookup (self->entries, &exact);
  RestoreEntry *candidate = NULL;
  CdAudioStream *stream_candidate = NULL;
  guint entry_count;
  guint stream_count;

  *ambiguous = FALSE;
  if (entry != NULL)
    return entry;
  entry = g_hash_table_lookup (self->tombstones, &exact);
  if (entry != NULL)
    {
      g_assert_true (g_hash_table_steal (self->tombstones, &entry->key));
      g_hash_table_insert (self->entries, &entry->key, entry);
      g_ptr_array_add (request->revived_entries, entry);
      return entry;
    }
  if (*normalized_restore_id (stream->restore_id) == '\0')
    return NULL;
  entry_count = count_entry_group (self, stream->stable_id, stream->restore_id, &candidate);
  if (entry_count == 0)
    return NULL;
  stream_count
      = count_stream_group (self, stream->stable_id, stream->restore_id, &stream_candidate);
  if (entry_count == 1 && stream_count == 1 && stream_candidate == stream)
    {
      request_remap_entry (self, request, candidate, stream->id);
      return candidate;
    }

  /* Multiple live streams may legitimately share a restore ID. If every saved
   * member of the group still has its exact live stream, this is a new stream,
   * not a remap. Its Pulse index safely disambiguates it until restart. */
  if (stream_count > entry_count)
    {
      GHashTableIter iterator;
      gpointer value;
      gboolean all_exact = TRUE;

      g_hash_table_iter_init (&iterator, self->entries);
      while (g_hash_table_iter_next (&iterator, NULL, &value))
        {
          RestoreEntry *current = value;
          CdAudioStream *resolved = NULL;
          gboolean remapped = FALSE;

          if (!g_str_equal (current->key.selector, stream->stable_id)
              || !g_str_equal (normalized_restore_id (current->key.restore_id),
                               normalized_restore_id (stream->restore_id)))
            continue;
          if (resolve_entry (self, current, &resolved, &remapped) != RESOLVE_FOUND || remapped)
            {
              all_exact = FALSE;
              break;
            }
        }
      if (all_exact)
        return NULL;
    }
  *ambiguous = TRUE;
  return NULL;
}

static gboolean
valid_stream_layout (CdAudioStream *stream)
{
  return pa_cvolume_valid (&stream->volume) && pa_channel_map_valid (&stream->channel_map)
         && stream->volume.channels == stream->channel_map.channels;
}

static RestoreEntry *
capture_entry (CdVolumeController *self, ControllerRequest *request, CdAudioStream *stream)
{
  RestoreEntry *entry = g_new0 (RestoreEntry, 1);

  entry->key.selector = g_strdup (stream->stable_id);
  entry->key.pulse_index = stream->id;
  entry->key.restore_id = g_strdup (normalized_restore_id (stream->restore_id));
  entry->name = g_strdup (stream->display_name != NULL ? stream->display_name : "");
  entry->original = stream->volume;
  entry->channel_map = stream->channel_map;
  g_hash_table_insert (self->entries, &entry->key, entry);
  g_ptr_array_add (request->new_entries, entry);
  return entry;
}

static void
request_set_error (ControllerRequest *request, GError *error)
{
  if (request->error == NULL)
    request->error = g_error_copy (error);
}

static void
request_set_literal_error (ControllerRequest *request, GQuark domain, gint code,
                           const char *message)
{
  if (request->error == NULL)
    request->error = g_error_new_literal (domain, code, message);
}

static void
queue_operation (ControllerRequest *request, RestoreEntry *entry, gboolean restore, gboolean force)
{
  Operation *operation = g_new0 (Operation, 1);

  operation->entry = entry;
  operation->restore = restore;
  operation->force = force;
  g_queue_push_tail (&request->operations, operation);
}

static void
build_normal_operations (CdVolumeController *self, ControllerRequest *request)
{
  g_autoptr (GList) entries = sorted_entries (self);

  /* Restorations run before new target operations. */
  for (GList *item = entries; item != NULL; item = item->next)
    {
      RestoreEntry *entry = item->data;

      if (!g_hash_table_contains (request->target_entries, entry))
        queue_operation (request, entry, TRUE, FALSE);
    }
  for (GList *item = entries; item != NULL; item = item->next)
    {
      RestoreEntry *entry = item->data;

      if (g_hash_table_contains (request->target_entries, entry))
        queue_operation (request, entry, FALSE, FALSE);
    }
}

static void
build_rollback_operations (CdVolumeController *self, ControllerRequest *request)
{
  g_autoptr (GList) entries = sorted_entries (self);

  request->rolling_back = TRUE;
  g_queue_clear_full (&request->operations, (GDestroyNotify)operation_free);
  for (GList *item = entries; item != NULL; item = item->next)
    {
      RestoreEntry *entry = item->data;

      if (!g_hash_table_contains (request->restored_entries, entry))
        queue_operation (request, entry, TRUE, TRUE);
    }
}

static gboolean
prepare_reconcile (CdVolumeController *self, ControllerRequest *request)
{
  gboolean staged = FALSE;

  if (self->journal_blocked)
    {
      request->error = g_error_copy (self->journal_error);
      return FALSE;
    }
  for (guint index = 0; request->targets != NULL && index < request->targets->len; index++)
    {
      TargetIdentity *target = g_ptr_array_index (request->targets, index);
      gboolean target_ambiguous = FALSE;
      gboolean entry_ambiguous = FALSE;
      CdAudioStream *stream = find_target_stream (self, target, &target_ambiguous);
      RestoreEntry *entry;

      if (target_ambiguous)
        {
          request_set_literal_error (request, G_IO_ERROR, G_IO_ERROR_BUSY,
                                     "Audio stream identity is ambiguous");
          break;
        }
      if (stream == NULL)
        continue;
      if (!valid_stream_layout (stream))
        {
          request_set_literal_error (request, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                     "Audio stream has an invalid channel layout");
          break;
        }
      entry = entry_for_stream (self, request, stream, &entry_ambiguous);
      if (entry_ambiguous)
        {
          request_set_literal_error (request, G_IO_ERROR, G_IO_ERROR_BUSY,
                                     "Restore identity is ambiguous");
          break;
        }
      if (entry == NULL)
        entry = capture_entry (self, request, stream);
      g_hash_table_add (request->target_entries, entry);
    }

  staged = request->new_entries->len > 0 || request->revived_entries->len > 0
           || request->remaps->len > 0;
  if (request->error == NULL && staged)
    {
      g_autoptr (GError) error = NULL;

      if (!save (self, &error))
        request_set_error (request, error);
    }
  if (request->error != NULL)
    {
      if (staged)
        undo_staged_journal (self, request);
      build_rollback_operations (self, request);
      return request->operations.length > 0;
    }
  g_ptr_array_set_size (request->new_entries, 0);
  g_ptr_array_set_size (request->revived_entries, 0);
  g_array_set_size (request->remaps, 0);
  build_normal_operations (self, request);
  return TRUE;
}

static gboolean
prepare_restore (CdVolumeController *self, ControllerRequest *request)
{
  if (self->journal_blocked)
    {
      request->error = g_error_copy (self->journal_error);
      return FALSE;
    }
  build_rollback_operations (self, request);
  return TRUE;
}

static ControllerRequest *
request_new (CdVolumeController *self, RequestKind kind, GCancellable *cancellable,
             GAsyncReadyCallback callback, gpointer data)
{
  ControllerRequest *request = g_new0 (ControllerRequest, 1);

  request->kind = kind;
  request->generation = ++self->generation;
  request->task = g_task_new (NULL, cancellable, callback, data);
  request->target_entries = g_hash_table_new (g_direct_hash, g_direct_equal);
  request->restored_entries = g_hash_table_new (g_direct_hash, g_direct_equal);
  request->new_entries = g_ptr_array_new ();
  request->revived_entries = g_ptr_array_new ();
  request->remaps = g_array_new (FALSE, FALSE, sizeof (RemapRecord));
  return request;
}

static gboolean
target_identity_equal (TargetIdentity *target, CdAudioStream *stream)
{
  return target->pulse_index == stream->id && g_str_equal (target->selector, stream->stable_id)
         && g_str_equal (normalized_restore_id (target->restore_id),
                         normalized_restore_id (stream->restore_id));
}

static void
copy_targets (ControllerRequest *request, GPtrArray *targets)
{
  request->targets = g_ptr_array_new_with_free_func ((GDestroyNotify)target_identity_free);
  for (guint index = 0; targets != NULL && index < targets->len; index++)
    {
      CdAudioStream *stream = g_ptr_array_index (targets, index);
      TargetIdentity *target;
      gboolean duplicate = FALSE;

      if (!stream_is_eligible (stream))
        continue;
      for (guint existing = 0; existing < request->targets->len; existing++)
        if (target_identity_equal (g_ptr_array_index (request->targets, existing), stream))
          {
            duplicate = TRUE;
            break;
          }
      if (duplicate)
        continue;
      target = g_new0 (TargetIdentity, 1);
      target->pulse_index = stream->id;
      target->selector = g_strdup (stream->stable_id);
      target->restore_id = g_strdup (normalized_restore_id (stream->restore_id));
      g_ptr_array_add (request->targets, target);
    }
}

static void
supersede_pending (ControllerRequest **slot)
{
  if (*slot == NULL)
    return;
  return_superseded (*slot, "Volume request was superseded");
  g_clear_pointer (slot, request_free);
}

static void
supersede_current_reconcile (CdVolumeController *self)
{
  if (self->current == NULL || self->current->kind != REQUEST_RECONCILE
      || self->current->rolling_back)
    return;
  return_superseded (self->current, "Volume request was superseded");
  g_queue_clear_full (&self->current->operations, (GDestroyNotify)operation_free);
  if (!self->set_active)
    {
      g_clear_pointer (&self->current, request_free);
      start_next_request (self);
    }
}

void
cd_volume_controller_reconcile_async (CdVolumeController *self, GPtrArray *targets, double target,
                                      GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer data)
{
  ControllerRequest *request;

  g_return_if_fail (self != NULL);
  request = request_new (self, REQUEST_RECONCILE, cancellable, callback, data);
  g_task_set_source_tag (request->task, cd_volume_controller_reconcile_async);
  request->target = CLAMP (target, 0.0, 1.0);
  copy_targets (request, targets);
  supersede_pending (&self->pending_reconcile);
  self->pending_reconcile = request;
  supersede_current_reconcile (self);
  start_next_request (self);
}

gboolean
cd_volume_controller_reconcile_finish (CdVolumeController *self, GAsyncResult *result,
                                       GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
  g_return_val_if_fail (
      g_task_get_source_tag (G_TASK (result)) == cd_volume_controller_reconcile_async, FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cd_volume_controller_restore_all_async (CdVolumeController *self, GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer data)
{
  ControllerRequest *request;

  g_return_if_fail (self != NULL);
  request = request_new (self, REQUEST_RESTORE, cancellable, callback, data);
  g_task_set_source_tag (request->task, cd_volume_controller_restore_all_async);
  supersede_pending (&self->pending_reconcile);
  supersede_pending (&self->pending_restore);
  self->pending_restore = request;
  supersede_current_reconcile (self);
  start_next_request (self);
}

gboolean
cd_volume_controller_restore_all_finish (CdVolumeController *self, GAsyncResult *result,
                                         GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);
  g_return_val_if_fail (
      g_task_get_source_tag (G_TASK (result)) == cd_volume_controller_restore_all_async, FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

static gboolean
remap_volume (RestoreEntry *entry, CdAudioStream *stream, pa_cvolume *volume, GError **error)
{
  *volume = entry->original;
  if (!valid_stream_layout (stream) || !pa_cvolume_valid (volume)
      || !pa_channel_map_valid (&entry->channel_map)
      || volume->channels != entry->channel_map.channels
      || pa_cvolume_remap (volume, &entry->channel_map, &stream->channel_map) == NULL
      || !pa_cvolume_valid (volume) || volume->channels != stream->channel_map.channels)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Could not remap the saved channel volume");
      return FALSE;
    }
  return TRUE;
}

static gboolean
persist_dispatch_remap (CdVolumeController *self, RestoreEntry *entry, guint32 pulse_index,
                        GError **error)
{
  guint32 old_index = entry->key.pulse_index;

  if (old_index == pulse_index)
    return TRUE;
  move_entry_index (self, entry, pulse_index);
  if (save (self, error))
    return TRUE;
  move_entry_index (self, entry, old_index);
  return FALSE;
}

static void
begin_rollback (CdVolumeController *self, ControllerRequest *request, GError *error)
{
  request_set_error (request, error);
  if (request->kind == REQUEST_RECONCILE && !request->rolling_back)
    build_rollback_operations (self, request);
}

static void
handle_operation_failure (CdVolumeController *self, ControllerRequest *request, GError *error)
{
  if (request->kind == REQUEST_RECONCILE && !request->rolling_back)
    begin_rollback (self, request, error);
  else
    request_set_error (request, error);
}

static void
set_call_free (SetCall *call)
{
  operation_free (call->operation);
  g_free (call);
}

static void
set_done (GObject *object, GAsyncResult *result, gpointer data)
{
  SetCall *call = data;
  CdVolumeController *self = call->controller;
  ControllerRequest *request = call->request;
  g_autoptr (GError) error = NULL;
  gboolean succeeded
      = cd_audio_backend_set_volume_finish (CD_AUDIO_BACKEND (object), result, &error);

  self->set_active = FALSE;
  if (self->current != request || request->superseded || self->disposed)
    {
      set_call_free (call);
      if (self->current == request)
        g_clear_pointer (&self->current, request_free);
      if (!self->disposed)
        start_next_request (self);
      controller_unref (self);
      return;
    }

  if (succeeded)
    {
      RestoreEntry *entry = call->operation->entry;

      entry->last_set_valid = TRUE;
      entry->last_set_index = call->index;
      entry->last_set_epoch = self->snapshot_epoch;
      entry->last_set = call->volume;
      if (call->operation->restore)
        g_hash_table_add (request->restored_entries, entry);
    }
  else
    handle_operation_failure (self, request, error);
  set_call_free (call);
  process_current (self);
  controller_unref (self);
}

static gboolean
effective_volume_equal (CdVolumeController *self, RestoreEntry *entry, CdAudioStream *stream,
                        const pa_cvolume *desired)
{
  if (entry->last_set_valid && entry->last_set_index == stream->id)
    {
      if (self->snapshot_epoch > entry->last_set_epoch)
        entry->last_set_valid = FALSE;
      else
        return pa_cvolume_equal (&entry->last_set, desired);
    }
  return pa_cvolume_equal (&stream->volume, desired);
}

static gboolean
finalize_restored_entries (CdVolumeController *self, ControllerRequest *request, GError **error)
{
  GHashTableIter iterator;
  gpointer entry_pointer;
  g_autoptr (GPtrArray) removed = g_ptr_array_new ();

  g_hash_table_iter_init (&iterator, request->restored_entries);
  while (g_hash_table_iter_next (&iterator, &entry_pointer, NULL))
    {
      RestoreEntry *entry = entry_pointer;

      if (g_hash_table_steal (self->entries, &entry->key))
        g_ptr_array_add (removed, entry);
    }
  if (removed->len == 0)
    return TRUE;
  if (!save (self, error))
    {
      for (guint index = 0; index < removed->len; index++)
        {
          RestoreEntry *entry = g_ptr_array_index (removed, index);

          g_hash_table_insert (self->entries, &entry->key, entry);
        }
      return FALSE;
    }
  for (guint index = 0; index < removed->len; index++)
    {
      RestoreEntry *entry = g_ptr_array_index (removed, index);

      g_hash_table_replace (self->tombstones, &entry->key, entry);
    }
  return TRUE;
}

static void
notify_changed (CdVolumeController *self, const GError *error)
{
  CdVolumeControllerChanged changed = self->changed;
  gpointer data = self->changed_data;

  if (self->disposed || changed == NULL)
    return;
  controller_ref (self);
  changed (self, error, data);
  controller_unref (self);
}

static void
complete_current (CdVolumeController *self)
{
  ControllerRequest *request = self->current;

  g_assert_nonnull (request);
  g_assert_false (self->set_active);
  controller_ref (self);
  if (!request->superseded && !self->disposed)
    {
      g_autoptr (GError) save_error = NULL;

      if (!finalize_restored_entries (self, request, &save_error))
        request_set_error (request, save_error);
      /* Detach before invoking user code: a changed callback may dispose the
       * controller, but this local request remains valid until we return. */
      self->current = NULL;
      notify_changed (self, request->error);
      if (!request->task_returned)
        {
          request->task_returned = TRUE;
          if (request->error != NULL)
            g_task_return_error (request->task, g_error_copy (request->error));
          else
            g_task_return_boolean (request->task, TRUE);
        }
    }
  else
    self->current = NULL;
  request_free (request);
  start_next_request (self);
  controller_unref (self);
}

static void
process_current (CdVolumeController *self)
{
  ControllerRequest *request = self->current;

  if (request == NULL || self->set_active || self->disposed)
    return;
  if (request->superseded)
    {
      complete_current (self);
      return;
    }
  while (!g_queue_is_empty (&request->operations))
    {
      Operation *operation = g_queue_pop_head (&request->operations);
      RestoreEntry *entry = operation->entry;
      CdAudioStream *stream = NULL;
      gboolean remapped = FALSE;
      ResolveResult resolved = resolve_entry (self, entry, &stream, &remapped);
      pa_cvolume desired;
      g_autoptr (GError) error = NULL;

      if (resolved != RESOLVE_FOUND)
        {
          operation_free (operation);
          continue;
        }
      if (remapped && !persist_dispatch_remap (self, entry, stream->id, &error))
        {
          handle_operation_failure (self, request, error);
          operation_free (operation);
          continue;
        }
      if (!remap_volume (entry, stream, &desired, &error))
        {
          handle_operation_failure (self, request, error);
          operation_free (operation);
          continue;
        }
      if (!operation->restore)
        desired = cd_volume_absolute (&desired, request->target);
      if (!operation->force && effective_volume_equal (self, entry, stream, &desired))
        {
          if (operation->restore)
            g_hash_table_add (request->restored_entries, entry);
          operation_free (operation);
          continue;
        }

      SetCall *call = g_new0 (SetCall, 1);
      call->controller = controller_ref (self);
      call->request = request;
      call->operation = operation;
      call->index = stream->id;
      call->volume = desired;
      self->set_active = TRUE;
      cd_audio_backend_set_volume_async (
          self->backend, stream->id, stream->stable_id, normalized_restore_id (stream->restore_id),
          &stream->channel_map, &desired,
          request->rolling_back ? NULL : g_task_get_cancellable (request->task), set_done, call);
      return;
    }
  complete_current (self);
}

static void
start_next_request (CdVolumeController *self)
{
  gboolean prepared;

  if (self->disposed || self->current != NULL || self->set_active)
    return;
  if (self->pending_restore != NULL)
    {
      self->current = self->pending_restore;
      self->pending_restore = NULL;
    }
  else if (self->pending_reconcile != NULL)
    {
      self->current = self->pending_reconcile;
      self->pending_reconcile = NULL;
    }
  else
    return;

  prepared = self->current->kind == REQUEST_RECONCILE ? prepare_reconcile (self, self->current)
                                                      : prepare_restore (self, self->current);
  if (!prepared && g_queue_is_empty (&self->current->operations))
    complete_current (self);
  else
    process_current (self);
}

guint
cd_volume_controller_get_pending (CdVolumeController *self)
{
  g_return_val_if_fail (self != NULL, 0);
  return g_hash_table_size (self->entries);
}
