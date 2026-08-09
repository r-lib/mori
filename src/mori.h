#ifndef MORI_H
#define MORI_H

#include <Rversion.h>
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Identifier grammar constants ------------------------------------------------

#define MORI_NAME_MAX        30                /* size of mori_shm.name; fits Windows worst case (Local\\mori_<8hex>_<8hex> = 28) + NUL with 1 byte slack */
#define MORI_MAX_PATH        64                /* max indices in a path */
#define MORI_IDENTIFIER_MAX  1024              /* parser input length cap */
#define MORI_FORMAT_BUFLEN   1024              /* formatter stack buffer */

#ifdef _WIN32
#define MORI_PREFIX_LITERAL  "Local\\mori_"
#else
#define MORI_PREFIX_LITERAL  "/mori_"
#endif

// Types -----------------------------------------------------------------------

typedef struct mori_shm_s {
  void *addr;
  size_t size;
  char name[MORI_NAME_MAX];
  uint8_t name_len;                /* strlen(name); fits since MORI_NAME_MAX < 256 */
  unsigned int pid;                /* creator PID: fork guard for the host finalizer (POSIX; unused on Windows) */
#ifdef _WIN32
  void *handle;
#endif
} mori_shm;

typedef struct mori_buf_s {
  unsigned char *buf;
  size_t len;
  size_t cur;
} mori_buf;

typedef struct mori_vec_s {
  const void *data;
  R_xlen_t length;
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} mori_vec;

typedef struct mori_list_view_s {
  unsigned char *base;       /* points to child MORL start */
  int64_t region_size;       /* bounds all reads within this region */
  int32_t n_elements;
  int32_t index;             /* -1 = root, >= 0 = sub-list */
} mori_list_view;

/* Outcome of an SHM creation attempt: MORI_OK on success, otherwise a
   portable failure category. The platform layer classifies errno /
   GetLastError into one of these; mori_err_describe supplies the user-facing
   summary and remediation hint. */
enum {
  MORI_OK = 0,
  MORI_ENOSPC,   /* ENOSPC / ERROR_DISK_FULL */
  MORI_ENOMEM,   /* ENOMEM / commit limit exceeded */
  MORI_EEXIST,   /* region name already in use (orphan of a reused PID) */
  MORI_EOTHER
};

// shm.c -----------------------------------------------------------------------

int mori_shm_create(mori_shm *shm, size_t size);
int mori_shm_open(mori_shm *shm, const char *name);
void mori_shm_close(mori_shm *shm, int unlink);
int mori_shm_create_heap(mori_shm **out, size_t size);
mori_shm *mori_shm_open_heap(const char *name);
void mori_shm_finalizer(SEXP ptr);
void mori_host_finalizer(SEXP ptr);
char **mori_shm_reap(int *n);
void mori_err_describe(int category, const char **summary, const char **hint);

// serialize.c -----------------------------------------------------------------

size_t mori_serialize_count(SEXP object);
void mori_serialize_into(unsigned char *dst, size_t size, SEXP object);
SEXP mori_unserialize_from(unsigned char *src, size_t size);

static inline size_t mori_sizeof_elt(int type) {
  switch (type) {
  case REALSXP:  return sizeof(double);
  case INTSXP:   return sizeof(int);
  case LGLSXP:   return sizeof(int);
  case RAWSXP:   return 1;
  case CPLXSXP:  return sizeof(Rcomplex);
  default:       return 0;
  }
}

// altrep.c --------------------------------------------------------------------

void mori_altrep_init(DllInfo *dll);

// Alignment macro -------------------------------------------------------------

#define MORI_ALIGN64(x) (((x) + 63) & ~(size_t)63)

#endif /* MORI_H */
