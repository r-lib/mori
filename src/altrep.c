#include <limits.h>
#include <stdlib.h>
#include "mori.h"

/* ANY_ATTRIB() joined the C API in R 4.5.0; equivalent fallback for
   earlier R, where ATTRIB() was still the sanctioned spelling. */
#if R_VERSION < R_Version(4, 5, 0) && !defined(ANY_ATTRIB)
#define ANY_ATTRIB(x) (ATTRIB(x) != R_NilValue)
#endif

// Global ALTREP class handles and sentinel ------------------------------------

static R_altrep_class_t mori_list_class;
static R_altrep_class_t mori_real_class;
static R_altrep_class_t mori_integer_class;
static R_altrep_class_t mori_logical_class;
static R_altrep_class_t mori_raw_class;
static R_altrep_class_t mori_complex_class;
static R_altrep_class_t mori_string_class;
static SEXP mori_shm_tag;    /* tag on SHM mapping extptrs (addr is mori_shm *) */
static SEXP mori_host_tag;   /* tag on host-only unlink extptrs (addr is mori_shm *) */
static SEXP mori_owned_tag;  /* tag on every view ALTREP data1 extptr; addr type dispatched via TYPEOF(x) */
SEXP mori_int64_class;       /* STRSXP(1) "integer64" — preserved at init */

/* Embedder wire hooks (mori.h): set once at embedder load, read on the
   serialize / unserialize paths. */
static mori_emit_hook_fn mori_emit_hook;
static mori_resolve_hook_fn mori_resolve_hook;

/* Embedder open hook (mori.h): the consumer-mapping cache's miss branch. */
static mori_open_hook_fn mori_open_hook;

void mori_set_wire_hooks(mori_emit_hook_fn emit, mori_resolve_hook_fn resolve) {
  mori_emit_hook = emit;
  mori_resolve_hook = resolve;
}

void mori_set_open_hook(mori_open_hook_fn hook) {
  mori_open_hook = hook;
}

/* Embedder attribute-blob hooks (mori.h): set once at embedder load as a
   triple, consulted at every attribute-blob write and read. */
static mori_attrs_size_fn mori_attrs_size_hook;
static mori_attrs_write_fn mori_attrs_write_hook;
static mori_attrs_read_fn mori_attrs_read_hook;

void mori_set_attrs_hooks(mori_attrs_size_fn size,
                               mori_attrs_write_fn write,
                               mori_attrs_read_fn read) {
  mori_attrs_size_hook = size;
  mori_attrs_write_hook = write;
  mori_attrs_read_hook = read;
}

/* The attr blob's size: the embedder's encoding when the hooks are set
   and accept the set (a nonzero return), else R_Serialize. x is the
   attributed object (the hook qualifies it); attrs its serialized-form
   container (mori_get_attrs_for_serialize's). */
static size_t mori_attrs_size(SEXP x, SEXP attrs) {
  if (mori_attrs_size_hook != NULL) {
    size_t n = mori_attrs_size_hook(x);
    if (n != 0) return n;
  }
  return mori_serialize_count(attrs);
}

/* Writes the blob at dst; returns bytes written. The embedder's write
   runs the same qualification as its size pass over the same object, so
   the two agree; a zero return after a nonzero size is a bug, never a
   fallback (the layout has already been sized). */
static size_t mori_attrs_write(unsigned char *dst, SEXP x, SEXP attrs) {
  if (mori_attrs_write_hook != NULL) {
    size_t n = mori_attrs_write_hook(dst, x);
    if (n != 0) return n;
    if (mori_attrs_size_hook != NULL && mori_attrs_size_hook(x) != 0)
      Rf_error("mori: attribute blob write disagrees with its size pass");
  }
  return mori_serialize_into(dst, attrs);
}

// Element directory (for list SHM layout) -------------------------------------

typedef struct {
  int64_t data_offset;
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
} mori_elem;

// SHM eligibility: any atomic vector (attributes stored separately) ---------

static inline int mori_shm_eligible(int type) {
  return type == REALSXP || type == INTSXP || type == LGLSXP ||
         type == RAWSXP || type == CPLXSXP || type == STRSXP;
}

// Type-dispatched data pointer (avoids non-API DATAPTR) -----------------------

static inline void *mori_data_ptr(SEXP x) {
  switch (TYPEOF(x)) {
  case REALSXP:  return (void *) REAL(x);
  case INTSXP:   return (void *) INTEGER(x);
  case LGLSXP:   return (void *) LOGICAL(x);
  case RAWSXP:   return (void *) RAW(x);
  case CPLXSXP:  return (void *) COMPLEX(x);
  default:       return (void *) DATAPTR_RO(x);
  }
}

// Bounds check for a [offset, offset+size) chunk within a region -------------

static inline int mori_oob(int64_t offset, int64_t size, int64_t region_size) {
  return offset < 0 || size < 0 || offset > region_size - size;
}

// Attribute helpers for API compliance ----------------------------------------

/* Named list on R >= 4.6.0, pairlist otherwise. R_NilValue if none.
   Caller must PROTECT. */
static inline SEXP mori_get_attrs_for_serialize(SEXP x) {
#if R_VERSION >= R_Version(4, 6, 0)
  return ANY_ATTRIB(x) ? R_getAttributes(x) : R_NilValue;
#else
  return ATTRIB(x);
#endif
}

/* The raw-tier integer64 gate: a REALSXP whose entire attribute set is
   class = "integer64" (bit64's exact layout) stages as bare int64 bytes —
   the wire tag consumes the class at stage and re-applies it at receive, so
   no attrs section rides the layout. Attribute shape follows
   mori_get_attrs_for_serialize's split (named list on R >= 4.6,
   pairlist below); the CHARSXP comparator is interned, so the class test is
   a pointer compare. */
static int view_int64_classed(SEXP x, int altrep_ok) {
  if (TYPEOF(x) != REALSXP || (!altrep_ok && ALTREP(x)) || Rf_isS4(x))
    return 0;
  SEXP cls = Rf_getAttrib(x, R_ClassSymbol);
  if (TYPEOF(cls) != STRSXP || XLENGTH(cls) != 1 ||
      STRING_ELT(cls, 0) != STRING_ELT(mori_int64_class, 0))
    return 0;
#if R_VERSION >= R_Version(4, 6, 0)
  return XLENGTH(R_getAttributes(x)) == 1;
#else
  /* ATTRIB is a pairlist here, and XLENGTH errors on pairlists: the
     class-only test is exactly one node, the class */
  return TAG(ATTRIB(x)) == R_ClassSymbol && CDR(ATTRIB(x)) == R_NilValue;
#endif
}

int mori_is_int64(SEXP x) {
  return view_int64_classed(x, 0);
}

/* The ALTREP-tolerant half: the capability gates and the interop writer ask
   only whether the class rides the wire tag (an INT64 view's region header
   carries it, and a by-value copy stages through *_GET_REGION either way);
   the raw tier's data-pointer question keeps the ALTREP rejection above. */
int mori_is_int64_any(SEXP x) {
  return view_int64_classed(x, 1);
}

/* Sets class last to avoid validation ordering issues. */
static void mori_set_attrs_from(SEXP result, SEXP attrs) {
#if R_VERSION >= R_Version(4, 6, 0)
  SEXP names = PROTECT(Rf_getAttrib(attrs, R_NamesSymbol));
  R_xlen_t n = XLENGTH(attrs), class_idx = -1;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP nm = Rf_installChar(STRING_ELT(names, i));
    if (nm == R_ClassSymbol) { class_idx = i; continue; }
    Rf_setAttrib(result, nm, VECTOR_ELT(attrs, i));
  }
  if (class_idx >= 0)
    Rf_classgets(result, VECTOR_ELT(attrs, class_idx));
  UNPROTECT(1);
#else
  SET_ATTRIB(result, attrs);
  if (Rf_getAttrib(result, R_ClassSymbol) != R_NilValue)
    SET_OBJECT(result, 1);
#endif
}

/* The one site every attribute-blob read passes through, root and leaf
   alike: the embedder's form (the hooks' encoding, dispatched on the
   blob's first byte — 'I' is the interchange stream's magic) goes to the
   read hook; anything else is R_Unserialize as before. An R reader homes
   any attribute set, so the hook's apply is shape-free — a malformed
   blob rejects in the corrupt-region house style from the cursor's own
   errors. */
void mori_restore_attrs(SEXP result, unsigned char *buf, size_t size) {
  if (size > 0 && buf[0] == MORI_INTEROP_MAGIC &&
      mori_attrs_read_hook != NULL) {
    mori_attrs_read_hook(result, buf, size);
    return;
  }
  SEXP attrs = PROTECT(mori_unserialize_from(buf, size));
  mori_set_attrs_from(result, attrs);
  UNPROTECT(1);
}

// Identifier formatter, chain walker, and parser -----------------------------

/* Tight u32 → decimal emit. Caller guarantees buffer has >= 10 bytes free. */
static inline char *mori_u32_to_dec(char *p, uint32_t v) {
  char tmp[10];
  int n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n--) *p++ = tmp[n];
  return p;
}

/* Format "<name>" (len == 0) or "<name>[i1+1,i2+1,...]" (len > 0) into buf.
   Caller passes 1-based-internal path (already -1 indices that the formatter
   does not adjust); on entry path_internal[i] is 0-based and the formatter
   adds 1 per element. Returns bytes written (excluding NUL), or -1 on
   defensive truncation. */
static int mori_format_path(char *buf, size_t buflen,
                            const char *name, size_t name_len,
                            const int32_t *path_internal, int len) {

  if (name_len + 2 + 11 * (size_t) len >= buflen)
    return -1;  /* defensive; sized for MORI_MAX_PATH worst case */

  char *p = buf;
  memcpy(p, name, name_len);
  p += name_len;

  if (len == 0) {
    *p = '\0';
    return (int)(p - buf);
  }

  *p++ = '[';
  for (int i = 0; i < len; i++) {
    p = mori_u32_to_dec(p, (uint32_t)(path_internal[i] + 1));
    *p++ = ',';
  }
  p[-1] = ']';                     /* overwrite trailing ',' */
  *p = '\0';
  return (int)(p - buf);
}

/* Walk owned-tag hops up from keeper_extptr (= R_ExternalPtrProtected of the
   leaf's data1), placing the leaf_index (if >= 0) at the rear of `path` and
   each collected view->index ahead of it as the walk proceeds. The result
   ends up in root→leaf order in path[rear..MORI_MAX_PATH) without a second
   pass. Returns 0 on success, -1 on MORI_MAX_PATH overflow, -2 on malformed
   chain (terminus not shm_tag, or NULL addr). */
static int mori_format_chain(SEXP keeper_extptr, int32_t leaf_index,
                             char *buf, size_t buflen) {

  int32_t path[MORI_MAX_PATH];
  int rear = MORI_MAX_PATH;            /* exclusive upper bound; fill toward 0 */

  if (leaf_index >= 0) path[--rear] = leaf_index;

  SEXP hop = keeper_extptr;
  while (TYPEOF(hop) == EXTPTRSXP &&
         R_ExternalPtrTag(hop) == mori_owned_tag) {
    mori_list_view *view = (mori_list_view *) R_ExternalPtrAddr(hop);
    if (view == NULL) return -2;
    int32_t idx = view->index;
    if (idx >= 0) {
      if (rear == 0) return -1;
      path[--rear] = idx;
    }
    hop = R_ExternalPtrProtected(hop);
  }

  if (TYPEOF(hop) != EXTPTRSXP || R_ExternalPtrTag(hop) != mori_shm_tag)
    return -2;
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(hop);
  if (shm == NULL) return -2;

  return mori_format_path(buf, buflen, shm->name, shm->name_len,
                          path + rear, MORI_MAX_PATH - rear) < 0 ? -1 : 0;
}

