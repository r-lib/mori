/* The thread-local error slot: handle-free entry points (region
   create/open, failed channel/pool create/attach) record a portable
   category plus formatted message here, read back via
   mori_last_error_category / mori_last_error_message. Depends only on
   mori_region.h so the region layer vendors as a unit (shm.c + err_tls.c). */

#include "mori_region.h"

#include <stdarg.h>
#include <stdio.h>

/* Local copy of the internal header's cold annotation (this TU vendors with the
   region layer and cannot include the internal header). */
#if defined(_MSC_VER)
#  define MORI_COLD
#else
#  define MORI_COLD __attribute__((cold))
#endif

static _Thread_local mori_errcat tls_cat = MORI_ERRCAT_NONE;
static _Thread_local char tls_msg[256];

MORI_COLD void mori_err_record_tls(mori_errcat cat, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tls_msg, sizeof(tls_msg), fmt, ap);
  va_end(ap);
  tls_cat = cat;
}

mori_errcat mori_last_error_category(void) {
  return tls_cat;
}

const char *mori_last_error_message(void) {
  return tls_msg;
}
