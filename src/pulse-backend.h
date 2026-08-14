#pragma once

#include "cd-audio-backend.h"

G_BEGIN_DECLS

#define CD_TYPE_PULSE_BACKEND (cd_pulse_backend_get_type ())
G_DECLARE_FINAL_TYPE (CdPulseBackend, cd_pulse_backend, CD, PULSE_BACKEND, GObject)

CdPulseBackend *cd_pulse_backend_new (void);
void cd_pulse_backend_start (CdPulseBackend *self);

G_END_DECLS
