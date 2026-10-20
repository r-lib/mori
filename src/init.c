#include <R_ext/Rdynload.h>
#include "mori.h"

SEXP mori_create(SEXP);
SEXP mori_shm_open_and_wrap(SEXP);
SEXP mori_is_shared(SEXP);
SEXP mori_shm_name(SEXP);
SEXP mori_prune(void);

static const R_CallMethodDef CallEntries[] = {
  {"mori_create",             (DL_FUNC) &mori_create,             1},
  {"mori_shm_open_and_wrap",  (DL_FUNC) &mori_shm_open_and_wrap,  1},
  {"mori_is_shared",          (DL_FUNC) &mori_is_shared,          1},
  {"mori_shm_name",           (DL_FUNC) &mori_shm_name,           1},
  {"mori_prune",              (DL_FUNC) &mori_prune,              0},
  {NULL, NULL, 0}
};

/* The consumer-mapping cache's wraps vector: the vendored view layer
   borrows it, so mori roots it for the session. */
static SEXP cache_wraps;

void R_init_mori(DllInfo *dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  cache_wraps = Rf_allocVector(VECSXP, MORI_CACHE_MAX);
  R_PreserveObject(cache_wraps);
  mori_altrep_init(dll, cache_wraps);
}
