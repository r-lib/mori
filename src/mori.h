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

// Region layout constants -----------------------------------------------------

/* Region magics (first 4 bytes): atomic vector, string vector, list tree. */
#define MORI_MAGIC_VEC   0x4D4F5248u  /* "MORH" */
#define MORI_MAGIC_STR   0x4D4F5253u  /* "MORS" */
#define MORI_MAGIC_LIST  0x4D4F524Cu  /* "MORL" */

/* Every region layout opens with a 64-byte header. Bytes [24-63] are
   reserved (written zero): embedders keep cross-process state there. */
#define MORI_HEADER_SIZE 64

/* External-pointer tag strings (installed once at init). */
#define MORI_TAG_SHM   "mori_shm"
#define MORI_TAG_HOST  "mori_host"
#define MORI_TAG_OWNED "mori_owned"

// Types -----------------------------------------------------------------------

typedef struct mori_shm_s {
  void *addr;
  size_t size;
  char name[MORI_NAME_MAX];
  uint8_t name_len;                /* strlen(name); fits since MORI_NAME_MAX < 256 */
  unsigned int pid;                /* creator PID: fork guard for the host finalizer (read on POSIX only; Windows has no fork) */
#ifdef _WIN32
  void *handle;
#endif
} mori_shm;

typedef struct mori_buf_s {
  unsigned char *buf;
  size_t len;
  size_t cur;
} mori_buf;

/* Embedder release callback, fired exactly once per view — at COW
   materialization or at the view finalizer, whichever comes first. Embedded
   as the first member of every owned-metadata struct below, so the shared
   finalizer recovers it from any extptr addr (a struct pointer, suitably
   converted, points to its initial member). ALTLIST views fire at the
   finalizer only: extracted element views keep referencing the region, so a
   list is never fully detached before the whole view tree is finalized. */
typedef void (*mori_release_fn)(void *);

typedef struct mori_owned_s {
  mori_release_fn release;
  void *release_arg;
} mori_owned;

typedef struct mori_vec_s {
  mori_owned owned;
  const void *data;
  R_xlen_t length;
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} mori_vec;

typedef struct mori_list_view_s {
  mori_owned owned;
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
size_t mori_serialize_into(unsigned char *dst, SEXP object);
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

/* Wrap constructors: the returned view's data1 pins `keeper` through its
   protected slot; release/release_arg ride the owned metadata and fire
   once (materialize or finalizer). Internal callers pass NULL, NULL. */
SEXP mori_vec_wrap(const void *data, R_xlen_t length, int sexptype,
                   SEXP keeper, mori_release_fn release, void *release_arg);
SEXP mori_str_wrap(const unsigned char *region_base, R_xlen_t n,
                   int64_t data_size, SEXP keeper,
                   mori_release_fn release, void *release_arg);
SEXP mori_list_wrap(unsigned char *base, int64_t region_size, int32_t index,
                    SEXP keeper, mori_release_fn release, void *release_arg);
void mori_restore_attrs(SEXP result, unsigned char *buf, size_t size);

/* Layout oracle and writer for embedder-managed regions: the size pass
   walks the tree and returns 0 for anything the layout writer must not
   take (a non-mori ALTREP node would materialize through DATAPTR_RO; S4
   bits do not survive the layouts). The write emits exactly
   mori_layout_size bytes and zeroes header reserved bytes. */
size_t mori_layout_size(SEXP x);
void mori_layout_write(unsigned char *base, SEXP x);

/* View introspection: C-level is_shared, the identifier formatter, the
   identifier parser, and a path walk over an already-open region (keeper
   flows to the returned view's chain). */
int mori_view_check(SEXP x);
SEXP mori_shm_name(SEXP x);
int mori_parse_id(const char *s, char *name_out, size_t name_out_size,
                  int32_t *path_out, int *path_len);
SEXP mori_walk_path(unsigned char *base, int64_t region_size,
                    const int32_t *path, int path_len, SEXP keeper);

/* Embedder wire hooks (optional; set once at embedder load): `emit` fires
   from the Serialized_state methods when an identifier — not a
   materialization — is emitted for a view (the holder set then widens
   beyond the direct peer); `resolve` fires from the identifier resolve
   paths after the wrap, with the freshly opened consumer mapping. An
   embedder running a cross-process lifetime protocol uses the pair to
   flag regions on escape and to count remote references on arrival. */
typedef void (*mori_emit_hook_fn)(SEXP view);
typedef void (*mori_resolve_hook_fn)(SEXP view, mori_shm *shm);
void mori_set_wire_hooks(mori_emit_hook_fn emit, mori_resolve_hook_fn resolve);

// Alignment macro -------------------------------------------------------------

#define MORI_ALIGN64(x) (((x) + 63) & ~(size_t)63)

#endif /* MORI_H */
