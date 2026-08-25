#ifndef MORI_H
#define MORI_H

#include <Rversion.h>
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <string.h>

#include "mori_region.h"

// Identifier grammar constants ------------------------------------------------

#define MORI_MAX_PATH        64                /* max indices in a path */
#define MORI_IDENTIFIER_MAX  1024              /* parser input length cap */
#define MORI_FORMAT_BUFLEN   1024              /* formatter stack buffer */

/* External-pointer tag strings (installed once at init). */
#define MORI_TAG_SHM   "mori_shm"
#define MORI_TAG_HOST  "mori_host"
#define MORI_TAG_OWNED "mori_owned"

/* Region header flags word at byte offset 32 of the 64-byte header —
   bytes [24-31] remain embedder cross-process state, [36-63] reserved.
   Bit 0 records the S4 object bit, which the layouts otherwise cannot
   carry. */
#define MORI_FLAGS_OFF 32
#define MORI_FLAG_S4 0x1u

// Types -----------------------------------------------------------------------

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

/* R's S4 data-part wrapper is an ALTREP that forwards every data method
   to the wrapped vector (data1): directly readable, no compression, no
   keeper chain. Recognize it by that shape — a non-ALTREP data1 of the
   same type whose data pointer the wrapper shares. Anything else foreign
   (a lazy ALTREP, a materialized compact sequence, an extptr-data1 view)
   is rejected: the layout write would materialize it or forfeit its
   compact wire form. */
static inline int mori_altrep_readable(SEXP x) {
  SEXP inner = R_altrep_data1(x);
  if (ALTREP(inner) || TYPEOF(inner) != TYPEOF(x)) return 0;
  const void *pi = DATAPTR_OR_NULL(inner);
  return pi != NULL && DATAPTR_OR_NULL(x) == pi;
}

/* Apply a region header's S4 flag to a freshly wrapped view — after
   attributes land, so a read never consults a class definition
   (Rf_asS4 with complete = 0 sets the bit in place on a fresh object).
   Call on a validated region (>= MORI_HEADER_SIZE bytes); embedders
   wrapping MORH / MORS roots through the raw constructors call this
   last. */
static inline SEXP mori_apply_s4(SEXP x, const unsigned char *base) {
  uint32_t flags;
  memcpy(&flags, base + MORI_FLAGS_OFF, 4);
  return (flags & MORI_FLAG_S4) ? Rf_asS4(x, TRUE, 0) : x;
}

// altrep.c --------------------------------------------------------------------

void mori_altrep_init(DllInfo *dll);

/* SHM extptr finalizers, defined alongside the wrap constructors that
   register them: mori_shm_finalizer releases this side's mapping only;
   mori_host_finalizer releases the SHM name/handle via
   mori_shm_host_release. */
void mori_shm_finalizer(SEXP ptr);
void mori_host_finalizer(SEXP ptr);

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
   take: an ALTREP node is rejected unless it is a mori view (rides the
   wire hooks) or mori_altrep_readable (R's S4 data-part wrappers
   qualify; a compact 1:1e8 would materialize through DATAPTR_RO at
   write). The write emits exactly mori_layout_size bytes and zeroes
   header reserved bytes. */
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
