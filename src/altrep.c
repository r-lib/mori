#include <stdlib.h>
#include "mori.h"

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
static SEXP mori_owned_tag;  /* tag on every mori ALTREP data1 extptr; addr type dispatched via TYPEOF(x) */

// Element directory (for list SHM layout) -------------------------------------

typedef struct {
  int64_t data_offset;
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
} mori_elem;

/* ALTSTRING offset table entry (16 bytes per string).
   str_length < 0 sentinel means NA_STRING.
   str_encoding is a cetype_t. */
typedef struct {
  int64_t str_offset;
  int32_t str_length;
  int32_t str_encoding;
} mori_str_entry;

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

static inline void mori_restore_attrs(SEXP result, unsigned char *buf,
                                      size_t size) {
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
static int mori_parse_identifier(const char *s,
                                 char *name_out, size_t name_out_size,
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

static void mori_owned_finalizer(SEXP ptr) {
  void *v = R_ExternalPtrAddr(ptr);
  if (v != NULL) {
    free(v);
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

  return p;
}

/* keeper: SEXP kept alive via the extptr's protected slot (parent SHM). */
static SEXP mori_make_vector(const void *data, R_xlen_t length,
                             int sexptype, SEXP keeper) {

  R_altrep_class_t cls;
  switch (sexptype) {
  case REALSXP:  cls = mori_real_class;    break;
  case INTSXP:   cls = mori_integer_class; break;
  case LGLSXP:   cls = mori_logical_class; break;
  case RAWSXP:   cls = mori_raw_class;     break;
  case CPLXSXP:  cls = mori_complex_class; break;
  default:       Rf_error("mori: unsupported ALTREP type %d", sexptype);
  }

  mori_vec *v = malloc(sizeof(mori_vec));
  if (v == NULL) Rf_error("mori: allocation failure");
  v->data = data;
  v->length = length;
  v->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(v, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  SEXP result = R_new_altrep(cls, ptr, R_NilValue);
  UNPROTECT(1);
  return result;
}

// ALTSTRING methods -----------------------------------------------------------

/*
 * String entry in offset table (16 bytes per string):
 *   str_offset(int64) + str_length(int32) + str_encoding(int32)
 * str_length < 0 means NA_STRING.
 *
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_str *)
 *           protected value keeps the parent SHM extptr alive (shm_tag for
 *           standalone, parent view for element within an ALTLIST)
 *   data2 = R_NilValue while SHM-backed; regular STRSXP after materialization
 */

typedef struct {
  const unsigned char *table;
  const unsigned char *data;
  R_xlen_t length;
  int64_t str_bytes;  /* size of the packed string area; bounds each entry */
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} mori_str;

static inline SEXP mori_string_elt_shm(mori_str *s, R_xlen_t i) {
  mori_str_entry e;
  memcpy(&e, s->table + sizeof(mori_str_entry) * (size_t) i,
         sizeof(mori_str_entry));

  if (e.str_length < 0) return NA_STRING;

  /* Bounds-check the entry against the packed string area before reading */
  if (e.str_offset < 0 ||
      e.str_offset > s->str_bytes - (int64_t) e.str_length ||
      e.str_encoding < CE_NATIVE || e.str_encoding > CE_BYTES)
    Rf_error("mori: invalid string data");

  return Rf_mkCharLenCE((const char *) (s->data + e.str_offset),
                        e.str_length, (cetype_t) e.str_encoding);
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

/* region_base: points to the offset table. data_size: bytes available for
   the table, alignment padding, and packed strings (the caller excludes any
   trailing attributes) — the table must fit within it, and the remainder is
   recorded as str_bytes to bound each offset-table entry at Elt time.
   keeper: SEXP kept alive via the extptr's protected slot (parent SHM). */
static SEXP mori_make_string(const unsigned char *region_base,
                             R_xlen_t n, int64_t data_size, SEXP keeper) {

  if (n < 0 || data_size < 0 ||
      n > data_size / (R_xlen_t) sizeof(mori_str_entry))
    Rf_error("mori: invalid string data");

  size_t table_size = sizeof(mori_str_entry) * (size_t) n;
  size_t aligned = MORI_ALIGN64(table_size);
  if (aligned > (size_t) data_size)
    Rf_error("mori: invalid string data");

  mori_str *s = malloc(sizeof(mori_str));
  if (s == NULL) Rf_error("mori: allocation failure");

  s->table = region_base;
  s->data = region_base + aligned;
  s->length = n;
  s->str_bytes = data_size - (int64_t) aligned;
  s->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(s, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  SEXP result = R_new_altrep(mori_string_class, ptr, R_NilValue);
  UNPROTECT(1);
  return result;
}

// Forward declarations --------------------------------------------------------

static SEXP mori_make_list_view(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper);

// Element extraction helper (shared by list Elt and open_path) ----------------

static SEXP mori_unwrap_element(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper) {

  unsigned char *dir = base + 24 + 32 * (size_t) index;
  mori_elem entry;
  memcpy(&entry, dir, sizeof(mori_elem));
  int64_t data_offset = entry.data_offset, data_size = entry.data_size;
  int64_t length = entry.length;
  int32_t sexptype = entry.sexptype, attrs_size = entry.attrs_size;

  if (mori_oob(data_offset, data_size, region_size))
    Rf_error("mori: invalid element data");
  if (attrs_size < 0 || attrs_size > data_size)
    Rf_error("mori: invalid element data");

  SEXP result;
  if (sexptype == VECSXP) {
    /* Nested MORL at base+data_offset; attrs live inside child region */
    result = PROTECT(mori_make_list_view(
      base + data_offset, data_size, index, keeper
    ));
  } else if (sexptype == STRSXP) {
    result = PROTECT(mori_make_string(
      base + data_offset, (R_xlen_t) length, data_size - attrs_size, keeper
    ));
    ((mori_str *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else if (sexptype != 0) {
    /* The claimed element data must fit within the directory entry's data
       region (minus trailing attributes) */
    size_t elt_size = mori_sizeof_elt(sexptype);
    if (elt_size != 0 &&
        (length < 0 ||
         length > (data_size - attrs_size) / (int64_t) elt_size))
      Rf_error("mori: invalid element data");
    result = PROTECT(mori_make_vector(
      base + data_offset, (R_xlen_t) length, sexptype, keeper
    ));
    ((mori_vec *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else {
    result = PROTECT(mori_unserialize_from(
      base + (size_t) data_offset, (size_t) data_size
    ));
  }

  if (attrs_size > 0 && sexptype != 0 && sexptype != VECSXP) {
    size_t attrs_off = (size_t)(data_offset + data_size - attrs_size);
    mori_restore_attrs(result, base + attrs_off, (size_t) attrs_size);
  }

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
 *   Bytes 0-3:   uint32_t magic (0x4D4F524C "MORL")
 *   Bytes 4-7:   int32_t  n_elements
 *   Bytes 8-15:  int64_t  attrs_offset
 *   Bytes 16-23: int64_t  attrs_size
 *   Byte 24+:    element directory (32 bytes per element)
 */

/* Validate a MORL region and return a freshly allocated owned-tag extptr
   wrapping its mori_list_view. No ALTREP wrapper, no attribute restoration —
   suitable for path-walk intermediates whose ALTLIST view is never observed.
   keeper: parent extptr (shm_tag for root, owned_tag for sub-list).
   out_attrs_offset / out_attrs_size: optional out-params (NULL to skip);
   when non-NULL, populated with the validated attrs locator so the caller
   can avoid re-reading the header.
   Errors cleanly on corrupt input rather than SEGV. */
static SEXP mori_make_view_extptr(unsigned char *base, int64_t region_size,
                                  int32_t index, SEXP keeper,
                                  int64_t *out_attrs_offset,
                                  int64_t *out_attrs_size) {

  if (region_size < 24)
    Rf_error("mori: invalid nested list region");

  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic != 0x4D4F524Cu)
    Rf_error("mori: invalid nested list region");

  int32_t n;
  int64_t attrs_offset, attrs_size;
  memcpy(&n, base + 4, 4);
  memcpy(&attrs_offset, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  if (n < 0 || n > (region_size - 24) / 32 ||
      mori_oob(attrs_offset, attrs_size, region_size))
    Rf_error("mori: invalid nested list region");

  mori_list_view *v = malloc(sizeof(mori_list_view));
  if (v == NULL) Rf_error("mori: allocation failure");
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
static SEXP mori_make_list_view(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper) {

  int64_t attrs_offset, attrs_size;
  SEXP ptr = PROTECT(mori_make_view_extptr(
    base, region_size, index, keeper, &attrs_offset, &attrs_size
  ));

  /* Cache is allocated lazily on first Elt access */
  SEXP result = PROTECT(R_new_altrep(mori_list_class, ptr, R_NilValue));

  if (attrs_size > 0)
    mori_restore_attrs(result, base + (size_t) attrs_offset,
                       (size_t) attrs_size);

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

/* Full materialization fallback */
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

// String write helper (shared by standalone and list paths) -------------------

/* Returns total bytes written (including alignment padding). */
static size_t mori_write_strings(unsigned char *dest, SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t table_size = sizeof(mori_str_entry) * (size_t) n;
  size_t data_start = MORI_ALIGN64(table_size);

  /* Zero-fill alignment gap */
  if (data_start > table_size)
    memset(dest + table_size, 0, data_start - table_size);

  size_t cur = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    unsigned char *tbl = dest + sizeof(mori_str_entry) * (size_t) i;
    mori_str_entry e;

    if (elt == NA_STRING) {
      e.str_offset = 0;
      e.str_length = -1;
      e.str_encoding = 0;
    } else {
      int32_t slen = (int32_t) LENGTH(elt);
      e.str_offset = (int64_t) cur;
      e.str_length = slen;
      e.str_encoding = (int32_t) Rf_getCharCE(elt);
      memcpy(dest + data_start + cur, CHAR(elt), (size_t) slen);
      cur += (size_t) slen;
    }

    memcpy(tbl, &e, sizeof(mori_str_entry));
  }

  return data_start + cur;
}

static size_t mori_string_data_size(SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t table_size = sizeof(mori_str_entry) * (size_t) n;
  size_t str_bytes = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    if (elt != NA_STRING)
      str_bytes += (size_t) LENGTH(elt);
  }
  return MORI_ALIGN64(table_size) + str_bytes;
}

// .Call entry points: host-side SHM creation ---------------------------------

// Recursive size/write helpers for nested list regions -----------------------

static size_t mori_nested_size(SEXP x);
static size_t mori_nested_write(unsigned char *base, SEXP x);

/* Total bytes occupied by a MORL region for VECSXP x, including header,
   directory, elements (recursing into VECSXP/LISTSXP children), trailing
   attrs, and all 64-byte alignment padding. Caller passes a VECSXP; any
   LISTSXP children are coerced locally during recursion. */
static size_t mori_nested_size(SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t total = MORI_ALIGN64(24 + 32 * (size_t) n);

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);
    int type = TYPEOF(elt);
    size_t elt_size;

    if (type == LISTSXP || type == VECSXP) {
      SEXP coerced = (type == LISTSXP) ? Rf_coerceVector(elt, VECSXP) : elt;
      PROTECT(coerced);
      elt_size = mori_nested_size(coerced);
      UNPROTECT(1);
    } else if (mori_shm_eligible(type)) {
      size_t raw_size = (type == STRSXP) ?
        mori_string_data_size(elt) :
        (size_t) XLENGTH(elt) * mori_sizeof_elt(type);
      SEXP elt_attrs = PROTECT(mori_get_attrs_for_serialize(elt));
      size_t attrs_size = (elt_attrs != R_NilValue) ?
        mori_serialize_count(elt_attrs) : 0;
      UNPROTECT(1);
      elt_size = raw_size + attrs_size;
    } else {
      elt_size = mori_serialize_count(elt);
    }

    total += MORI_ALIGN64(elt_size);
  }

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ?
    mori_serialize_count(attrs) : 0;
  UNPROTECT(1);
  total += MORI_ALIGN64(attrs_size);

  return total;
}

/* Writes a complete MORL region for VECSXP x starting at base. Returns
   total bytes written (must equal mori_nested_size(x)). */
static size_t mori_nested_write(unsigned char *base, SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t cur = MORI_ALIGN64(24 + 32 * (size_t) n);

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);
    int type = TYPEOF(elt);
    mori_elem entry;
    entry.data_offset = (int64_t) cur;

    if (type == LISTSXP || type == VECSXP) {
      SEXP coerced = (type == LISTSXP) ? Rf_coerceVector(elt, VECSXP) : elt;
      PROTECT(coerced);
      size_t written = mori_nested_write(base + cur, coerced);
      entry.sexptype = VECSXP;
      entry.attrs_size = 0;
      entry.length = (int64_t) XLENGTH(coerced);
      entry.data_size = (int64_t) written;
      UNPROTECT(1);
      cur += MORI_ALIGN64(written);
    } else if (mori_shm_eligible(type)) {
      size_t raw_size = (type == STRSXP) ?
        mori_string_data_size(elt) :
        (size_t) XLENGTH(elt) * mori_sizeof_elt(type);

      SEXP elt_attrs = PROTECT(mori_get_attrs_for_serialize(elt));
      size_t attrs_size = (elt_attrs != R_NilValue) ?
        mori_serialize_count(elt_attrs) : 0;

      entry.sexptype = type;
      entry.attrs_size = (int32_t) attrs_size;
      entry.length = (int64_t) XLENGTH(elt);
      entry.data_size = (int64_t) (raw_size + attrs_size);

      if (type == STRSXP) {
        size_t written = mori_write_strings(base + cur, elt);
        if (attrs_size > 0)
          mori_serialize_into(base + cur + written, attrs_size, elt_attrs);
      } else {
        memcpy(base + cur, DATAPTR_RO(elt), raw_size);
        if (attrs_size > 0)
          mori_serialize_into(base + cur + raw_size, attrs_size, elt_attrs);
      }

      UNPROTECT(1);
      cur += MORI_ALIGN64((size_t) entry.data_size);
    } else {
      size_t elt_size = mori_serialize_count(elt);
      mori_serialize_into(base + cur, elt_size, elt);
      entry.sexptype = 0;
      entry.attrs_size = 0;
      entry.length = 0;
      entry.data_size = (int64_t) elt_size;
      cur += MORI_ALIGN64(elt_size);
    }

    memcpy(base + 24 + 32 * (size_t) i, &entry, sizeof(mori_elem));
  }

  SEXP list_attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (list_attrs != R_NilValue) ?
    mori_serialize_count(list_attrs) : 0;
  int64_t attrs_offset = (int64_t) cur;
  if (attrs_size > 0)
    mori_serialize_into(base + cur, attrs_size, list_attrs);
  cur += MORI_ALIGN64(attrs_size);
  UNPROTECT(1);

  /* Write header */
  uint32_t magic = 0x4D4F524Cu;
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

/* Write list/data frame to SHM (with transparent nested VECSXP) */
static SEXP mori_shm_create_list_call(SEXP x) {

  /* Coerce top-level pairlists to VECSXP so VECTOR_ELT/XLENGTH work */
  if (TYPEOF(x) == LISTSXP) {
    x = PROTECT(Rf_coerceVector(x, VECSXP));
  } else {
    PROTECT(x);
  }

  size_t total = mori_nested_size(x);

  mori_shm *shm;
  int rc = mori_shm_create_heap(&shm, total);
  if (rc) mori_shm_create_failed(rc, total);

  mori_nested_write((unsigned char *) shm->addr, x);

  UNPROTECT(1);
  return mori_make_result(shm);
}

/* Write atomic vector to SHM: 64-byte header + data (64-byte aligned) + attrs */
static SEXP mori_shm_create_vector_call(SEXP x) {

  int type = TYPEOF(x);
  R_xlen_t n = XLENGTH(x);
  size_t elt_size = mori_sizeof_elt(type);
  size_t data_size = (size_t) n * elt_size;

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? mori_serialize_count(attrs) : 0;
  size_t total = 64 + data_size + attrs_size;

  mori_shm *shm;
  int rc = mori_shm_create_heap(&shm, total);
  if (rc) mori_shm_create_failed(rc, total);

  unsigned char *base = (unsigned char *) shm->addr;

  /* Zero-fill header, then write fields */
  memset(base, 0, 64);
  uint32_t magic = 0x4D4F5248u;
  int32_t sexptype = (int32_t) type;
  int64_t length = (int64_t) n;
  int64_t as64 = (int64_t) attrs_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &sexptype, 4);
  memcpy(base + 8, &length, 8);
  memcpy(base + 16, &as64, 8);

  memcpy(base + 64, DATAPTR_RO(x), data_size);

  if (attrs_size > 0)
    mori_serialize_into(base + 64 + data_size, attrs_size, attrs);

  UNPROTECT(1);
  return mori_make_result(shm);
}

/* Write character vector to SHM: 24-byte header + offset table + strings + attrs */
static SEXP mori_shm_create_string_call(SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t header_size = 24;
  size_t str_size = mori_string_data_size(x);

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? mori_serialize_count(attrs) : 0;
  size_t total = header_size + str_size + attrs_size;

  mori_shm *shm;
  int rc = mori_shm_create_heap(&shm, total);
  if (rc) mori_shm_create_failed(rc, total);

  unsigned char *base = (unsigned char *) shm->addr;

  /* Write header */
  memset(base, 0, header_size);
  uint32_t magic = 0x4D4F5253u;
  int32_t as32 = (int32_t) attrs_size;
  int64_t n64 = (int64_t) n;
  int64_t sd = (int64_t) str_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &as32, 4);
  memcpy(base + 8, &n64, 8);
  memcpy(base + 16, &sd, 8);

  mori_write_strings(base + header_size, x);

  if (attrs_size > 0)
    mori_serialize_into(base + header_size + str_size, attrs_size, attrs);

  UNPROTECT(1);
  return mori_make_result(shm);
}

/* Unified entry point: dispatch by type */
SEXP mori_create(SEXP x) {
  if (ALTREP(x)) {
    SEXP d1 = R_altrep_data1(x);
    if (TYPEOF(d1) == EXTPTRSXP && R_ExternalPtrTag(d1) == mori_owned_tag)
      return x;
  }
  int type = TYPEOF(x);
  if (type == VECSXP || type == LISTSXP)
    return mori_shm_create_list_call(x);
  if (type == STRSXP)
    return mori_shm_create_string_call(x);
  if (mori_shm_eligible(type))
    return mori_shm_create_vector_call(x);
  return x;
}

// .Call entry points: daemon-side SHM open and wrap --------------------------

/* All three open_* take an already-wrapped shm_ptr (shm_tag extptr) that the
   caller has PROTECTed. The shm_ptr flows through as the keeper for the
   returned ALTREP's data1 extptr, pinning the mapping to its lifetime. */

static SEXP mori_open_list(SEXP shm_ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  return mori_make_list_view(
    (unsigned char *) shm->addr, (int64_t) shm->size, -1, shm_ptr
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
  if (region_size < 64 || length < 0 || attrs_size < 0 ||
      (elt_size != 0 && length > (region_size - 64) / (int64_t) elt_size) ||
      attrs_size > region_size - 64 - length * (int64_t) elt_size)
    Rf_error("mori: invalid or corrupted shared memory region");

  SEXP result = PROTECT(mori_make_vector(
    base + 64, (R_xlen_t) length, sexptype, shm_ptr
  ));

  if (attrs_size > 0) {
    size_t data_bytes = (size_t) length * mori_sizeof_elt(sexptype);
    mori_restore_attrs(result, base + 64 + data_bytes, (size_t) attrs_size);
  }

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
  if (region_size < 24 || n < 0 || str_data_size < 0 || attrs_size < 0 ||
      str_data_size > region_size - 24 ||
      attrs_size > region_size - 24 - str_data_size)
    Rf_error("mori: invalid or corrupted shared memory region");

  SEXP result = PROTECT(mori_make_string(
    base + 24, (R_xlen_t) n, str_data_size, shm_ptr
  ));

  if (attrs_size > 0)
    mori_restore_attrs(result, base + 24 + (size_t) str_data_size,
                       (size_t) attrs_size);

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
  if (magic == 0x4D4F524Cu) return mori_open_list(shm_ptr);
  if (magic == 0x4D4F5248u) return mori_open_vector(shm_ptr);
  if (magic == 0x4D4F5253u) return mori_open_string(shm_ptr);
  Rf_error("mori: invalid or corrupted shared memory region: '%s'",
           err_name != NULL ? err_name : "");
}

/* Forward declaration for the path-form branch below. */
static SEXP mori_open_path_c(const char *name,
                             const int32_t *path, int path_len);

/* Open SHM by name, inspect magic, dispatch to appropriate wrapper.
   Malformed input (wrong type/length, NA, or not a mori SHM identifier)
   returns NULL silently. A well-formed identifier that fails to open or
   has unexpected magic bytes errors with a specific message. Accepts both
   bare prefix form (root) and bracketed path form (sub-object). */
SEXP mori_shm_open_and_wrap(SEXP name) {

  if (TYPEOF(name) != STRSXP || XLENGTH(name) != 1)
    return R_NilValue;
  SEXP nm_sxp = STRING_ELT(name, 0);
  if (nm_sxp == NA_STRING)
    return R_NilValue;
  const char *s = CHAR(nm_sxp);

  char shm_name[MORI_NAME_MAX];
  int32_t path[MORI_MAX_PATH];
  int path_len = 0;
  int rc = mori_parse_identifier(s, shm_name, sizeof(shm_name),
                                 path, &path_len);
  if (rc < 0) return R_NilValue;        /* probe miss */

  if (rc == 0) {
    mori_shm *shm = mori_shm_open_heap(shm_name);
    if (shm == NULL)
      Rf_error("mori: shared memory region not found: '%s'", shm_name);
    SEXP shm_ptr = PROTECT(mori_shm_wrap_consumer(shm));
    SEXP result = mori_dispatch_by_magic(shm_ptr, shm_name);
    UNPROTECT(1);
    return result;
  }

  /* Path form: 0-based indices, route through C-level core. */
  return mori_open_path_c(shm_name, path, path_len);
}

SEXP mori_is_shared(SEXP x) {
  if (!ALTREP(x)) return Rf_ScalarLogical(0);
  SEXP d1 = R_altrep_data1(x);
  return Rf_ScalarLogical(
    TYPEOF(d1) == EXTPTRSXP &&
    R_ExternalPtrTag(d1) == mori_owned_tag
  );
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
   Windows). Only names matching the mori identifier grammar are ever unlinked,
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
                        buf, sizeof(buf)) == 0)
    return Rf_mkString(buf);

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
                        buf, sizeof(buf)) == 0)
    return Rf_mkString(buf);

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
                        buf, sizeof(buf)) == 0)
    return Rf_mkString(buf);

  /* Overflow / malformed chain: materialize. */
  R_xlen_t n = view->n_elements;
  SEXP mat = PROTECT(Rf_allocVector(VECSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_VECTOR_ELT(mat, i, mori_list_Elt(x, i));
  DUPLICATE_ATTRIB(mat, x);
  UNPROTECT(1);
  return mat;
}

/* Open parent SHM and walk the path, returning the leaf element.
   path has length >= 1. Intermediate steps must be VECSXP children
   (nested MORL regions); the final step is the leaf. */
static SEXP mori_open_path_c(const char *name,
                             const int32_t *path, int path_len) {

  mori_shm *shm = mori_shm_open_heap(name);
  if (shm == NULL)
    Rf_error("mori: shared memory region not found: '%s'", name);

  SEXP shm_ptr = PROTECT(mori_shm_wrap_consumer(shm));

  /* Root view (index = -1): uniform chain shape with mori_open_list. */
  SEXP root_view = PROTECT(mori_make_view_extptr(
    (unsigned char *) shm->addr, (int64_t) shm->size, -1, shm_ptr, NULL, NULL
  ));
  mori_list_view *rv = (mori_list_view *) R_ExternalPtrAddr(root_view);

  SEXP keeper = root_view;
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

    unsigned char *dir = cur_base + 24 + 32 * (size_t) idx;
    mori_elem entry;
    memcpy(&entry, dir, sizeof(mori_elem));
    int64_t data_offset = entry.data_offset, data_size = entry.data_size;
    int32_t sexptype = entry.sexptype;

    if (sexptype != VECSXP)
      Rf_error("mori: path step is not a nested list");
    if (mori_oob(data_offset, data_size, cur_region_size))
      Rf_error("mori: invalid nested region");

    /* Bare extptr: no ALTLIST wrapper, no attr restore (intermediate is
       never observed; only its index in the keeper chain matters). */
    REPROTECT(child = mori_make_view_extptr(
      cur_base + data_offset, data_size, idx, keeper, NULL, NULL
    ), child_idx);
    keeper = child;

    mori_list_view *cv = (mori_list_view *) R_ExternalPtrAddr(child);
    cur_base = cv->base;
    cur_region_size = cv->region_size;
    cur_n = cv->n_elements;
  }

  int32_t leaf_idx = path[path_len - 1];
  if (leaf_idx < 0 || leaf_idx >= cur_n)
    Rf_error("mori: leaf index out of bounds");

  SEXP result = mori_unwrap_element(cur_base, cur_region_size,
                                    leaf_idx, keeper);
  UNPROTECT(3);
  return result;
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

  mori_shm_tag = Rf_install("mori_shm");
  mori_host_tag = Rf_install("mori_host");
  mori_owned_tag = Rf_install("mori_owned");

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
