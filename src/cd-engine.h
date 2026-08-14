#pragma once
#include <glib.h>

typedef enum
{
  CD_STATE_DISABLED,
  CD_STATE_LISTENING,
  CD_STATE_CALL_ACTIVE,
  CD_STATE_UNAVAILABLE
} CdState;
typedef struct _CdEngine CdEngine;
typedef void (*CdEngineChanged) (CdEngine *, gpointer);

CdEngine *cd_engine_new (CdEngineChanged changed, gpointer data);
void cd_engine_free (CdEngine *self);
void cd_engine_set_enabled (CdEngine *self, gboolean enabled);
void cd_engine_set_available (CdEngine *self, gboolean available, const char *error);
void cd_engine_set_notice (CdEngine *self, const char *message);
void cd_engine_set_trigger (CdEngine *self, const char *id, const char *name, gboolean active);
void cd_engine_end_call (CdEngine *self);
void cd_engine_set_ducked (CdEngine *self, const char *id, const char *name, gboolean ducked);
void cd_engine_clear_ducked (CdEngine *self);
void cd_engine_pause_for_call (CdEngine *self);
void cd_engine_set_pending (CdEngine *self, guint count);
CdState cd_engine_get_state (CdEngine *self);
const char *cd_state_name (CdState state);
char **cd_engine_dup_trigger_names (CdEngine *self);
char **cd_engine_dup_ducked_names (CdEngine *self);
const char *cd_engine_get_error (CdEngine *self);
guint cd_engine_get_pending (CdEngine *self);
gboolean cd_engine_should_duck (CdEngine *self);
