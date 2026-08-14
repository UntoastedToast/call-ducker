#pragma once

#include "cd-audio-backend.h"
#include "cd-state-store.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _CdApplicationRegistry CdApplicationRegistry;

CdApplicationRegistry *cd_application_registry_new (GSettings *settings, CdAudioBackend *backend,
                                                    CdStateStore *store);
void cd_application_registry_free (CdApplicationRegistry *self);

gboolean cd_application_registry_load (CdApplicationRegistry *self, GError **error);
gboolean cd_application_registry_refresh (CdApplicationRegistry *self, gboolean *changed,
                                          GError **error);
GVariant *cd_application_registry_dup_applications (CdApplicationRegistry *self);

G_END_DECLS