/* Parse "<prefix>" or "<prefix>[i1,i2,...]" with 1-based positive indices.
   Returns 1 (full match with path), 0 (prefix only), or -1 (malformed).
   On rc 0 or 1, name_out is NUL-terminated with the prefix (variable
   length, bounded by name_out_size - 1; pass MORI_NAME_MAX). On rc 1,
   path_out (capacity MORI_MAX_PATH) is filled with 0-based indices and
   *path_len is set to the count (>= 1). On rc -1, all outputs are
   indeterminate. */
int mori_parse_id(const char *s, char *name_out, size_t name_out_size,
                  int32_t *path_out, int *path_len) {

  /* Step 1: length cap (cheapest possible bound) */
  const char *eos = (const char *) memchr(s, '\0', MORI_IDENTIFIER_MAX);
  if (eos == NULL) return -1;

  /* Step 2: literal-prefix match */
  const size_t lit_len = sizeof(MORI_PREFIX_LITERAL) - 1;
  if ((size_t)(eos - s) < lit_len) return -1;
  if (memcmp(s, MORI_PREFIX_LITERAL, lit_len) != 0) return -1;
  const char *p = s + lit_len;

  /* Step 3: variable-width random-part scan: hex+ '_' hex+. Each *p deref
     is guarded by p < eos; each iteration advances p by 1; total work is
     bounded by MORI_NAME_MAX via the (p - s) < name_out_size cap. */
  const char *r = p;
  while (p < eos && (size_t)(p - s) < name_out_size &&
         ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
    p++;
  if (p == r) return -1;                /* empty first hex run */
  if (p >= eos || *p != '_') return -1; /* missing separator */
  p++;
  r = p;
  while (p < eos && (size_t)(p - s) < name_out_size &&
         ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
    p++;
  if (p == r) return -1;                /* empty second hex run */

  /* Step 4: copy prefix, dispatch on next byte */
  size_t prefix_len = (size_t)(p - s);
  if (prefix_len >= name_out_size) return -1;  /* prefix too long for buf */
  memcpy(name_out, s, prefix_len);
  name_out[prefix_len] = '\0';

  if (p == eos) return 0;               /* prefix only */
  if (*p != '[') return -1;
  p++;

  /* Step 5: path parse */
  int count = 0;
  while (1) {
    /* First digit of token: must be 1-9 (rejects empty, leading zero,
       junk, and '\0' since '\0' < '1') */
    if (*p < '1' || *p > '9') return -1;
    uint32_t val = (uint32_t)(*p - '0');
    p++;
    while (*p >= '0' && *p <= '9') {
      uint32_t d = (uint32_t)(*p - '0');
      if (val > (uint32_t) INT32_MAX / 10 ||
          (val == (uint32_t) INT32_MAX / 10 &&
           d > (uint32_t) INT32_MAX % 10))
        return -1;
      val = val * 10 + d;
      p++;
    }
    if (count >= MORI_MAX_PATH) return -1;
    path_out[count++] = (int32_t)(val - 1);  /* convert to 0-based */

    if (*p == ',') { p++; continue; }
    if (*p == ']') { p++; break; }
    return -1;
  }

  if (p != eos) return -1;              /* trailing junk after ']' */
  *path_len = count;
  return 1;
}

// Generic finalizer for mori_owned_tag extptrs (vec / str / view) ------------

/* The embedder release callback (the mori_owned first member of every owned
   struct): fired here or at COW materialization, whichever comes first — the
   NULL store is what makes it once-only. */
static inline void mori_release_once(mori_owned *o) {
  if (o->release != NULL) {
    o->release(o->release_arg);
    o->release = NULL;
  }
}

static void mori_owned_finalizer(SEXP ptr) {
  mori_owned *o = R_ExternalPtrAddr(ptr);
  if (o != NULL) {
    mori_release_once(o);
    free(o);
    R_ClearExternalPtr(ptr);
  }
}

// ALTREP atomic vector methods (shared by all 5 types) ------------------------

/*
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_vec *)
 *           protected value keeps the parent SHM extptr alive (shm_tag for
 *           standalone, parent view for element within an ALTLIST)
 *   data2 = R_NilValue while SHM-backed; regular SEXP after materialization
 */

static R_xlen_t mori_vec_Length(SEXP x) {
  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return XLENGTH(d2);
  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v->length;
}

static const void *mori_vec_Dataptr_or_null(SEXP x) {
  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return DATAPTR_RO(d2);
  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v->data;
}

static void *mori_vec_Dataptr(SEXP x, Rboolean writable) {

  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return mori_data_ptr(d2);

  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(R_altrep_data1(x));

  if (!writable)
    return (void *) v->data;

  /* COW: materialize to a regular R vector. Attributes live on the ALTREP
     wrapper x, not on data2 — no method reads data2's attrs, and R's
     ALTREP serialize reapplies ATTRIB(x) on top of the unserialized state. */
  R_xlen_t n = v->length;
  int type = TYPEOF(x);
  SEXP mat = PROTECT(Rf_allocVector(type, n));
  void *p = mori_data_ptr(mat);
  memcpy(p, v->data, (size_t) n * mori_sizeof_elt(type));
  R_set_altrep_data2(x, mat);
  UNPROTECT(1);

  /* the shared pages are dead weight from here — release them early */
  mori_release_once(&v->owned);

  return p;
}

/* keeper: SEXP kept alive via the extptr's protected slot (parent SHM).
   release/release_arg: embedder once-hook (mori_owned member). */
SEXP mori_vec_wrap(const void *data, R_xlen_t length, int sexptype,
                   SEXP keeper, mori_release_fn release, void *release_arg) {

  R_altrep_class_t cls;
  switch (sexptype) {
  case REALSXP:  cls = mori_real_class;    break;
  case INTSXP:   cls = mori_integer_class; break;
  case LGLSXP:   cls = mori_logical_class; break;
  case RAWSXP:   cls = mori_raw_class;     break;
  case CPLXSXP:  cls = mori_complex_class; break;
  case MORI_TYPE_INT64: cls = mori_real_class; break;
  default:       Rf_error("mori: unsupported ALTREP type %d", sexptype);
  }

  mori_vec *v = malloc(sizeof(mori_vec));
  if (v == NULL) Rf_error("mori: allocation failure");
  v->owned.release = release;
  v->owned.release_arg = release_arg;
  v->data = data;
  v->length = length;
  v->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(v, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  SEXP result = PROTECT(R_new_altrep(cls, ptr, R_NilValue));
  /* the single class-application home: covers the SHM_VEC/REF top-level
     read, the identifier walk/resolve path, and MORL leaf reads */
  if (sexptype == MORI_TYPE_INT64)
    Rf_classgets(result, mori_int64_class);
  UNPROTECT(2);
  return result;
}

// ALTSTRING methods -----------------------------------------------------------

/*
 * The string block (mori.h, mori_str_geometry): validity bitmap,
 * (n + 1) int64 offsets, n encoding bytes, then the packed string bytes.
 *
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_str *)
 *           protected value keeps the parent SHM extptr alive (shm_tag for
 *           standalone, parent view for element within an ALTLIST)
 *   data2 = R_NilValue while SHM-backed; regular STRSXP after materialization
 */

typedef struct {
  mori_owned owned;
  const unsigned char *validity;
  const unsigned char *offsets;
  const unsigned char *encoding;
  const unsigned char *data;
  R_xlen_t length;
  int64_t str_bytes;  /* size of the packed string area; bounds each span */
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} mori_str;

static inline SEXP mori_string_elt_shm(mori_str *s, R_xlen_t i) {
  if (!mori_str_valid(s->validity, (size_t) i)) return NA_STRING;

  int64_t lo, hi;
  memcpy(&lo, s->offsets + 8 * (size_t) i, 8);
  memcpy(&hi, s->offsets + 8 * ((size_t) i + 1), 8);
  unsigned enc = s->encoding[i];

  /* Bounds-check the span against the packed string area before reading;
     a CHARSXP holds at most INT_MAX bytes. */
  if (lo < 0 || hi < lo || hi > s->str_bytes || hi - lo > INT_MAX ||
      enc > CE_BYTES)
    Rf_error("mori: invalid string data");

  return Rf_mkCharLenCE((const char *) (s->data + lo), (int) (hi - lo),
                        (cetype_t) enc);
}

static R_xlen_t mori_string_Length(SEXP x) {
  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return XLENGTH(d2);
  mori_str *s = (mori_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  return s->length;
}

static SEXP mori_string_Elt(SEXP x, R_xlen_t i) {
  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return STRING_ELT(d2, i);
  mori_str *s = (mori_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  return mori_string_elt_shm(s, i);
}

static const void *mori_string_Dataptr_or_null(SEXP x) {
  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return DATAPTR_RO(d2);
  return NULL;
}

static void *mori_string_Dataptr(SEXP x, Rboolean writable) {
  SEXP d2 = R_altrep_data2(x);
  if (d2 != R_NilValue)
    return (void *) DATAPTR_RO(d2);

  mori_str *s = (mori_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  R_xlen_t n = s->length;
  SEXP mat = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(mat, i, mori_string_elt_shm(s, i));
  R_set_altrep_data2(x, mat);
  UNPROTECT(1);

  mori_release_once(&s->owned);

  return (void *) DATAPTR_RO(mat);
}

static SEXP mori_string_Duplicate(SEXP x, Rboolean deep) {
  (void) deep;
  R_xlen_t n = XLENGTH(x);
  SEXP result = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(result, i, STRING_ELT(x, i));
  DUPLICATE_ATTRIB(result, x);
  UNPROTECT(1);
  return result;
}

/* region_base: points to the string block. data_size: bytes available for
   the block (the caller excludes any trailing attributes) — the sections
   ahead of the string bytes must fit within it, and the remainder is
   recorded as str_bytes to bound each offset span at Elt time. The two end
   offsets are checked here, in O(1); every span is checked as it is read.
   keeper: SEXP kept alive via the extptr's protected slot (parent SHM). */
SEXP mori_str_wrap(const unsigned char *region_base, R_xlen_t n,
                   int64_t data_size, SEXP keeper,
                   mori_release_fn release, void *release_arg) {

  /* n bounded by the offsets section alone keeps the geometry overflow-free */
  if (n < 0 || data_size < 0 || n > data_size / 8)
    Rf_error("mori: invalid string data");

  mori_str_geom g = mori_str_geometry((size_t) n);
  if (g.data > (size_t) data_size)
    Rf_error("mori: invalid string data");

  int64_t str_bytes = data_size - (int64_t) g.data, first, last;
  memcpy(&first, region_base + g.offsets, 8);
  memcpy(&last, region_base + g.offsets + 8 * (size_t) n, 8);
  if (first != 0 || last < 0 || last > str_bytes)
    Rf_error("mori: invalid string data");

  mori_str *s = malloc(sizeof(mori_str));
  if (s == NULL) Rf_error("mori: allocation failure");

  s->owned.release = release;
  s->owned.release_arg = release_arg;
  s->validity = region_base + g.validity;
  s->offsets = region_base + g.offsets;
  s->encoding = region_base + g.encoding;
  s->data = region_base + g.data;
  s->length = n;
  s->str_bytes = str_bytes;
  s->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(s, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  SEXP result = R_new_altrep(mori_string_class, ptr, R_NilValue);
  UNPROTECT(1);
  return result;
}

// Element extraction helper (shared by list Elt and open_path) ----------------

/* The remote-leaf (tag 33) reader, defined next to the one resolve path. */
static SEXP mori_unwrap_remote(unsigned char *base, int64_t region_size,
                                    int32_t index, int s4,
                                    int64_t data_offset, int64_t data_size,
                                    int64_t length, int32_t attrs_size);

static SEXP mori_unwrap_element(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper) {

  unsigned char *dir = base + MORI_HEADER_SIZE + 32 * (size_t) index;
  mori_elem entry;
  memcpy(&entry, dir, sizeof(mori_elem));
  int64_t data_offset = entry.data_offset, data_size = entry.data_size;
  int64_t length = entry.length;
  int32_t sexptype = entry.sexptype, attrs_size = entry.attrs_size;
  int s4 = sexptype & MORI_ELEM_S4;
  sexptype &= ~MORI_ELEM_S4;

  if (mori_oob(data_offset, data_size, region_size))
    Rf_error("mori: invalid element data");
  if ((data_offset & 63) != 0)
    Rf_error("mori: invalid element data");   /* entries are 64-aligned */
  /* A remote leaf's attrs_size describes the referenced column as resolved
     — no relation to the local identifier span (the factor case: a 30-byte
     span referencing a leaf with a 200-byte blob) */
  if (attrs_size < 0 ||
      (sexptype != MORI_TAG_REF && attrs_size > data_size))
    Rf_error("mori: invalid element data");

  SEXP result;
  if (sexptype == VECSXP) {
    /* Nested MORL at base+data_offset; attrs live inside child region */
    result = PROTECT(mori_list_wrap(
      base + data_offset, data_size, index, keeper, NULL, NULL
    ));
  } else if (sexptype == STRSXP) {
    result = PROTECT(mori_str_wrap(
      base + data_offset, (R_xlen_t) length, data_size - attrs_size, keeper,
      NULL, NULL
    ));
    ((mori_str *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else if (sexptype == MORI_TAG_REF) {
    result = PROTECT(mori_unwrap_remote(
      base, region_size, index, s4, data_offset, data_size, length, attrs_size
    ));
  } else if (sexptype != 0) {
    /* The claimed element data must fit within the directory entry's data
       region (minus trailing attributes) */
    size_t elt_size = mori_sizeof_elt(sexptype);
    if (elt_size != 0 &&
        (length < 0 ||
         length > (data_size - attrs_size) / (int64_t) elt_size))
      Rf_error("mori: invalid element data");
    result = PROTECT(mori_vec_wrap(
      base + data_offset, (R_xlen_t) length, sexptype, keeper, NULL, NULL
    ));
    ((mori_vec *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else {
    result = PROTECT(mori_unserialize_from(
      base + (size_t) data_offset, (size_t) data_size
    ));
  }

  /* A remote leaf skips the local attr restore: its attrs_size describes
     the referenced column, and the resolved view carries its own
     attributes (the S4 tail needs no guard — s4 rejects on a remote leaf) */
  if (attrs_size > 0 && sexptype != 0 && sexptype != VECSXP &&
      sexptype != MORI_TAG_REF) {
    size_t attrs_off = (size_t)(data_offset + data_size - attrs_size);
    mori_restore_attrs(result, base + attrs_off, (size_t) attrs_size);
  }
  if (s4) result = Rf_asS4(result, TRUE, 0);

  UNPROTECT(1);
  return result;
}

// ALTLIST methods -------------------------------------------------------------

/*
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_list_view *)
 *           prot: parent extptr (shm_tag for root; parent view for sub-list)
 *   data2 = R_NilValue or cache VECSXP (length n; R_NilValue slot = uncached
 *           or NIL element — re-extracted on cache miss either way)
 *
 * MORL region layout (same whether root SHM or nested inside a parent):
 *   Bytes 0-3:   uint32_t magic (MORI_MAGIC_LIST, "MORL")
 *   Bytes 4-7:   int32_t  n_elements
 *   Bytes 8-15:  int64_t  attrs_offset
 *   Bytes 16-23: int64_t  attrs_size
 *   Bytes 24-31: reserved (zero) — embedder cross-process state
 *   Bytes 32-35: uint32_t flags (bit 0: S4 object bit)
 *   Bytes 36-63: reserved (zero)
 *   Byte 64+:    element directory (32 bytes per element)
 *   Then the element data, each entry's data_offset 64-byte aligned: bare
 *   element bytes (an atomic leaf), a string block (a STRSXP leaf — mori.h,
 *   mori_str_geometry), a nested MORL (a VECSXP) or an R_Serialize
 *   stream (sexptype 0); a leaf's attrs blob trails its data (the last
 *   attrs_size bytes of data_size).
 */

/* Validate a MORL region and return a freshly allocated owned-tag extptr
   wrapping its mori_list_view. No ALTREP wrapper, no attribute restoration —
   suitable for path-walk intermediates whose ALTLIST view is never observed.
   keeper: parent extptr (shm_tag for root, owned_tag for sub-list).
   out_attrs_offset / out_attrs_size: optional out-params (NULL to skip);
   when non-NULL, populated with the validated attrs locator so the caller
   can avoid re-reading the header.
   Errors cleanly on corrupt input rather than SEGV. */
static SEXP mori_make_extptr(unsigned char *base, int64_t region_size,
                                  int32_t index, SEXP keeper,
                                  int64_t *out_attrs_offset,
                                  int64_t *out_attrs_size) {

  if (region_size < MORI_HEADER_SIZE)
    Rf_error("mori: invalid nested list region");

  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic != MORI_MAGIC_LIST)
    Rf_error("mori: invalid nested list region");

  int32_t n;
  int64_t attrs_offset, attrs_size;
  memcpy(&n, base + 4, 4);
  memcpy(&attrs_offset, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  if (n < 0 || n > (region_size - MORI_HEADER_SIZE) / 32 ||
      mori_oob(attrs_offset, attrs_size, region_size) ||
      !mori_flags_known(base))
    Rf_error("mori: invalid nested list region");

  mori_list_view *v = malloc(sizeof(mori_list_view));
  if (v == NULL) Rf_error("mori: allocation failure");
  v->owned.release = NULL;
  v->owned.release_arg = NULL;
  v->base = base;
  v->region_size = region_size;
  v->n_elements = n;
  v->index = index;

  SEXP ptr = R_MakeExternalPtr(v, mori_owned_tag, keeper);
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  if (out_attrs_offset != NULL) *out_attrs_offset = attrs_offset;
  if (out_attrs_size != NULL)   *out_attrs_size   = attrs_size;
  return ptr;
}

/* User-visible ALTLIST: validates header, allocates view, restores attrs. */
SEXP mori_list_wrap(unsigned char *base, int64_t region_size, int32_t index,
                    SEXP keeper, mori_release_fn release, void *release_arg) {

  int64_t attrs_offset, attrs_size;
  SEXP ptr = PROTECT(mori_make_extptr(
    base, region_size, index, keeper, &attrs_offset, &attrs_size
  ));
  mori_list_view *v = (mori_list_view *) R_ExternalPtrAddr(ptr);
  v->owned.release = release;
  v->owned.release_arg = release_arg;

  /* Cache is allocated lazily on first Elt access */
  SEXP result = PROTECT(R_new_altrep(mori_list_class, ptr, R_NilValue));

  if (attrs_size > 0)
    mori_restore_attrs(result, base + (size_t) attrs_offset,
                       (size_t) attrs_size);
  result = mori_apply_s4(result, base);

  UNPROTECT(2);
  return result;
}

static R_xlen_t mori_list_Length(SEXP x) {
  mori_list_view *v = (mori_list_view *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v != NULL ? v->n_elements : 0;
}

static SEXP mori_list_Elt(SEXP x, R_xlen_t i) {

  SEXP d1 = R_altrep_data1(x);
  mori_list_view *view = (mori_list_view *) R_ExternalPtrAddr(d1);

  /* Lazy cache allocation. Fresh VECSXP is naturally R_NilValue-filled,
     which serves as the "uncached" sentinel — no init loop needed. We
     don't cache results that are themselves R_NilValue: it's a singleton
     (`mori_unwrap_element`'s only NIL-producing path returns R's one true
     R_NilValue), so re-extracting on a cache miss preserves identity for
     free without needing a separate sentinel. */
  SEXP cache = R_altrep_data2(x);
  if (cache == R_NilValue) {
    cache = PROTECT(Rf_allocVector(VECSXP, view->n_elements));
    R_set_altrep_data2(x, cache);
    UNPROTECT(1);
  }

  SEXP cached = VECTOR_ELT(cache, i);
  if (cached != R_NilValue) return cached;

  SEXP result = mori_unwrap_element(view->base, view->region_size,
                                    (int32_t) i, d1);
  if (result != R_NilValue)
    SET_VECTOR_ELT(cache, i, result);
  return result;
}

/* NULL forces R to use Elt() for element access */
static const void *mori_list_Dataptr_or_null(SEXP x) {
  return NULL;
}

/* Full materialization fallback. The release hook is NOT fired here,
   unlike the leaf-vector materializations: extracted element views keep
   referencing the region's pages through their own keeper chains, so a
   list view is never fully detached while it or its elements live — the
   release fires at the finalizer only. */
static void *mori_list_Dataptr(SEXP x, Rboolean writable) {
  R_xlen_t n = mori_list_Length(x);
  for (R_xlen_t i = 0; i < n; i++)
    mori_list_Elt(x, i);
  if (R_altrep_data2(x) == R_NilValue) {
    SEXP cache = PROTECT(Rf_allocVector(VECSXP, n));
    R_set_altrep_data2(x, cache);
    UNPROTECT(1);
  }
  return mori_data_ptr(R_altrep_data2(x));
}

/* COW: modification produces a regular list */
static SEXP mori_list_Duplicate(SEXP x, Rboolean deep) {
  R_xlen_t n = mori_list_Length(x);
  SEXP result = PROTECT(Rf_allocVector(VECSXP, n));
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = mori_list_Elt(x, i);
    SET_VECTOR_ELT(result, i, deep ? Rf_duplicate(elt) : elt);
  }
  DUPLICATE_ATTRIB(result, x);
  UNPROTECT(1);
  return result;
}

static SEXP mori_dispatch_by_magic(SEXP shm_ptr, const char *err_name);

// SHM extptr finalizers ------------------------------------------------------

/* Mapping finalizer (both sides): releases this side's mapping only.
   The name (POSIX) / creator handle (Windows) is released independently
   by mori_host_finalizer on the chained host_tag extptr — so a consumer
   keeps reading after the host is GC'd. */
void mori_shm_finalizer(SEXP ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(ptr);
  if (shm != NULL) {
    mori_shm_close_stack(shm, 0);
    free(shm);
    R_ClearExternalPtr(ptr);
  }
}

/* Host-side finalizer: releases the SHM name/handle. */
void mori_host_finalizer(SEXP ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(ptr);
  if (shm != NULL) {
    mori_shm_host_release(shm);
    free(shm);
    R_ClearExternalPtr(ptr);
  }
}

// SHM keeper wrappers: transfer heap mori_shm ownership to R -----------------

/* Consumer-side: single shm_tag extptr with munmap-only finalizer. Takes
   ownership of shm; the returned SEXP's finalizer frees it on GC. */
static SEXP mori_shm_wrap_consumer(mori_shm *shm) {
  SEXP ptr = R_MakeExternalPtr(shm, mori_shm_tag, R_NilValue);
  R_RegisterCFinalizerEx(ptr, mori_shm_finalizer, TRUE);
  return ptr;
}

/* Producer-side: shm_tag extptr (munmap finalizer) chained to a host_tag
   extptr (unlink / CloseHandle finalizer) via its protected slot. Takes
   ownership of shm. The host copy gets the name / Windows handle; shm
   keeps only the mapping, so munmap and unlink fire independently. */
static SEXP mori_shm_wrap_producer(mori_shm *shm) {

  mori_shm *host = malloc(sizeof(mori_shm));
  if (host == NULL) Rf_error("mori: allocation failure");
  memcpy(host, shm, sizeof(mori_shm));
  host->addr = NULL;
  host->size = 0;
#ifdef _WIN32
  shm->handle = NULL;
#endif

  SEXP host_ptr = PROTECT(R_MakeExternalPtr(host, mori_host_tag, R_NilValue));
  R_RegisterCFinalizerEx(host_ptr, mori_host_finalizer, TRUE);

  SEXP shm_ptr = R_MakeExternalPtr(shm, mori_shm_tag, host_ptr);
  R_RegisterCFinalizerEx(shm_ptr, mori_shm_finalizer, TRUE);

  UNPROTECT(1);
  return shm_ptr;
}

static SEXP mori_make_result(mori_shm *shm) {
  SEXP shm_ptr = PROTECT(mori_shm_wrap_producer(shm));
  SEXP result = mori_dispatch_by_magic(shm_ptr, NULL);
  UNPROTECT(1);
  return result;
}

// Consumer-mapping cache (identifier resolve paths) -----------------------------

/* The wire-resolve paths (the ALTREP Unserialize methods) dedupe consumer
   mappings per region name: a payload carrying many references to few
   regions would otherwise multiply mappings on the receiver (Linux caps a
   process's VMA count — enough nested references against stock limits wedge
   the resolve with ENOMEM). Process-global, not per-handle: R_Unserialize's
   ALTREP method gets (class_info, state) only — no handle rides the resolve.
   Sound because the protocol separates the concerns: mappings are per-region
   (shared here), refcounts per-view (each resolve fires the resolve hook and
   each view adds/subs its own count on the shared base). Eviction never
   unmaps: a live view pins the cached wrap through its keeper chain, so
   eviction only drops the cache's reference. One behavioral caveat: a cached
   mapping resolves a name whose region was since unlinked (a forged/stale
   identifier then reads stale-but-valid pages instead of erroring "not
   found") — unreachable in-protocol, where the sender's keeper pins the
   region through the read. */
static SEXP mori_cache_wraps;  /* VECSXP(MORI_CACHE_MAX), preserved at init */
static char mori_cache_names[MORI_CACHE_MAX][MORI_NAME_MAX];
static uint8_t mori_cache_len[MORI_CACHE_MAX];   /* 0 = empty slot */
static uint64_t mori_cache_stamp[MORI_CACHE_MAX];
static uint64_t mori_cache_tick;

/* The name-keyed lookup: the wrap SEXP on a hit (stamp bumped), R_NilValue
   on a miss or a finalized entry (the caller re-opens and re-stores). A
   forked child inherits the entries — the mappings are its own; an entry
   finalized before the fork reads as a miss and reopens. */
static SEXP mori_cache_find(const char *name, size_t len) {
  for (int i = 0; i < MORI_CACHE_MAX; i++)
    if (mori_cache_len[i] == len &&
        memcmp(mori_cache_names[i], name, len) == 0) {
      SEXP wrap = VECTOR_ELT(mori_cache_wraps, i);
      if (R_ExternalPtrAddr(wrap) == NULL) return R_NilValue;  /* finalized */
      mori_cache_stamp[i] = ++mori_cache_tick;
      return wrap;
    }
  return R_NilValue;
}

static void mori_cache_store(const char *name, size_t len, SEXP wrap) {
  int slot = 0;
  for (int i = 0; i < MORI_CACHE_MAX; i++) {
    if (mori_cache_len[i] == 0) {
      slot = i;
      break;
    }
    if (mori_cache_stamp[i] < mori_cache_stamp[slot]) slot = i;
  }
  SET_VECTOR_ELT(mori_cache_wraps, slot, wrap);  /* the evicted LRU drops to GC */
  memcpy(mori_cache_names[slot], name, len);
  mori_cache_len[slot] = (uint8_t) len;
  mori_cache_stamp[slot] = ++mori_cache_tick;
}

/* The single home of the resolve paths' consumer open: cache consult, then
   the embedder's open hook or the default fully-RO open, wrapped and cached.
   The resolve hook is the call sites', fired per resolve (the counted add
   belongs to the view, not the mapping) — never inside this miss branch, or
   a cache hit would skip it. Returns the wrap UNPROTECTED — the caller
   PROTECTs (nothing allocates between). The _or_null form lets the caller
   shape the gone-region error (the remote-leaf reader's decline). */
static SEXP mori_open_consumer_or_null(const char *name) {
  size_t len = strlen(name);
  SEXP wrap = mori_cache_find(name, len);
  if (wrap != R_NilValue) return wrap;
  mori_shm *shm = mori_open_hook != NULL ?
    mori_open_hook(name) : mori_shm_open_heap(name);
  if (shm == NULL) return R_NilValue;
  wrap = mori_shm_wrap_consumer(shm);
  mori_cache_store(name, len, wrap);
  return wrap;
}

static SEXP mori_open_consumer(const char *name) {
  SEXP wrap = mori_open_consumer_or_null(name);
  if (wrap == R_NilValue)
    Rf_error("mori: shared memory region not found: '%s'", name);
  return wrap;
}

// String write helper (shared by standalone and list paths) -------------------

/* Writes the string block for x at dest (mori.h documents the form).
   Returns total bytes written (including alignment padding). */
static size_t mori_write_strings(unsigned char *dest, SEXP x) {
  R_xlen_t n = XLENGTH(x);
  mori_str_geom g = mori_str_geometry((size_t) n);
  unsigned char *validity = dest + g.validity;
  unsigned char *offsets = dest + g.offsets;
  unsigned char *encoding = dest + g.encoding;
  unsigned char *data = dest + g.data;

  /* Everything ahead of the string bytes starts zeroed: a clear validity
     bit and a zero encoding byte are the NA form, and the alignment gaps
     carry nothing an embedder's region reuse could leave stale. The
     offsets are then written in full (offsets[0] is the zero). */
  memset(dest, 0, g.data);

  int64_t cur = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    if (elt != NA_STRING) {
      size_t slen = (size_t) LENGTH(elt);
      memcpy(data + cur, CHAR(elt), slen);
      cur += (int64_t) slen;
      validity[i >> 3] |= (unsigned char) (1u << (i & 7));
      encoding[i] = (unsigned char) Rf_getCharCE(elt);
    }
    memcpy(offsets + 8 * ((size_t) i + 1), &cur, 8);
  }

  return g.data + (size_t) cur;
}

static size_t mori_string_data_size(SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t str_bytes = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    if (elt != NA_STRING)
      str_bytes += (size_t) LENGTH(elt);
  }
  return mori_str_geometry((size_t) n).data + str_bytes;
}

// .Call entry points: host-side SHM creation ---------------------------------

// Recursive size/write helpers for nested list regions -----------------------

static size_t mori_nested_size(SEXP x, int *ok, int foreign);
static int mori_na_present(int type, const void *src, uint64_t n);
static size_t mori_nested_write(unsigned char *base, SEXP x, int foreign);
/* The remote-leaf (tag 33) writer's descriptor helpers, defined with the
   resolve path. */
static int mori_ref_descriptor(SEXP elt, SEXP id, int64_t *length,
                                    int64_t *attrs, int *na_free);

/* Total bytes occupied by a MORL region for VECSXP x, including header,
   directory, elements (recursing into VECSXP/LISTSXP children), trailing
   attrs, and all 64-byte alignment padding. Caller passes a VECSXP; any
   LISTSXP children are coerced locally during recursion. The foreign
   staging mode adds the validity tail's reserve (the write spends it
   only where NAs are present, so the write returns at most this).
   ok is NULL on the host path. When non-NULL (the embedder layout oracle),
   each node is vetted before sizing and the first rejection sets *ok = 0 and
   bails out with return 0: an ALTREP node that is neither a view
   nor directly readable with no keeper chain (mori_altrep_readable)
   would materialize through DATAPTR_RO at write or duplicate a wire
   identity (a compact 1:1e8 becomes an 800 MB memcpy). R's S4 data-part
   wrappers forward to their materialized data part and are accepted. The
   foreign mode relaxes this: the foreign filter has already vetted the
   tree, and the write copies any atomic ALTREP through *_GET_REGION. */
static size_t mori_nested_size(SEXP x, int *ok, int foreign) {

  R_xlen_t n = XLENGTH(x);
  size_t total = MORI_ALIGN64(MORI_HEADER_SIZE + 32 * (size_t) n);
  size_t valid_reserve = 0;
  int any_na_atomic = 0;
  int any_remote = 0;

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);

    if (ok != NULL && !foreign) {
      if (ALTREP(elt) && !mori_view_check(elt) && !mori_altrep_readable(elt)) {
        *ok = 0; return 0;
      }
    }

    int type = TYPEOF(elt);
    size_t elt_size;

    if (mori_refable(elt)) {
      SEXP id = PROTECT(mori_shm_name(elt));
      if (id != R_NilValue) {
        /* a remote leaf (F2.5): the identifier span, no local bytes — the
           bitmap reserve is the local leaves', the table reserve any
           leaf's that may carry a claim */
        elt_size = (size_t) LENGTH(STRING_ELT(id, 0));
        any_remote = 1;
      } else {
        /* a view whose identifier broke: not remote-capable, and the copy
           path must not duplicate a wire identity either */
        if (ok != NULL) { *ok = 0; UNPROTECT(1); return 0; }
        elt_size = mori_serialize_count(elt);
      }
      UNPROTECT(1);
    } else if (type == VECSXP || (type == LISTSXP && !Rf_isS4(elt))) {
      SEXP coerced = (type == LISTSXP) ? Rf_coerceVector(elt, VECSXP) : elt;
      PROTECT(coerced);
      elt_size = mori_nested_size(coerced, ok, foreign);
      UNPROTECT(1);
      if (ok != NULL && !*ok) return 0;
    } else if (mori_shm_eligible(type)) {
      /* the integer64 leaf gate: the directory tag carries the class, no
         blob (mizh's root treatment — the write gates identically) */
      int int64 = type != STRSXP && mori_is_int64_any(elt);
      size_t raw_size = (type == STRSXP) ?
        mori_string_data_size(elt) :
        (size_t) XLENGTH(elt) * mori_sizeof_elt(type);
      SEXP elt_attrs = PROTECT(mori_get_attrs_for_serialize(elt));
      size_t attrs_size = 0;
      if (elt_attrs != R_NilValue && !int64)
        attrs_size = mori_attrs_size(elt, elt_attrs);
      UNPROTECT(1);
      elt_size = raw_size + attrs_size;
      if (foreign && type != STRSXP && type != RAWSXP) {
        any_na_atomic = 1;
        valid_reserve += 63 + ((size_t) XLENGTH(elt) + 7) / 8;
      }
    } else {
      elt_size = mori_serialize_count(elt);
    }

    total += MORI_ALIGN64(elt_size);
  }

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ?
    mori_attrs_size(x, attrs) : 0;
  UNPROTECT(1);
  total += MORI_ALIGN64(attrs_size);
  if (any_na_atomic || any_remote) valid_reserve += 63 + 16 * (size_t) n;
  return total + valid_reserve;
}

/* Writes a complete MORL region for VECSXP x starting at base. Returns
   total bytes written (mori_nested_size(x) exactly when foreign ==
   0; at most it in the foreign staging mode, the difference the clean
   leaves' unspent validity bytes). Blob sizes are read off the write
   itself — the blob hook's cursor return and mori_write_strings'
   data-size return — so the counting pass that mori_nested_size
   already ran is never repeated here. */
static size_t mori_nested_write(unsigned char *base, SEXP x, int foreign) {

  R_xlen_t n = XLENGTH(x);
  size_t cur = MORI_ALIGN64(MORI_HEADER_SIZE + 32 * (size_t) n);

  /* Reserved header bytes [24-63] are zeroed on every write: an embedder
     may recycle regions, so no stale field may survive a reuse. */
  memset(base + 24, 0, MORI_HEADER_SIZE - 24);
  uint32_t flags = Rf_isS4(x) ? MORI_FLAG_S4 : 0u;
  memcpy(base + MORI_FLAGS_OFF, &flags, 4);

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);
    int type = TYPEOF(elt);
    mori_elem entry;
    entry.data_offset = (int64_t) cur;

    int is_remote = 0;
    if (mori_refable(elt)) {
      SEXP id = PROTECT(mori_shm_name(elt));
      int64_t rlength = 0, rattrs = 0;
      int rna_free = 0;
      if (id != R_NilValue &&
          mori_ref_descriptor(elt, id, &rlength, &rattrs, &rna_free) == 0) {
        /* the remote leaf (F2.5): the identifier span and the referenced
           column's own length / attrs size as resolved; the emit hook
           marks the region REFHELD (the holder set widens) */
        size_t len = (size_t) LENGTH(STRING_ELT(id, 0));
        memcpy(base + cur, CHAR(STRING_ELT(id, 0)), len);
        entry.sexptype = MORI_TAG_REF;
        entry.attrs_size = (int32_t) rattrs;
        entry.length = rlength;
        entry.data_size = (int64_t) len;
        if (mori_emit_hook != NULL) mori_emit_hook(elt);
        is_remote = 1;
      }
      UNPROTECT(1);
      /* a broken identifier: the host path never meets it (the probe
         rejects); the copy forms below take it */
    }

    if (is_remote) {
      cur += MORI_ALIGN64((size_t) entry.data_size);
    } else if (type == VECSXP || (type == LISTSXP && !Rf_isS4(elt))) {
      SEXP coerced = (type == LISTSXP) ? Rf_coerceVector(elt, VECSXP) : elt;
      PROTECT(coerced);
      size_t written = mori_nested_write(base + cur, coerced, foreign);
      entry.sexptype = VECSXP;
      entry.attrs_size = 0;
      entry.length = (int64_t) XLENGTH(coerced);
      entry.data_size = (int64_t) written;
      UNPROTECT(1);
      cur += MORI_ALIGN64(written);
    } else if (mori_shm_eligible(type)) {
      /* the integer64 leaf gate (the size pass gates identically) */
      int int64 = type != STRSXP && mori_is_int64_any(elt);
      SEXP elt_attrs = PROTECT(mori_get_attrs_for_serialize(elt));
      size_t raw_size, attrs_size = 0;

      if (type == STRSXP) {
        raw_size = mori_write_strings(base + cur, elt);
      } else {
        raw_size = (size_t) XLENGTH(elt) * mori_sizeof_elt(type);
        if (ALTREP(elt) && DATAPTR_OR_NULL(elt) == NULL) {
          /* a foreign-admitted ALTREP leaf copies through *_GET_REGION —
             the sender's vector stays compact (morh_write's branch) */
          switch (type) {
          case LGLSXP:
          case INTSXP:
            INTEGER_GET_REGION(elt, 0, XLENGTH(elt), (int *) (base + cur));
            break;
          case REALSXP:
            REAL_GET_REGION(elt, 0, XLENGTH(elt), (double *) (base + cur));
            break;
          case CPLXSXP:
            COMPLEX_GET_REGION(elt, 0, XLENGTH(elt), (Rcomplex *) (base + cur));
            break;
          default:
            RAW_GET_REGION(elt, 0, XLENGTH(elt), (Rbyte *) (base + cur));
            break;
          }
        } else {
          memcpy(base + cur, DATAPTR_RO(elt), raw_size);
        }
      }
      if (elt_attrs != R_NilValue && !int64)
        attrs_size = mori_attrs_write(base + cur + raw_size,
                                           elt, elt_attrs);

      entry.sexptype = (int64 ? MORI_TYPE_INT64 : type) |
        (Rf_isS4(elt) ? MORI_ELEM_S4 : 0);
      entry.attrs_size = (int32_t) attrs_size;
      entry.length = (int64_t) XLENGTH(elt);
      entry.data_size = (int64_t) (raw_size + attrs_size);

      UNPROTECT(1);
      cur += MORI_ALIGN64((size_t) entry.data_size);
    } else {
      size_t elt_size = mori_serialize_into(base + cur, elt);
      entry.sexptype = 0;
      entry.attrs_size = 0;
      entry.length = 0;
      entry.data_size = (int64_t) elt_size;
      cur += MORI_ALIGN64(elt_size);
    }

    memcpy(base + MORI_HEADER_SIZE + 32 * (size_t) i, &entry,
           sizeof(mori_elem));
  }

  SEXP list_attrs = PROTECT(mori_get_attrs_for_serialize(x));
  int64_t attrs_offset = (int64_t) cur;
  size_t attrs_size = 0;
  if (list_attrs != R_NilValue)
    attrs_size = mori_attrs_write(base + cur, x, list_attrs);
  cur += MORI_ALIGN64(attrs_size);
  UNPROTECT(1);

  if (foreign) {
    /* The validity tail: a bitmap per NA-capable atomic leaf, built from
       the just-written data and spent only where NAs are present, then
       the n-entry table — a clean run collapses to the header's
       known-NA-free and no tail bytes, but only when every remote leaf
       claims known-NA-free too (the header never speaks for a remote
       column: no bitmap and no count from one). */
    int64_t *tab = (int64_t *) malloc(16 * (size_t) n);
    if (tab == NULL) Rf_error("mori: allocation failure");
    int64_t total_nulls = 0;
    int any_remote_unknown = 0;
    for (R_xlen_t i = 0; i < n; i++) {
      mori_elem entry;
      memcpy(&entry, base + MORI_HEADER_SIZE + 32 * (size_t) i,
             sizeof(mori_elem));
      int32_t tag = entry.sexptype & ~MORI_ELEM_S4;
      if (tag == MORI_TAG_REF) {
        /* a remote leaf: the {0,0} / {0,-1} claim alone, upgraded per
           the referenced leaf */
        int64_t rlength = 0, rattrs = 0;
        int rna_free = 0;
        int64_t claim = 0;
        SEXP ref_elt = VECTOR_ELT(x, i);
        SEXP id = PROTECT(mori_shm_name(ref_elt));
        if (id != R_NilValue &&
            mori_ref_descriptor(ref_elt, id, &rlength, &rattrs,
                                     &rna_free) == 0 && rna_free)
          claim = -1;
        UNPROTECT(1);
        tab[2 * i] = 0;
        tab[2 * i + 1] = claim;
        if (claim != -1) any_remote_unknown = 1;
        continue;
      }
      if (mori_sizeof_elt(tag) == 0) {
        tab[2 * i] = 0;   /* VEC / STR / serialized: the nested header or
                             the string block carries its own */
        tab[2 * i + 1] = 0;
        continue;
      }
      if (tag == RAWSXP) {
        tab[2 * i] = 0;   /* no missing sentinel: known-NA-free */
        tab[2 * i + 1] = -1;
        continue;
      }
      size_t off = MORI_ALIGN64(cur);
      int64_t nulls = mori_na_present(
        tag, base + entry.data_offset, (uint64_t) entry.length) ?
        (int64_t) mori_na_build(
          tag, base + off, base + entry.data_offset,
          (uint64_t) entry.length, 0) :
        0;
      if (nulls > 0) {
        tab[2 * i] = (int64_t) off;
        tab[2 * i + 1] = nulls;
        total_nulls += nulls;
        cur = off + ((size_t) entry.length + 7) / 8;
      } else {
        tab[2 * i] = 0;
        tab[2 * i + 1] = -1;
      }
    }
    if (total_nulls > 0 || any_remote_unknown) {
      size_t tab_off = MORI_ALIGN64(cur);
      memcpy(base + tab_off, tab, 16 * (size_t) n);
      mori_morh_validity_set(base, (int64_t) tab_off, total_nulls);
      cur = tab_off + 16 * (size_t) n;
    } else {
      mori_morh_validity_set(base, 0, -1);
    }
    free(tab);
  }

  /* Write header */
  uint32_t magic = MORI_MAGIC_LIST;
  int32_t n32 = (int32_t) n;
  int64_t as64 = (int64_t) attrs_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &n32, 4);
  memcpy(base + 8, &attrs_offset, 8);
  memcpy(base + 16, &as64, 8);

  return cur;
}

/* Format a byte count as a human-readable size (1024-based). */
static void mori_format_bytes(size_t n, char *buf, size_t buflen) {
  static const char *unit[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
  if (n < 1024) {
    snprintf(buf, buflen, "%llu bytes", (unsigned long long) n);
    return;
  }
  double v = (double) n;
  int u = 0;
  while (v >= 1024.0 && u < 5) {
    v /= 1024.0;
    u++;
  }
  snprintf(buf, buflen, "%.1f %s", v, unit[u]);
}

/* Raise an R error for an SHM creation failure, composing the requested size
   with the category's summary and optional remediation hint. */
static void mori_shm_create_failed(int category, size_t requested) {
  char sizebuf[32];
  const char *summary, *hint;
  mori_format_bytes(requested, sizebuf, sizeof(sizebuf));
  mori_err_describe(category, &summary, &hint);
  Rf_error("mori: cannot create region (requested %s): %s%s%s",
           sizebuf, summary, hint[0] != '\0' ? ". " : "", hint);
}

/* MORH layout size: 64-byte header + data + attrs, plus the foreign
   staging mode's validity reserve (the write spends the bitmap bytes
   only when NAs are present, so the write returns at most this). */
static size_t morh_size(SEXP x, int foreign) {
  size_t data_size = (size_t) XLENGTH(x) * mori_sizeof_elt(TYPEOF(x));
  size_t total = MORI_HEADER_SIZE + data_size;
  /* class-only integer64: the wire tag carries the class — no attrs
     section (morh_write gates identically; the two must agree) */
  if (!mori_is_int64_any(x)) {
    SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
    if (attrs != R_NilValue)
      total += mori_attrs_size(x, attrs);
    UNPROTECT(1);
  }
  if (foreign && TYPEOF(x) != RAWSXP)
    total = MORI_ALIGN64(total) + ((size_t) XLENGTH(x) + 7) / 8;
  return total;
}

/* MORH write: header (reserved bytes zeroed) + bare data + attrs, then
   the foreign staging mode's validity section — the bitmap built from
   the just-written data, spent only when NAs are present (a clean vector
   stamps known-NA-free); the returned total is the actual bytes used.
   Header fields are written last, once the counted attr write reports
   its size. An ALTREP with no readable pointer (a top-level ALTREP
   atomic admitted on a foreign handle, zc.c's baseline) copies through
   *_GET_REGION — never expanded on the sender. */
/* The validity section's cheap gate: any NA at all (early exit). Most
   vectors are NA-free — the bitmap build's per-element read-modify-write
   then collapses to the {0, -1} stamp, and the gate is a plain read scan
   the compiler vectorizes where the build could not. Only a vector that
   carries an NA pays the build. */
static int mori_na_present(int type, const void *src, uint64_t n) {
  switch (type) {
  case LGLSXP:
  case INTSXP: {
    const int32_t *v = (const int32_t *) src;
    for (uint64_t i = 0; i < n; i++)
      if (v[i] == MORI_NA_INT32) return 1;
    return 0;
  }
  case REALSXP: {
    const uint64_t *v = (const uint64_t *) src;
    for (uint64_t i = 0; i < n; i++)
      /* one shift + compare in the hot loop: a NaN or Inf exponent
         gates the precise payload test */
      if ((v[i] >> 52) == 0x7FF && mori_ext_na_real_bits(v[i])) return 1;
    return 0;
  }
  case CPLXSXP: {
    const uint64_t *v = (const uint64_t *) src;
    for (uint64_t i = 0; i < 2 * n; i++)
      if ((v[i] >> 52) == 0x7FF && mori_ext_na_real_bits(v[i])) return 1;
    return 0;
  }
  case MORI_TYPE_INT64: {
    const int64_t *v = (const int64_t *) src;
    for (uint64_t i = 0; i < n; i++)
      if (v[i] == MORI_NA_INT64) return 1;
    return 0;
  }
  default:
    return 0;
  }
}

static size_t morh_write(unsigned char *base, SEXP x, int foreign) {

  int int64 = mori_is_int64_any(x);
  int type = TYPEOF(x);
  R_xlen_t n = XLENGTH(x);
  size_t data_size = (size_t) n * mori_sizeof_elt(type);

  memset(base, 0, MORI_HEADER_SIZE);
  if (ALTREP(x) && DATAPTR_OR_NULL(x) == NULL) {
    switch (type) {
    case LGLSXP:
    case INTSXP:
      INTEGER_GET_REGION(x, 0, n, (int *) (base + MORI_HEADER_SIZE));
      break;
    case REALSXP:
      REAL_GET_REGION(x, 0, n, (double *) (base + MORI_HEADER_SIZE));
      break;
    case CPLXSXP:
      COMPLEX_GET_REGION(x, 0, n, (Rcomplex *) (base + MORI_HEADER_SIZE));
      break;
    default:
      RAW_GET_REGION(x, 0, n, (Rbyte *) (base + MORI_HEADER_SIZE));
      break;
    }
  } else {
    memcpy(base + MORI_HEADER_SIZE, DATAPTR_RO(x), data_size);
  }

  size_t attrs_size = 0;
  if (!int64) {
    SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
    if (attrs != R_NilValue)
      attrs_size = mori_attrs_write(base + MORI_HEADER_SIZE + data_size,
                                         x, attrs);
    UNPROTECT(1);
  }

  uint32_t magic = MORI_MAGIC_VEC;
  int32_t sexptype = int64 ? (int32_t) MORI_TYPE_INT64 : (int32_t) type;
  int64_t length = (int64_t) n;
  int64_t as64 = (int64_t) attrs_size;
  uint32_t flags = Rf_isS4(x) ? MORI_FLAG_S4 : 0u;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &sexptype, 4);
  memcpy(base + 8, &length, 8);
  memcpy(base + 16, &as64, 8);
  memcpy(base + MORI_FLAGS_OFF, &flags, 4);

  size_t total = MORI_HEADER_SIZE + data_size + attrs_size;
  if (foreign && type != RAWSXP) {
    size_t off = MORI_ALIGN64(total);
    uint64_t nulls = mori_na_present(
      int64 ? MORI_TYPE_INT64 : type, base + MORI_HEADER_SIZE,
      (uint64_t) n) ?
      mori_na_build(int64 ? MORI_TYPE_INT64 : type, base + off,
                    base + MORI_HEADER_SIZE, (uint64_t) n, 0) :
      0;
    if (nulls > 0) {
      mori_morh_validity_set(base, (int64_t) off, (int64_t) nulls);
      total = off + ((size_t) n + 7) / 8;
    } else {
      mori_morh_validity_set(base, 0, -1);
    }
  }
  return total;
}

/*
 * MORS region layout:
 *   Bytes 0-3:   uint32_t magic (MORI_MAGIC_STR, "MORS")
 *   Bytes 4-7:   int32_t  attrs_size
 *   Bytes 8-15:  int64_t  n (string count)
 *   Bytes 16-23: int64_t  str_size (the string block's byte size)
 *   Bytes 24-31: reserved (zero) — embedder cross-process state
 *   Bytes 32-35: uint32_t flags (bit 0: S4 object bit)
 *   Bytes 36-63: reserved (zero)
 *   Byte 64+:    the string block (mori.h, mori_str_geometry), then
 *                the attrs blob at 64 + str_size
 */

/* MORS layout size: 64-byte header + string block + attrs. */
static size_t mors_size(SEXP x) {
  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? mori_attrs_size(x, attrs) : 0;
  UNPROTECT(1);
  return MORI_HEADER_SIZE + mori_string_data_size(x) + attrs_size;
}

/* MORS write: header (reserved bytes zeroed) + string block + attrs. The
   block write reports its exact size, so no separate sizing walk;
   header fields are written last, once both sizes are known. The string
   block carries its own validity bitmap — no header section. */
static void mors_write(unsigned char *base, SEXP x) {

  R_xlen_t n = XLENGTH(x);

  memset(base, 0, MORI_HEADER_SIZE);
  size_t str_size = mori_write_strings(base + MORI_HEADER_SIZE, x);

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = 0;
  if (attrs != R_NilValue)
    attrs_size = mori_attrs_write(base + MORI_HEADER_SIZE + str_size,
                                       x, attrs);

  uint32_t magic = MORI_MAGIC_STR;
  int32_t as32 = (int32_t) attrs_size;
  int64_t n64 = (int64_t) n;
  int64_t sd = (int64_t) str_size;
  uint32_t flags = Rf_isS4(x) ? MORI_FLAG_S4 : 0u;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &as32, 4);
  memcpy(base + 8, &n64, 8);
  memcpy(base + 16, &sd, 8);
  memcpy(base + MORI_FLAGS_OFF, &flags, 4);

  UNPROTECT(1);
}

/* Shared size dispatcher for the host (mori_create) and embedder
   (mori_layout_size) paths. ok == NULL vets nothing — the host path
   materializes foreign ALTREPs through DATAPTR_RO at write. The embedder
   oracle passes &ok: the root is vetted here and every descendant inside
   mori_nested_size (list trees recurse; everything else is a leaf), and
   the first rejection sets *ok = 0 and yields a 0 return; the foreign
   staging mode relaxes the ALTREP rejection (the write copies through
   *_GET_REGION). Non-layoutable types also return 0 — never ambiguous:
   every region opens with a 64-byte header. */
static size_t mori_layout_size_impl(SEXP x, int *ok, int foreign) {
  if (ok != NULL && !foreign) {
    if (ALTREP(x) && !mori_view_check(x) && !mori_altrep_readable(x)) {
      *ok = 0; return 0;
    }
  }
  int type = TYPEOF(x);
  /* An S4 pairlist root passes through: VECSXP coercion drops the bit. */
  if (type == LISTSXP && Rf_isS4(x)) return 0;
  if (type == VECSXP || type == LISTSXP) {
    if (type == LISTSXP) {
      x = PROTECT(Rf_coerceVector(x, VECSXP));
    } else {
      PROTECT(x);
    }
    size_t total = mori_nested_size(x, ok, foreign);
    UNPROTECT(1);
    return total;
  }
  if (type == STRSXP) return mors_size(x);
  if (mori_shm_eligible(type)) return morh_size(x, foreign);
  return 0;
}

size_t mori_layout_size(SEXP x, int foreign) {
  int ok = 1;
  return mori_layout_size_impl(x, &ok, foreign);
}

size_t mori_layout_write(unsigned char *base, SEXP x, int foreign) {
  int type = TYPEOF(x);
  if (type == VECSXP || type == LISTSXP) {
    if (type == LISTSXP) {
      x = PROTECT(Rf_coerceVector(x, VECSXP));
    } else {
      PROTECT(x);
    }
    size_t total = mori_nested_write(base, x, foreign);
    UNPROTECT(1);
    return total;
  }
  if (type == STRSXP) {
    mors_write(base, x);
    int32_t as32;
    int64_t sd;
    memcpy(&as32, base + 4, 4);
    memcpy(&sd, base + 16, 8);
    return MORI_HEADER_SIZE + (size_t) sd + (size_t) as32;
  }
  return morh_write(base, x, foreign);
}

/* Unified entry point: existing views return unchanged (idempotent);
   layoutable types size, allocate, and write through the shared layout
   dispatchers; a 0 size means pass-through. A LISTSXP root is coerced once
   per dispatcher — pairlist roots are rare enough to pay that for a single
   dispatch. */
SEXP mori_create(SEXP x) {
  if (mori_view_check(x)) return x;

  size_t total = mori_layout_size_impl(x, NULL, 0);
  if (total == 0) return x;

  mori_shm *shm;
  int rc = mori_shm_create_heap(&shm, total);
  if (rc) mori_shm_create_failed(rc, total);

  mori_layout_write((unsigned char *) shm->addr, x, 0);

  return mori_make_result(shm);
}

// .Call entry points: daemon-side SHM open and wrap --------------------------

/* All three open_* take an already-wrapped shm_ptr (shm_tag extptr) that the
   caller has PROTECTed. The shm_ptr flows through as the keeper for the
   returned ALTREP's data1 extptr, pinning the mapping to its lifetime. */

static SEXP mori_open_list(SEXP shm_ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  return mori_list_wrap(
    (unsigned char *) shm->addr, (int64_t) shm->size, -1, shm_ptr, NULL, NULL
  );
}

static SEXP mori_open_vector(SEXP shm_ptr) {

  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;
  int32_t sexptype;
  int64_t length, attrs_size;
  memcpy(&sexptype, base + 4, 4);
  memcpy(&length, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  /* Validate the header against the mapped size before any data access:
     the data block and trailing attributes must fit within the region.
     Short-circuit order keeps the length * elt_size product overflow-free. */
  int64_t region_size = (int64_t) shm->size;
  size_t elt_size = mori_sizeof_elt(sexptype);
  if (region_size < MORI_HEADER_SIZE || length < 0 || attrs_size < 0 ||
      (elt_size != 0 &&
       length > (region_size - MORI_HEADER_SIZE) / (int64_t) elt_size) ||
      attrs_size > region_size - MORI_HEADER_SIZE - length * (int64_t) elt_size ||
      !mori_flags_known(base))
    Rf_error("mori: invalid or corrupted shared memory region");

  SEXP result = PROTECT(mori_vec_wrap(
    base + MORI_HEADER_SIZE, (R_xlen_t) length, sexptype, shm_ptr, NULL, NULL
  ));

  if (attrs_size > 0) {
    size_t data_bytes = (size_t) length * mori_sizeof_elt(sexptype);
    mori_restore_attrs(result, base + MORI_HEADER_SIZE + data_bytes,
                       (size_t) attrs_size);
  }
  result = mori_apply_s4(result, base);

  UNPROTECT(1);
  return result;
}

static SEXP mori_open_string(SEXP shm_ptr) {

  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;
  int32_t attrs_size;
  int64_t n, str_data_size;
  memcpy(&attrs_size, base + 4, 4);
  memcpy(&n, base + 8, 8);
  memcpy(&str_data_size, base + 16, 8);

  /* Validate the header against the mapped size before any data access:
     the string data and trailing attributes must fit within the region. */
  int64_t region_size = (int64_t) shm->size;
  if (region_size < MORI_HEADER_SIZE || n < 0 || str_data_size < 0 ||
      attrs_size < 0 ||
      str_data_size > region_size - MORI_HEADER_SIZE ||
      attrs_size > region_size - MORI_HEADER_SIZE - str_data_size ||
      !mori_flags_known(base))
    Rf_error("mori: invalid or corrupted shared memory region");

  SEXP result = PROTECT(mori_str_wrap(
    base + MORI_HEADER_SIZE, (R_xlen_t) n, str_data_size, shm_ptr, NULL, NULL
  ));

  if (attrs_size > 0)
    mori_restore_attrs(result, base + MORI_HEADER_SIZE + (size_t) str_data_size,
                       (size_t) attrs_size);
  result = mori_apply_s4(result, base);

  UNPROTECT(1);
  return result;
}

/* Dispatch on magic bytes and wrap in the appropriate ALTREP class.
   On unknown magic, errors — GC cleans up shm_ptr's finalizer after longjmp.
   err_name is used in the error message ("" if caller has no name to report). */
static SEXP mori_dispatch_by_magic(SEXP shm_ptr, const char *err_name) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;
  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic == MORI_MAGIC_LIST) return mori_open_list(shm_ptr);
  if (magic == MORI_MAGIC_VEC) return mori_open_vector(shm_ptr);
  if (magic == MORI_MAGIC_STR) return mori_open_string(shm_ptr);
  Rf_error("mori: invalid or corrupted shared memory region: '%s'",
           err_name != NULL ? err_name : "");
}

/* Open SHM by name, inspect magic, dispatch to appropriate wrapper.
   Malformed input (wrong type/length, NA, or not a recognized SHM identifier)
   returns NULL silently. A well-formed identifier that fails to open or
   has unexpected magic bytes errors with a specific message. Accepts both
   bare prefix form (root) and bracketed path form (sub-object). */
SEXP mori_shm_open_and_wrap(SEXP name) {

  if (TYPEOF(name) != STRSXP || XLENGTH(name) != 1)
    return R_NilValue;
  SEXP nm_sxp = STRING_ELT(name, 0);
  if (nm_sxp == NA_STRING)
    return R_NilValue;
  return mori_resolve_id(CHAR(nm_sxp), R_NilValue);
}

/* C-level view check: ALTREP with a mori_owned-tagged data1. */
int mori_view_check(SEXP x) {
  if (!ALTREP(x)) return 0;
  SEXP d1 = R_altrep_data1(x);
  return TYPEOF(d1) == EXTPTRSXP &&
    R_ExternalPtrTag(d1) == mori_owned_tag;
}

/* The remote-leaf writer's gate (mori.h): the REF tier's predicate — a
   vector or string view only while unmaterialized (data2 set means
   COW'd), a list view at any time (its data2 is the read-only element
   cache) — plus no local attributes: a remote leaf carries the referenced
   column's own attributes, so a locally attributed view would silently
   downgrade. */
int mori_refable(SEXP x) {
  if (!mori_view_check(x)) return 0;
  if (TYPEOF(x) != VECSXP && R_altrep_data2(x) != R_NilValue) return 0;
  return !ANY_ATTRIB(x);
}

SEXP mori_is_shared(SEXP x) {
  return Rf_ScalarLogical(mori_view_check(x));
}

/* Recover the leaf's per-element index from the ALTREP type. */
static inline int32_t mori_index_of(SEXP x) {
  void *addr = R_ExternalPtrAddr(R_altrep_data1(x));
  switch (TYPEOF(x)) {
  case VECSXP:  return ((mori_list_view *) addr)->index;
  case STRSXP:  return ((mori_str *) addr)->index;
  default:      return ((mori_vec *) addr)->index;
  }
}

SEXP mori_shm_name(SEXP x) {
  if (!ALTREP(x)) return R_NilValue;
  SEXP d1 = R_altrep_data1(x);
  if (TYPEOF(d1) != EXTPTRSXP ||
      R_ExternalPtrTag(d1) != mori_owned_tag)
    return R_NilValue;

  char buf[MORI_FORMAT_BUFLEN];
  if (mori_format_chain(R_ExternalPtrProtected(d1), mori_index_of(x),
                        buf, sizeof(buf)) != 0)
    return R_NilValue;
  return Rf_mkString(buf);
}

/* Unlink shared memory regions by name, or (name == R_NilValue) reap regions
   orphaned by a dead creating process. Returns the character vector of region
   names actually removed, or R_NilValue if nothing was removed — including the
   reap path on platforms that cannot enumerate the SHM namespace (macOS,
   Windows). Only names matching the view identifier grammar are ever unlinked,
   so unrelated names are silently ignored; a bracketed sub-object path
   resolves to its underlying region. */
SEXP mori_prune(void) {
  int n = 0;
  char **list = mori_shm_reap(&n);
  if (n == 0) return R_NilValue;                  /* list is NULL when n == 0 */
  SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
  for (int i = 0; i < n; i++) {
    SET_STRING_ELT(out, i, Rf_mkChar(list[i]));
    free(list[i]);
  }
  free(list);
  UNPROTECT(1);
  return out;
}

// ALTREP serialization hooks --------------------------------------------------

/* All three Serialized_state methods emit the same string form as
   mori_shm_name (the .Call): bare prefix for root standalones, prefix +
   bracketed 1-based path for sub-objects. mori_format_chain is the single
   source of truth shared with mori_shm_name. On overflow / malformed
   chain, fall back to materialization. The string class wraps materialized
   states in a length-1 VECSXP (see mori_wrap_string_state) so that a bare
   STRSXP state is always an identifier. */

static SEXP mori_vec_Serialized_state(SEXP x) {
  SEXP data2 = R_altrep_data2(x);
  if (data2 != R_NilValue) return data2;  /* COW-materialized copy */

  SEXP data1 = R_altrep_data1(x);
  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(data1);

  char buf[MORI_FORMAT_BUFLEN];
  if (mori_format_chain(R_ExternalPtrProtected(data1), v->index,
                        buf, sizeof(buf)) == 0) {
    if (mori_emit_hook != NULL) mori_emit_hook(x);
    return Rf_mkString(buf);
  }

  /* Overflow / malformed chain: materialize. */
  R_xlen_t n = v->length;
  int type = TYPEOF(x);
  SEXP mat = PROTECT(Rf_allocVector(type, n));
  memcpy(mori_data_ptr(mat), v->data, (size_t) n * mori_sizeof_elt(type));
  UNPROTECT(1);
  return mat;
}

/* Wrap a materialized string vector in a length-1 VECSXP so the wire form
   is unambiguous: a bare STRSXP state is always an SHM identifier, and the
   materialized data — whose content could itself look like an identifier —
   is never mistaken for one. The fallback paths are cold (COW or nesting
   beyond MORI_MAX_PATH), so the wrapper costs nothing on the hot path. */
static SEXP mori_wrap_string_state(SEXP x) {
  SEXP state = PROTECT(Rf_allocVector(VECSXP, 1));
  SET_VECTOR_ELT(state, 0, x);
  UNPROTECT(1);
  return state;
}

static SEXP mori_string_Serialized_state(SEXP x) {
  SEXP data2 = R_altrep_data2(x);
  if (data2 != R_NilValue) return mori_wrap_string_state(data2);

  SEXP data1 = R_altrep_data1(x);
  mori_str *s = (mori_str *) R_ExternalPtrAddr(data1);

  char buf[MORI_FORMAT_BUFLEN];
  if (mori_format_chain(R_ExternalPtrProtected(data1), s->index,
                        buf, sizeof(buf)) == 0) {
    if (mori_emit_hook != NULL) mori_emit_hook(x);
    return Rf_mkString(buf);
  }

  /* Overflow / malformed chain: materialize. */
  R_xlen_t n = s->length;
  SEXP mat = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(mat, i, mori_string_elt_shm(s, i));
  SEXP state = mori_wrap_string_state(mat);
  UNPROTECT(1);
  return state;
}

static SEXP mori_list_Serialized_state(SEXP x) {
  SEXP data1 = R_altrep_data1(x);
  if (R_ExternalPtrTag(data1) != mori_owned_tag)
    return Rf_allocVector(VECSXP, 0);

  mori_list_view *view = (mori_list_view *) R_ExternalPtrAddr(data1);
  if (view == NULL) return Rf_allocVector(VECSXP, 0);

  char buf[MORI_FORMAT_BUFLEN];
  if (mori_format_chain(R_ExternalPtrProtected(data1), view->index,
                        buf, sizeof(buf)) == 0) {
    if (mori_emit_hook != NULL) mori_emit_hook(x);
    return Rf_mkString(buf);
  }

  /* Overflow / malformed chain: materialize. */
  R_xlen_t n = view->n_elements;
  SEXP mat = PROTECT(Rf_allocVector(VECSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_VECTOR_ELT(mat, i, mori_list_Elt(x, i));
  DUPLICATE_ATTRIB(mat, x);
  UNPROTECT(1);
  return mat;
}

/* Walk a path over an already-open region, returning the leaf element.
   path has length >= 1. Intermediate steps must be VECSXP children
   (nested MORL regions); the final step is the leaf. keeper anchors the
   chain (the caller's region wrap — shm_ptr below, or an embedder's). */
SEXP mori_walk_path(unsigned char *base, int64_t region_size,
                    const int32_t *path, int path_len, SEXP keeper) {

  /* Root view (index = -1): uniform chain shape with mori_open_list. */
  SEXP root_view = PROTECT(mori_make_extptr(
    base, region_size, -1, keeper, NULL, NULL
  ));
  mori_list_view *rv = (mori_list_view *) R_ExternalPtrAddr(root_view);

  SEXP cur_keeper = root_view;
  unsigned char *cur_base = rv->base;
  int64_t cur_region_size = rv->region_size;
  int32_t cur_n = rv->n_elements;

  PROTECT_INDEX child_idx;
  SEXP child = R_NilValue;
  PROTECT_WITH_INDEX(child, &child_idx);

  for (int k = 0; k < path_len - 1; k++) {
    int32_t idx = path[k];
    if (idx < 0 || idx >= cur_n)
      Rf_error("mori: path index out of bounds");

    unsigned char *dir = cur_base + MORI_HEADER_SIZE + 32 * (size_t) idx;
    mori_elem entry;
    memcpy(&entry, dir, sizeof(mori_elem));
    int64_t data_offset = entry.data_offset, data_size = entry.data_size;
    int32_t sexptype = entry.sexptype & ~MORI_ELEM_S4;

    if (sexptype != VECSXP)
      Rf_error("mori: path step is not a nested list");
    if (mori_oob(data_offset, data_size, cur_region_size))
      Rf_error("mori: invalid nested region");

    /* Bare extptr: no ALTLIST wrapper, no attr restore (intermediate is
       never observed; only its index in the keeper chain matters). */
    REPROTECT(child = mori_make_extptr(
      cur_base + data_offset, data_size, idx, cur_keeper, NULL, NULL
    ), child_idx);
    cur_keeper = child;

    mori_list_view *cv = (mori_list_view *) R_ExternalPtrAddr(child);
    cur_base = cv->base;
    cur_region_size = cv->region_size;
    cur_n = cv->n_elements;
  }

  int32_t leaf_idx = path[path_len - 1];
  if (leaf_idx < 0 || leaf_idx >= cur_n)
    Rf_error("mori: leaf index out of bounds");

  SEXP result = mori_unwrap_element(cur_base, cur_region_size,
                                    leaf_idx, cur_keeper);
  UNPROTECT(2);
  return result;
}

/* The one resolve path behind every identifier resolve: parse, open (the
   process-global consumer cache, the embedder open hook on a miss), wrap
   the root or walk the path, fire the resolve hook. keeper (R_NilValue
   on every current caller) replaces the cached mapping wrap as the
   view's chain anchor when given — the caller's own composition, which
   must then keep the mapping alive (the default anchor pins it through
   the cache). Returns R_NilValue on a malformed identifier (the caller
   shapes the error); a well-formed identifier whose region is gone
   errors in the open. */
SEXP mori_resolve_id(const char *id, SEXP keeper) {
  char shm_name[MORI_NAME_MAX];
  int32_t path[MORI_MAX_PATH];
  int path_len = 0;
  int rc = mori_parse_id(id, shm_name, sizeof(shm_name), path,
                              &path_len);
  if (rc < 0) return R_NilValue;
  SEXP shm_ptr = PROTECT(mori_open_consumer(shm_name));
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  SEXP anchor = keeper != R_NilValue ? keeper : shm_ptr;
  SEXP result = PROTECT(rc == 0 ?
    mori_dispatch_by_magic(anchor, shm_name) :
    mori_walk_path((unsigned char *) shm->addr, (int64_t) shm->size,
                        path, path_len, anchor));
  if (mori_resolve_hook != NULL) mori_resolve_hook(result, shm);
  UNPROTECT(2);
  return result;
}

// Remote-leaf (MORL directory tag 33) reader ------------------------------------

/* The resolved leaf's descriptor for a bare-name reference: length and
   attrs-blob size off the (already validated) header, and whether the form
   records known-NA-free — an MORH header pair does; a bare MORS root and
   an MORL root do not. */
static void mori_root_descriptor(const unsigned char *base,
                                      int64_t *length, int64_t *attrs,
                                      int *na_free) {
  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic == MORI_MAGIC_VEC) {
    int64_t voff, vcount;
    memcpy(length, base + 8, 8);
    memcpy(attrs, base + 16, 8);
    memcpy(&voff, base + MORI_HDR_VALID_OFF, 8);
    memcpy(&vcount, base + MORI_HDR_VALID_COUNT, 8);
    *na_free = voff == 0 && vcount == -1;
  } else if (magic == MORI_MAGIC_STR) {
    int32_t attrs32;
    memcpy(&attrs32, base + 4, 4);
    memcpy(length, base + 8, 8);
    *attrs = attrs32;
    *na_free = 0;
  } else {   /* MORI_MAGIC_LIST */
    int32_t n;
    memcpy(&n, base + 4, 4);
    memcpy(attrs, base + 16, 8);
    *length = n;
    *na_free = 0;
  }
}

/* The resolved leaf's descriptor for a path reference: the terminal
   directory entry through the vendored mori_morl_elem — the view layer's
   one read of MORL leaf validity (R reads NA in-band otherwise). The
   walked structure is trusted: the view-producing walk just validated it
   over the same pages. A known-NA-free record exists only on an atomic
   leaf's pair or a chained remote leaf's — STR, serialized and nested-list
   entries record none. */
static void mori_path_descriptor(unsigned char *base, int64_t region_size,
                                      const int32_t *path, int path_len,
                                      int64_t *length, int64_t *attrs,
                                      int *na_free) {
  unsigned char *cur = base;
  int64_t cur_size = region_size;
  for (int k = 0; k < path_len - 1; k++) {
    mori_elem step;
    memcpy(&step, cur + MORI_HEADER_SIZE + 32 * (size_t) path[k],
           sizeof(step));
    cur += step.data_offset;
    cur_size = step.data_size;
  }
  mori_morl_entry ent;
  if (mori_morl_elem(cur, (size_t) cur_size, path[path_len - 1], &ent) != 0)
    Rf_error("mori: invalid MORL remote leaf — corrupt or newer region");
  int32_t tag = ent.sexptype & ~(int32_t) MORI_MORL_S4;
  *length = ent.length;
  *attrs = ent.attrs_size;
  *na_free = ent.valid[0] == 0 && ent.valid[1] == -1 &&
    (mori_type_elt_size(tag) != 0 || tag == MORI_TAG_REF);
}

/* A REF-able view's referenced-leaf descriptor for the remote-leaf write
   (F2.5): the view's identifier resolved against its own region — the
   chain terminus's MORH/MORS/MORL header for a bare name, the terminal
   directory entry for a path. -1 when the identifier or the terminus is
   broken (the caller's copy path takes the element). */
static int mori_ref_descriptor(SEXP elt, SEXP id, int64_t *length,
                                    int64_t *attrs, int *na_free) {
  char shm_name[MORI_NAME_MAX];
  int32_t path[MORI_MAX_PATH];
  int path_len = 0;
  int rc = mori_parse_id(CHAR(STRING_ELT(id, 0)), shm_name,
                              sizeof(shm_name), path, &path_len);
  if (rc < 0) return -1;
  SEXP hop = R_ExternalPtrProtected(R_altrep_data1(elt));
  while (TYPEOF(hop) == EXTPTRSXP && R_ExternalPtrTag(hop) != mori_shm_tag)
    hop = R_ExternalPtrProtected(hop);
  if (TYPEOF(hop) != EXTPTRSXP) return -1;
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(hop);
  if (shm == NULL || shm->addr == NULL) return -1;
  if (rc == 0)
    mori_root_descriptor((unsigned char *) shm->addr, length, attrs,
                              na_free);
  else
    mori_path_descriptor((unsigned char *) shm->addr, (int64_t) shm->size,
                              path, path_len, length, attrs, na_free);
  return 0;
}

/* The remote-leaf reader: the column lives in another region and crosses
   by reference — the entry's span is the identifier, and its length /
   attrs_size / validity claim describe the referenced column as resolved.
   Resolves through the one path (parse, the consumer-mapping cache, magic
   dispatch or the path walk), validates the claims against the resolved
   leaf, and only then fires the resolve hook (the counted add — a decline
   holds no transient count). The resolved view chains to the
   consumer-mapping wrap, not the parent: its region is not the parent's.
   Every decline is the corrupt-or-newer shape — behind the capability
   gate, meeting one unadvertised is exactly that. */
static SEXP mori_unwrap_remote(unsigned char *base, int64_t region_size,
                                    int32_t index, int s4,
                                    int64_t data_offset, int64_t data_size,
                                    int64_t length, int32_t attrs_size) {

  /* The core's tag-33 entry branch (mori_ext_morl_ent), restated: the S4
     bit clear, the identifier span 1-255 bytes, the length non-negative. */
  if (s4 || length < 0 || data_size < 1 || data_size > 255)
    Rf_error("mori: invalid MORL remote leaf — corrupt or newer region");

  /* The entry's own NA claim: the header pair, or its validity-table row.
     A bitmap offset is region-local and cannot describe a remote column —
     the row is the {0,0} / {0,-1} claim alone. */
  int64_t voff, vcount;
  memcpy(&voff, base + MORI_HDR_VALID_OFF, 8);
  memcpy(&vcount, base + MORI_HDR_VALID_COUNT, 8);
  int64_t claim = 0;
  if (voff == 0) {
    if (vcount != 0 && vcount != -1)
      Rf_error("mori: invalid MORL remote leaf — corrupt or newer region");
    claim = vcount;
  } else {
    int32_t n;
    memcpy(&n, base + 4, 4);
    if ((voff & 63) != 0 ||
        mori_oob(voff, 16 * (int64_t) n, region_size))
      Rf_error("mori: invalid MORL remote leaf — corrupt or newer region");
    int64_t loff, lcount;
    memcpy(&loff, base + voff + 16 * (size_t) index, 8);
    memcpy(&lcount, base + voff + 16 * (size_t) index + 8, 8);
    if (loff != 0 || (lcount != 0 && lcount != -1))
      Rf_error("mori: invalid MORL remote leaf — corrupt or newer region");
    claim = lcount;
  }

  char id[256];
  memcpy(id, base + data_offset, (size_t) data_size);
  id[data_size] = '\0';

  char shm_name[MORI_NAME_MAX];
  int32_t path[MORI_MAX_PATH];
  int path_len = 0;
  int rc = mori_parse_id(id, shm_name, sizeof(shm_name), path, &path_len);
  if (rc < 0)
    Rf_error("mori: invalid MORL remote leaf — corrupt or newer region");

  SEXP shm_ptr = PROTECT(mori_open_consumer_or_null(shm_name));
  if (shm_ptr == R_NilValue)
    Rf_error("mori: invalid MORL remote leaf — referenced region not found: '%s'",
             shm_name);
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);

  int64_t rlength = 0, rattrs = 0;
  int rna_free = 0;
  SEXP result = PROTECT(rc == 0 ?
    mori_dispatch_by_magic(shm_ptr, shm_name) :
    mori_walk_path((unsigned char *) shm->addr, (int64_t) shm->size,
                        path, path_len, shm_ptr));
  if (rc == 0)
    mori_root_descriptor((unsigned char *) shm->addr, &rlength, &rattrs,
                              &rna_free);
  else
    mori_path_descriptor((unsigned char *) shm->addr, (int64_t) shm->size,
                              path, path_len, &rlength, &rattrs, &rna_free);

  if (rlength != length || rattrs != (int64_t) attrs_size ||
      (claim == -1 && !rna_free))
    Rf_error("mori: invalid MORL remote leaf — claims do not match the referenced region");

  if (mori_resolve_hook != NULL) mori_resolve_hook(result, shm);
  UNPROTECT(2);
  return result;
}

/* Fire a view's armed release record now, ahead of the GC: the
   once-only discipline (a no-op when the view is not a view, unarmed, or
   already fired — a COW-materialized view fired at materialization). The
   deterministic-release paths (the F1 worker's argument-loan release)
   call it only on views provably dead to the R side from here — the
   release subs the region's count while the pages stay mapped, so a live
   reader of the same pages must hold its own count. */
void mori_release_now(SEXP x) {
  if (!mori_view_check(x)) return;
  mori_owned *o = (mori_owned *) R_ExternalPtrAddr(R_altrep_data1(x));
  if (o != NULL) mori_release_once(o);
}

/* Vec and list classes: a STRSXP state is always an SHM identifier (their
   materialized fallbacks are atomic vectors or VECSXP, never STRSXP), so a
   probe miss means a corrupt stream. A well-formed identifier whose region
   is gone errors inside mori_shm_open_and_wrap (corruption, not user
   data). Any other state is materialized data, returned as-is (R restores
   ALTREP attributes separately). The string class has its own method
   below, since both of its wire forms are string-like. */
static SEXP mori_Unserialize(SEXP class_info, SEXP state) {
  (void) class_info;
  if (TYPEOF(state) == STRSXP) {
    SEXP opened = mori_shm_open_and_wrap(state);
    if (opened != R_NilValue) return opened;
    Rf_error("mori: invalid serialized state for a shared object");
  }
  return state;
}

/* String class: the identifier form is a bare STRSXP; the materialized
   fallback is wrapped in a length-1 VECSXP (see mori_wrap_string_state).
   The forms are disjoint by construction, so anything else is a corrupt
   stream. */
static SEXP mori_string_Unserialize(SEXP class_info, SEXP state) {
  (void) class_info;
  if (TYPEOF(state) == VECSXP && XLENGTH(state) == 1 &&
      TYPEOF(VECTOR_ELT(state, 0)) == STRSXP)
    return VECTOR_ELT(state, 0);
  if (TYPEOF(state) == STRSXP) {
    SEXP opened = mori_shm_open_and_wrap(state);
    if (opened != R_NilValue) return opened;
  }
  Rf_error("mori: invalid serialized state for a shared string vector");
}

// ALTREP class registration ---------------------------------------------------

typedef R_altrep_class_t (*mori_make_class_fn)(const char *, const char *,
                                               DllInfo *);

/* Register one of the 5 atomic ALTREP vector classes, all sharing the same
   method set (mori_vec_*). */
static R_altrep_class_t mori_register_vec_class(mori_make_class_fn make,
                                                const char *name,
                                                DllInfo *dll) {
  R_altrep_class_t cls = make(name, "mori", dll);
  R_set_altrep_Length_method(cls, mori_vec_Length);
  R_set_altvec_Dataptr_method(cls, mori_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(cls, mori_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(cls, mori_vec_Serialized_state);
  R_set_altrep_Unserialize_method(cls, mori_Unserialize);
  return cls;
}

void mori_altrep_init(DllInfo *dll) {

  mori_shm_tag = Rf_install(MORI_TAG_SHM);
  mori_host_tag = Rf_install(MORI_TAG_HOST);
  mori_owned_tag = Rf_install(MORI_TAG_OWNED);

  /* the integer64 class singleton: a STRSXP does not self-root like an
     interned tag, so preserve it explicitly (the layer has no fini) */
  mori_int64_class = Rf_mkString("integer64");
  R_PreserveObject(mori_int64_class);

  /* the consumer-mapping cache's wraps vector (fresh VECSXP is NIL-filled;
     a name_len of 0 marks an empty slot) */
  mori_cache_wraps = Rf_allocVector(VECSXP, MORI_CACHE_MAX);
  R_PreserveObject(mori_cache_wraps);

  /* ALTLIST class */
  mori_list_class = R_make_altlist_class("mori_list", "mori", dll);
  R_set_altrep_Length_method(mori_list_class, mori_list_Length);
  R_set_altrep_Duplicate_method(mori_list_class, mori_list_Duplicate);
  R_set_altvec_Dataptr_method(mori_list_class, mori_list_Dataptr);
  R_set_altvec_Dataptr_or_null_method(mori_list_class,
                                      mori_list_Dataptr_or_null);
  R_set_altlist_Elt_method(mori_list_class, mori_list_Elt);
  R_set_altrep_Serialized_state_method(mori_list_class,
                                       mori_list_Serialized_state);
  R_set_altrep_Unserialize_method(mori_list_class, mori_Unserialize);

  /* ALTREP atomic vector classes (all share the same mori_vec_* methods) */
  mori_real_class    = mori_register_vec_class(R_make_altreal_class,
                                               "mori_real",    dll);
  mori_integer_class = mori_register_vec_class(R_make_altinteger_class,
                                               "mori_integer", dll);
  mori_logical_class = mori_register_vec_class(R_make_altlogical_class,
                                               "mori_logical", dll);
  mori_raw_class     = mori_register_vec_class(R_make_altraw_class,
                                               "mori_raw",     dll);
  mori_complex_class = mori_register_vec_class(R_make_altcomplex_class,
                                               "mori_complex", dll);

  /* ALTSTRING class */
  mori_string_class = R_make_altstring_class("mori_string", "mori", dll);
  R_set_altrep_Length_method(mori_string_class, mori_string_Length);
  R_set_altrep_Duplicate_method(mori_string_class, mori_string_Duplicate);
  R_set_altvec_Dataptr_method(mori_string_class, mori_string_Dataptr);
  R_set_altvec_Dataptr_or_null_method(mori_string_class,
                                      mori_string_Dataptr_or_null);
  R_set_altstring_Elt_method(mori_string_class, mori_string_Elt);
  R_set_altrep_Serialized_state_method(mori_string_class,
                                       mori_string_Serialized_state);
  R_set_altrep_Unserialize_method(mori_string_class, mori_string_Unserialize);
}
