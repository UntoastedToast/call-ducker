#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  CD_DAEMON_ERROR_PREVIEW_UNAVAILABLE,
  CD_DAEMON_ERROR_REQUEST_SUPERSEDED,
} CdDaemonError;

static inline GQuark
cd_daemon_error_quark (void)
{
  return g_quark_from_static_string ("call-ducker-daemon-error");
}

#define CD_DAEMON_ERROR (cd_daemon_error_quark ())

G_END_DECLS
