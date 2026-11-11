#include "rh-err.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

RhErr rh_ok(void)
{
  RhErr e = RH_ERR_INIT;
  return e;
}

RhErr rh_fail(RhErrCode code, const char *fmt, ...)
{
  RhErr e;
  va_list ap;

  e = rh_ok();
  e.code = code;
  if (fmt) {
    va_start(ap, fmt);
    g_vsnprintf(e.msg, sizeof e.msg, fmt, ap);
    va_end(ap);
  }
  return e;
}

RhErr rh_err_from(RhErrCode code)
{
  return rh_fail(code, "%s", rh_err_code_name(code));
}

gboolean rh_err_ok(const RhErr *e)
{
  return e == NULL || e->code == RH_OK;
}

const char *rh_err_code_name(RhErrCode code)
{
  switch (code) {
  case RH_OK:              return "ok";
  case RH_ERR_IO:          return "I/O error";
  case RH_ERR_PARSE:       return "parse error";
  case RH_ERR_DB:          return "database error";
  case RH_ERR_DB_BUSY:     return "database is locked by another instance";
  case RH_ERR_SCHEMA:      return "database schema version is not supported";
  case RH_ERR_CONFIG:      return "configuration error";
  case RH_ERR_NOT_FOUND:   return "not found";
  case RH_ERR_EXISTS:      return "already exists";
  case RH_ERR_CONFLICT:    return "conflict";
  case RH_ERR_UNSUPPORTED: return "unsupported";
  case RH_ERR_CANCELLED:   return "cancelled";
  case RH_ERR_INTERNAL:    return "internal error";
  default:                 return "unknown error";
  }
}

gboolean rh_err_is_transient(RhErrCode code)
{
  return code == RH_ERR_DB_BUSY;
}

void rh_err_clear(RhErr *e)
{
  if (!e)
    return;
  e->code = RH_OK;
  e->msg[0] = '\0';
}

void rh_err_set(RhErr *e, RhErrCode code, const char *fmt, ...)
{
  va_list ap;

  if (!e)
    return;
  e->code = code;
  if (fmt) {
    va_start(ap, fmt);
    g_vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
  } else {
    g_strlcpy(e->msg, rh_err_code_name(code), sizeof e->msg);
  }
}

void rh_err_set_errno(RhErr *e, RhErrCode code, const char *fmt, ...)
{
  va_list ap;
  gchar head[RH_ERR_MSG_MAX];

  if (!e)
    return;
  if (fmt) {
    va_start(ap, fmt);
    g_vsnprintf(head, sizeof head, fmt, ap);
    va_end(ap);
  } else {
    g_strlcpy(head, rh_err_code_name(code), sizeof head);
  }
  e->code = code;
  g_snprintf(e->msg, sizeof e->msg, "%s: %s", head, g_strerror(errno));
}
