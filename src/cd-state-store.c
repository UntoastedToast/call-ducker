#include "cd-state-store.h"

#include <errno.h>
#include <glib/gstdio.h>

struct _CdStateStore
{
  char *path;
};

CdStateStore *
cd_state_store_new (const char *filename)
{
  CdStateStore *self = g_new0 (CdStateStore, 1);

  self->path = g_build_filename (g_get_user_state_dir (), "call-ducker", filename, NULL);
  return self;
}

CdStateStore *
cd_state_store_new_for_path (const char *path)
{
  CdStateStore *self = g_new0 (CdStateStore, 1);

  self->path = g_strdup (path);
  return self;
}

void
cd_state_store_free (CdStateStore *self)
{
  if (self == NULL)
    return;
  g_free (self->path);
  g_free (self);
}

JsonNode *
cd_state_store_load (CdStateStore *self, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!json_parser_load_from_file (parser, self->path, error))
    return NULL;
  return json_node_copy (json_parser_get_root (parser));
}

gboolean
cd_state_store_save (CdStateStore *self, JsonNode *root, GError **error)
{
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autofree char *directory = g_path_get_dirname (self->path);
  g_autofree char *contents = NULL;
  gsize length;

  if (g_mkdir_with_parents (directory, 0700) < 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not create state directory: %s", g_strerror (saved_errno));
      return FALSE;
    }
  json_generator_set_root (generator, root);
  contents = json_generator_to_data (generator, &length);
  return g_file_set_contents_full (self->path, contents, (gssize)length,
                                   G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                   0600, error);
}
