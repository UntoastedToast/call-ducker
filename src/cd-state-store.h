#pragma once

#include <json-glib/json-glib.h>

typedef struct _CdStateStore CdStateStore;

CdStateStore *cd_state_store_new (const char *filename);
CdStateStore *cd_state_store_new_for_path (const char *path);
void cd_state_store_free (CdStateStore *self);
JsonNode *cd_state_store_load (CdStateStore *self, GError **error);
gboolean cd_state_store_save (CdStateStore *self, JsonNode *root, GError **error);
