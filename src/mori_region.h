/* Generated from libmizu by tools/vendor.sh — do not edit.
   Upstream: https://github.com/shikokuchuo/libmizu
   Ref: 909563b2f1c9f8a46f6b8ca243fb94408c0237c5 (commit 909563b2f1c9f8a46f6b8ca243fb94408c0237c5, 2026-10-03T22:48:29+01:00)

   The shared-memory region layer of mori, vendored from libmizu: region
   create/map/unlink over POSIX shm / Win32 file mappings, plus the region
   wire-format constants and the layout helpers (geometry, validity, NA
   bitmaps, MORL directory entries) the vendored view layer resolves onto.
   This header, shm.c and err_tls.c are host-language independent. */

#ifndef MORI_REGION_H
#define MORI_REGION_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* Vendored static build: no export decoration. */
#define MORI_API

/* Some carved static inlines go unused in some TUs; keep them quiet.
   (Upstream defines this annotation above the carve boundary.) */
#if defined(__GNUC__) || defined(__clang__)
#  define MORI_MAYBE_UNUSED __attribute__((unused))
#else
#  define MORI_MAYBE_UNUSED
#endif

typedef struct mori_shm_s mori_shm;

// Status and errors ------------------------------------------------------------

/** The verb result. MORI_FULL: ring/slot capacity exhausted (try-send).
   MORI_TIMEOUT: deadline passed. MORI_CLOSED: orderly close. MORI_PEER_GONE:
   the peer's liveness lock is released (the death verdict; listeners are
   wake triggers only). MORI_ERR: a real error — category + message. */
typedef enum mori_status_e {
  MORI_OK = 0,
  MORI_FULL,
  MORI_TIMEOUT,
  MORI_CLOSED,
  MORI_PEER_GONE,
  MORI_ERR
} mori_status;

/** Portable failure categories for MORI_ERR (the platform layer classifies
   errno / GetLastError). */
typedef enum mori_errcat_e {
  MORI_ERRCAT_NONE = 0,
  MORI_ERRCAT_NOSPACE,      /**< ENOSPC / ERROR_DISK_FULL */
  MORI_ERRCAT_NOMEMORY,     /**< ENOMEM / commit limit exceeded */
  MORI_ERRCAT_EXISTS,       /**< region name already in use (orphan) */
  MORI_ERRCAT_EXHAUSTED,    /**< pool result slots exhausted (submit) */
  MORI_ERRCAT_STOPPED,      /**< pool stopped, or its owner died (submit) */
  MORI_ERRCAT_STAGE,        /**< the binding's stage_fn returned nonzero */
  MORI_ERRCAT_INTERRUPTED,  /**< the binding's check hook returned nonzero */
  MORI_ERRCAT_OTHER
} mori_errcat;

/** Error access. Handle verbs record the category + message on the handle;
   both are valid until the next call on that handle. Handle-free entry
   points (regions, prune) use a thread-local slot instead. */
MORI_API mori_errcat mori_last_error_category(void);
MORI_API const char *mori_last_error_message(void);


// Wire format: MORH / MORS / MORL region layouts ---------------------------------

/** Region magics (first 4 bytes): atomic vector, string vector, list tree.
   Every layout opens with a 64-byte header; bytes [24-63] were reserved
   (written zero) — the zc protocol owns [24-31], and the two words below
   are now assigned: the format flags word and the validity section.

   Header bytes [32-35] are the format flags word. Bit 0 is the S4 object
   bit, which the layouts otherwise cannot carry; every other bit is
   reserved zero. This is a format word, not a negotiation word: a reader
   rejects a set bit it does not know as a corrupt or newer region (the
   identity word's reserved bits are the opposite — ignored). */
#define MORI_HDR_FLAGS_OFF ((size_t) 32)
#define MORI_HDR_FLAG_S4   1u

/** Header bytes [40-47] and [48-55] are the optional validity-bitmap
   section of every MORH and MORL header (nested MORL included; MORS
   carries its bitmap in the string block instead): an i64 offset and an
   i64 null count, three states. {0, 0}: absent — a pre-section region,
   the core's flat reserve, or a same-language write; the reader's lazy
   fallback. {0, -1}: known-NA-free (no section materialized). Else the
   offset of a 64-byte-aligned section: for MORH a ceil(n / 8)-byte
   LSB-first bitmap (1 = present, the MORS string block's convention);
   for MORL a table of n {i64 offset, i64 null count} leaf entries, one
   per directory entry — a VECSXP or STRSXP entry's pair is {0, 0} (the
   nested header / the string block carries its own), and [48-55] is the
   total null count across that header's remaining leaves. Reserved-zero
   means absent, so a pre-dating reader simply never looks; the section
   needs no capability bit. */
#define MORI_HDR_VALID_OFF   ((size_t) 40)
#define MORI_HDR_VALID_COUNT ((size_t) 48)

/** MORH (atomic vector): [4-7] i32 wire type, [8-15] i64 element count,
   [16-23] i64 attrs blob size; bare element bytes at 64, the attrs blob
   trailing. MORS (string vector): [4-7] i32 attrs blob size, [8-15] i64
   string count, [16-23] i64 string-block byte size; the string block at
   64, the attrs blob trailing it. The string block (also the form of an
   MORL STRSXP leaf): a ceil(n / 8)-byte validity bitmap, (n + 1) i64
   offsets, n encoding bytes (MORI_CE_*), and the packed string bytes —
   each section 64-byte aligned from the block start. The first, second
   and fourth are Arrow large_utf8's three buffers verbatim; the encoding
   section is R's per-CHARSXP mark. MORL (list tree): [4-7] i32 element
   count n, [8-15] i64 attrs blob offset, [16-23] i64 attrs blob size;
   then n 32-byte directory entries, each { i64 data_offset (64-byte
   aligned), i64 data_size, i32 sexptype, i32 attrs_size, i64 length }.
   The entry's data is the bare element bytes (an atomic leaf), a string
   block (STRSXP), a nested MORL (VECSXP) or a serialized stream
   (sexptype 0); the leaf's attrs blob is the last attrs_size bytes of
   data_size. Bit 30 of the entry's sexptype is the leaf's S4 bit; the
   remainder is a listed tag — 0 (serialized), the atomic tags, STR, VEC,
   and 32 (MORI_TYPE_INT64 is legal on MORL leaves). Tag 33 is a remote
   leaf: the column lives in another region and crosses by reference.
   data_offset / data_size hold the identifier span — the region name,
   optionally name[i,j,...] with 1-based decimal indices, 1-255 bytes —
   length and attrs_size describe the referenced column as resolved, the
   S4 bit is never set, and the validity pair is a {0,0} / {0,-1} claim
   alone (DESIGN.md's remote-leaf rules are normative). */
#define MORI_MORL_S4 0x40000000

#define MORI_MAGIC_VEC   0x4D4F5248u  /**< "MORH" */
#define MORI_MAGIC_STR   0x4D4F5253u  /**< "MORS" */
#define MORI_MAGIC_LIST  0x4D4F524Cu  /**< "MORL" */
#define MORI_HEADER_SIZE 64

#define MORI_NAME_MAX 30  /**< fits Windows worst case + NUL; Darwin PSHMNAMLEN */


/** Wire type tags, fixed by the wire format (RAWVEC aux, MIZU* layout
   headers). Bindings map their own element types onto these. The numbers
   are libmizu-owned and frozen from the first release: they descend from
   R's SEXPTYPE space by history, never by dependency — a future SEXPTYPE
   joins the wire only by an explicit allocation here, as INT64 = 32
   already did. */
typedef enum mori_type_e {
  MORI_TYPE_LGL = 10,         /**< int32 logical; INT_MIN is the missing sentinel */
  MORI_TYPE_INT = 13,         /**< int32; INT_MIN is the missing sentinel */
  MORI_TYPE_REAL = 14,        /**< float64; a specific NaN payload is the sentinel */
  MORI_TYPE_CPLX = 15,        /**< interleaved float64 re/im */
  MORI_TYPE_STR = 16,         /**< string vector (MORS layout) */
  MORI_TYPE_VEC = 19,         /**< list tree (MORL layout) */
  MORI_TYPE_RAW = 24,         /**< bytes */
  MORI_TYPE_INT64 = 32        /**< int64; INT64_MIN is the missing sentinel */
} mori_type;

/** The missing-value sentinels of the atomic wire types (the R ABI fixes
   the bit patterns; a binding of any language writes them without R
   headers). Little-endian throughout, as the whole wire format is. */
#define MORI_NA_INT32     INT32_MIN              /**< NA_integer_ / NA logical */
#define MORI_NA_INT64     INT64_MIN              /**< NA_integer64_ sentinel */
#define MORI_NA_REAL_BITS 0x7FF80000000007A2ULL  /**< NA_real_ (a NaN payload) */



// Binding-tier constants ---------------------------------------------------------

#ifdef _WIN32
#  define MORI_PREFIX_LITERAL "Local\\mori_"
#else
#  define MORI_PREFIX_LITERAL "/mori_"
#endif

#define MORI_REGION_ALIGN64(x) (((x) + 63) & ~(size_t) 63)

/** Staging-policy floors for a binding's stage_fn (not used by the
   core): SHM_VEC escalates only past max(inline budget, MORI_ZC_FLOOR);
   the channel's raw floor is higher (the arena copy has no region
   machinery to amortize) and lifts entirely under the churn signal.
   Bindings pick their own. */
#define MORI_ZC_FLOOR     ((size_t) 32768)
#define MORI_ZC_FLOOR_RAW ((size_t) (256 << 10))

/** The core's consumer SHM_RAW mapping cache size; a binding sizes its
   own view cache to match. */
#define MORI_OPEN_CACHE_MAX  16

/** The self-describing stream dispatch byte: payload byte 0 of an INLINE
   frame, where an R native stream carries 'B'/'X'. The core is
   codec-agnostic; DESIGN.md's codec registry allocates the magic bytes
   ('R' = mizu, 'P' = pymizu). 'R' here and MORI_DROP_R share the letter
   deliberately — both denote an R-binding payload, in disjoint
   contexts. */
#define MORI_CODEC_MAGIC 0x52u   /**< 'R' */
#define MORI_PYMIZU_CODEC_MAGIC 0x50u   /**< 'P' */
#define MORI_INTEROP_MAGIC 0x49u   /**< 'I': the interchange stream (DESIGN.md's
   Interchange codec section) */

// Language and capability registries ----------------------------------------------

/** The language registry: the identity word's byte 0, the pool's worker
   identity word, and each binding's handle-level peer_lang. Append-only:
   a value, once shipped, is never reassigned (the byte is
   equality-compared, so a reassignment fails behaviorally, as false join
   rejections across mixed builds). 0 is "none" — no peer attached yet, a
   pool no worker has joined — never a stored binding identity: a binding
   whose identity word has a zero language byte is rejected at create,
   attach and join. */
#define MORI_LANG_NONE   0u
#define MORI_LANG_BYTES  1u   /**< the core's bytes binding and test bindings */
#define MORI_LANG_R      2u
#define MORI_LANG_PYTHON 3u

/** Reader capabilities: a 32-bit mask, one bit per layout or format
   extension a reader implements beyond the baseline (MORH atomic and
   INT64 views, 'I' format 0x01 as specified in DESIGN.md's Interchange
   codec section). Readers ignore bits they do not know — they never
   reject them; absent means unsupported, and a writer stages a layout or
   tag only for a peer that sets its bit. A bit names a byte layout, not a
   feature: an incompatible change to a gated layout, tag or shape
   allocates a new bit and retires the old, which is never reassigned.
   Features that always land together in every binding share a bit. */
#define MORI_CAP_MORS  (1u << 0)   /**< reads MORS string layouts */
#define MORI_CAP_ATTRS (1u << 1)   /**< reads an 'I' attribute blob on a
   layout root or MORL leaf */
#define MORI_CAP_MORL  (1u << 2)   /**< wraps a generic MORL tree as views */
#define MORI_CAP_TASKREF (1u << 3) /**< reads 0x13 ref leaves: task
   arguments by reference (task streams and the map descriptor, which
   shares the value grammar) */
#define MORI_CAP_MORL_REF (1u << 4) /**< reads MORL remote leaves —
   directory tag 33 */

/** The identity word: the language in bits 0-7, the 32-bit capability mask
   in bits 32-63, bits 8-31 reserved — written zero and ignored by readers
   (a negotiation word, so a later field needs no reader taught to skip
   it). The pool word's exact-match join compares the whole word. */
#define MORI_IDENT(lang, caps) \
  ((uint64_t) (uint8_t) (lang) | ((uint64_t) (uint32_t) (caps) << 32))


/** Some static inlines below go unused in some TUs; the amalgamation
   folds this header into the single mizu.c TU, where they would trip
   -Wunused-function (header inlines are exempt only in a real header). */
#if defined(__GNUC__) || defined(__clang__)
#  define MORI_EXT_INLINE static inline __attribute__((unused))
#else
#  define MORI_EXT_INLINE static inline
#endif


// The region handle struct ---------------------------------------------------------

/** The public opaque type in mori_region.h; defined here for bindings (the
   core's control-region mappings embed one in the handle structs).
   Layout-pinned per minor release like the rest of this tier: any field
   addition or reorder forces a binding rebuild. addr/size/name are
   borrowed reads, valid until close. */
struct mori_shm_s {
  void *addr;
  size_t size;
  char name[MORI_NAME_MAX];
  uint8_t name_len;
  unsigned int pid;            /**< creator PID: fork guard (POSIX only) */
#ifdef _WIN32
  void *handle;
#endif
};


// Utilities ----------------------------------------------------------------------


/** Category + remediation text for an MORI_ERRCAT (what the handle /
   thread-local error slot records). */
MORI_API void mori_err_describe(mori_errcat, const char **summary,
                              const char **hint);




/** The MORS string block's four section offsets from a string count
   (mori_region.h documents the block). data doubles as the size of everything
   before the string bytes. */
typedef struct mori_mors_geom_s {
  int64_t validity;
  int64_t offsets;
  int64_t encoding;
  int64_t data;
} mori_mors_geom;


/** The string block's section offsets for n strings (each section 64-byte
   aligned from the block start). Dual form, like the helpers below. */
#ifdef MORI_EXT_NO_INLINES
MORI_API mori_mors_geom mori_mors_geometry(int64_t n);
#else
MORI_EXT_INLINE mori_mors_geom mori_mors_geometry(int64_t n) {
  mori_mors_geom g;
  g.validity = 0;
  g.offsets  = (int64_t) MORI_REGION_ALIGN64((uint64_t) (n + 7) / 8);
  g.encoding = g.offsets + (int64_t) MORI_REGION_ALIGN64(8 * ((uint64_t) n + 1));
  g.data     = g.encoding + (int64_t) MORI_REGION_ALIGN64((uint64_t) n);
  return g;
}
#endif


MORI_EXT_INLINE size_t mori_type_elt_size(int type) {
  switch (type) {
  case MORI_TYPE_REAL: return sizeof(double);
  case MORI_TYPE_INT:
  case MORI_TYPE_LGL: return sizeof(int32_t);
  case MORI_TYPE_RAW: return 1;
  case MORI_TYPE_INT64: return sizeof(int64_t);
  case MORI_TYPE_CPLX: return 2 * sizeof(double);
  default:           return 0;
  }
}

/** One MORL directory entry plus its validity-table pair (mori_region.h documents
   the 32-byte entry): mori_morl_elem's out-param. sexptype is the wire
   value — MORI_MORL_S4 is the S4 bit, the remainder a listed tag. */
typedef struct mori_morl_entry_s {
  int64_t data_offset;   /**< 64-byte aligned */
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
  int64_t valid[2];      /**< {0, 0} absent, {0, -1} known-NA-free, else
   {bitmap offset, null count} */
} mori_morl_entry;


/** The [32-35] format flags word admits only the assigned S4 bit. */
MORI_EXT_INLINE int mori_ext_flags_known(const void *base) {
  uint32_t flags;
  memcpy(&flags, (const unsigned char *) base + MORI_HDR_FLAGS_OFF, 4);
  return (flags & ~MORI_HDR_FLAG_S4) == 0;
}


/** The three-state validity pair of an MORH header or one MORL leaf-table
   entry: {0, 0} absent, {0, -1} known-NA-free, or a 64-byte-aligned
   offset whose ceil(n / 8)-byte bitmap fits the region, 0 <= count <= n. */
MORI_EXT_INLINE int mori_ext_valid_ok(int64_t off, int64_t count,
                                     uint64_t n, size_t size) {
  if (off == 0) return count == 0 || count == -1;
  if (count < 0 || (uint64_t) count > n || (off & 63) != 0) return 0;
  const uint64_t bytes = (n + 7) / 8;
  return (uint64_t) off <= (uint64_t) size &&
         bytes <= (uint64_t) size - (uint64_t) off;
}


/** A directory entry's sexptype: MORI_MORL_S4 masked off, the remainder a
   listed tag — 0 (a serialized leaf), the atomic tags, STR, VEC, INT64,
   and the remote leaf (33). Anything else unlisted rejects. */
MORI_EXT_INLINE int mori_ext_morl_tag_ok(int32_t sexptype) {
  switch (sexptype & ~(int32_t) MORI_MORL_S4) {
  case 0:
  case MORI_TYPE_LGL:
  case MORI_TYPE_INT:
  case MORI_TYPE_REAL:
  case MORI_TYPE_CPLX:
  case MORI_TYPE_STR:
  case MORI_TYPE_VEC:
  case MORI_TYPE_RAW:
  case MORI_TYPE_INT64:
  case 33:                        /* remote leaf */
    return 1;
  default:
    return 0;
  }
}


/** One per-element NA test per NA-capable wire type (anything else is
   never null): INT32_MIN for LGL and INT, INT64_MIN, and the NA_real_
   payload discriminated from other NaNs — any NaN whose low word is
   1954 (0x7A2), R's own ISNA test, so a writer's quiet-bit-clear
   NA_real_ (R's verbatim form) and the wire's quiet-bit-set twin both
   read as NA while a genuine NaN stays a value. The CPLX pair either
   part. */
MORI_EXT_INLINE int mori_ext_na_real_bits(uint64_t bits) {
  return ((bits >> 52) & 0x7FF) == 0x7FF && (uint32_t) bits == 0x7A2u;
}
MORI_EXT_INLINE int mori_ext_na_at(int type, const void *src, uint64_t i) {
  const unsigned char *p = (const unsigned char *) src;
  uint64_t bits, re, im;
  int32_t v32;
  int64_t v64;
  switch (type) {
  case MORI_TYPE_LGL:
  case MORI_TYPE_INT:
    memcpy(&v32, p + 4 * i, 4);
    return v32 == MORI_NA_INT32;
  case MORI_TYPE_REAL:
    memcpy(&bits, p + 8 * i, 8);
    return mori_ext_na_real_bits(bits);
  case MORI_TYPE_CPLX:
    memcpy(&re, p + 16 * i, 8);
    memcpy(&im, p + 16 * i + 8, 8);
    return mori_ext_na_real_bits(re) || mori_ext_na_real_bits(im);
  case MORI_TYPE_INT64:
    memcpy(&v64, p + 8 * i, 8);
    return v64 == MORI_NA_INT64;
  default:
    return 0;
  }
}


/** Stamp the validity pair of an MORH or MORL header (the one write site):
   {0, 0} absent, {0, -1} known-NA-free, or the section offset and null
   count. */
MORI_EXT_INLINE void mori_morh_validity_set(void *base, int64_t off,
                                          int64_t count) {
  memcpy((unsigned char *) base + MORI_HDR_VALID_OFF, &off, 8);
  memcpy((unsigned char *) base + MORI_HDR_VALID_COUNT, &count, 8);
}

/** One MORL directory entry's checks, shared by mori_morl_check's pass and
   mori_morl_elem: alignment and extent, the attrs tail, the listed tag,
   and the length against the leaf kind — the string block's fixed
   sections for STR, the element extent for an atomic leaf. A remote leaf
   (tag 33) skips the attrs-tail and body checks: the span is the
   identifier (1–255 bytes), and length / attrs_size describe the
   referenced column as resolved; the S4 bit rejects. Fills *e except the
   valid pair. */
MORI_EXT_INLINE int mori_ext_morl_ent(const void *base, size_t size,
                                     int64_t i, mori_morl_entry *e) {
  const unsigned char *dir = (const unsigned char *) base +
    MORI_HEADER_SIZE + 32 * (size_t) i;
  memcpy(&e->data_offset, dir, 8);
  memcpy(&e->data_size, dir + 8, 8);
  memcpy(&e->sexptype, dir + 16, 4);
  memcpy(&e->attrs_size, dir + 20, 4);
  memcpy(&e->length, dir + 24, 8);
  if (e->data_offset < 0 || (e->data_offset & 63) != 0 ||
      e->data_size < 0 ||
      (uint64_t) e->data_offset > (uint64_t) size ||
      (uint64_t) e->data_size >
        (uint64_t) size - (uint64_t) e->data_offset ||
      e->attrs_size < 0)
    return -1;
  if (!mori_ext_morl_tag_ok(e->sexptype)) return -1;
  const int32_t tag = e->sexptype & ~(int32_t) MORI_MORL_S4;
  if (tag == 33) {              /* remote leaf: the span is the identifier */
    if ((e->sexptype & (int32_t) MORI_MORL_S4) != 0 || e->length < 0 ||
        e->data_size < 1 || e->data_size > 255)
      return -1;
    return 0;
  }
  if ((int64_t) e->attrs_size > e->data_size) return -1;
  const int64_t body = e->data_size - (int64_t) e->attrs_size;
  const size_t elt = mori_type_elt_size(tag);
  if (elt != 0) {
    if (e->length < 0 || e->length > body / (int64_t) elt) return -1;
  } else if (tag == MORI_TYPE_STR) {
    if (e->length < 0 || e->length > ((int64_t) 1 << 50) ||
        mori_mors_geometry(e->length).data > body)
      return -1;
  } else if (tag == MORI_TYPE_VEC) {
    if (e->length < 0) return -1;
  }
  return 0;
}


MORI_EXT_INLINE int mori_ext_morl_elem_impl(const void *base, size_t size,
                                           int64_t i,
                                           mori_morl_entry *elem) {
  if (size < MORI_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t n32;
  int64_t voff, vcount;
  memcpy(&magic, base, 4);
  if (magic != MORI_MAGIC_LIST) return -1;
  memcpy(&n32, (const unsigned char *) base + 4, 4);
  if (n32 < 0 || i < 0 || i >= (int64_t) n32 ||
      (uint64_t) (uint32_t) n32 >
        ((uint64_t) size - (uint64_t) MORI_HEADER_SIZE) / 32)
    return -1;
  if (!mori_ext_flags_known(base)) return -1;
  if (mori_ext_morl_ent(base, size, i, elem) != 0) return -1;
  memcpy(&voff, (const unsigned char *) base + MORI_HDR_VALID_OFF, 8);
  memcpy(&vcount, (const unsigned char *) base + MORI_HDR_VALID_COUNT, 8);
  if (voff == 0) {
    if (vcount != 0 && vcount != -1) return -1;
    elem->valid[0] = 0;
    elem->valid[1] = vcount;
    return 0;
  }
  if ((voff & 63) != 0) return -1;
  const uint64_t bytes = (uint64_t) (uint32_t) n32 * 16;
  if ((uint64_t) voff > (uint64_t) size ||
      bytes > (uint64_t) size - (uint64_t) voff)
    return -1;
  const unsigned char *tab = (const unsigned char *) base + voff;
  int64_t loff, lcount;
  memcpy(&loff, tab + 16 * (size_t) i, 8);
  memcpy(&lcount, tab + 16 * (size_t) i + 8, 8);
  /* a remote leaf (tag 33) carries a {0,0} / {0,-1} claim alone — a
     bitmap offset is region-local and cannot describe a remote column */
  if ((elem->sexptype & ~(int32_t) MORI_MORL_S4) == 33 && loff != 0)
    return -1;
  if (!mori_ext_valid_ok(loff, lcount, (uint64_t) elem->length, size))
    return -1;
  elem->valid[0] = loff;
  elem->valid[1] = lcount;
  return 0;
}


MORI_EXT_INLINE uint64_t mori_ext_na_build_impl(int type, uint8_t *bitmap,
                                               const void *src, uint64_t n,
                                               uint64_t bit_off) {
  uint64_t nulls = 0;
  for (uint64_t i = 0; i < n; i++) {
    const uint64_t bit = bit_off + i;
    if (mori_ext_na_at(type, src, i)) {
      bitmap[bit >> 3] &= (uint8_t) ~(1u << (bit & 7));
      nulls++;
    } else {
      bitmap[bit >> 3] |= (uint8_t) (1u << (bit & 7));
    }
  }
  return nulls;
}


/** The sentinels-to-bitmap scan: bit (bit_off + i) of bitmap records
   element i of src, 1 = present (the MORS string block's convention);
   a type with no missing sentinel is all-present. Returns the null
   count. */
MORI_EXT_INLINE uint64_t mori_na_build(int type, uint8_t *bitmap,
                                     const void *src, uint64_t n,
                                     uint64_t bit_off) {
  return mori_ext_na_build_impl(type, bitmap, src, n, bit_off);
}

/** One bounds-checked MORL directory entry (index i), with its
   validity-table pair: a {0, 0}/{0, -1} header state covers every leaf;
   otherwise the table's entry i is validated by the MORH rule against
   the leaf's length. 0 on success, -1 on any rejection. */
MORI_EXT_INLINE int mori_morl_elem(const void *base, size_t size, int64_t i,
                                 mori_morl_entry *elem) {
  return mori_ext_morl_elem_impl(base, size, i, elem);
}

// Region layer internals ---------------------------------------------------------

/* macOS registry dir: "<dir>/mori". The liveness dir env override: */
#define MORI_LIVENESS_DIR_ENV "MORI_LIVENESS_DIR"

/* Stack forms (the public API is the heap form). create returns an
   mori_errcat (MORI_ERRCAT_NONE on success); open returns 0/-1. */
int mori_shm_create_stack(mori_shm *shm, size_t size);
int mori_shm_open_stack(mori_shm *shm, const char *name);
/* Heap forms without the error-slot record (the public verbs add it):
   create_heap returns a mori_errcat; open_heap returns NULL on failure. */
int mori_shm_create_heap(mori_shm **out, size_t size);
mori_shm *mori_shm_open_heap(const char *name);
/* Unmaps only (never frees the struct); unlink != 0 also removes the
   name. The public mori_shm_close wraps this + free. */
void mori_shm_close_stack(mori_shm *shm, int unlink);

/* The open forms' name copy: fixed-buffer truncate, always
   NUL-terminated, length stamped. */
static inline MORI_MAYBE_UNUSED void mori_shm_set_name(mori_shm *shm,
                                                      const char *name) {
  size_t nl = strlen(name);
  if (nl >= sizeof(shm->name)) nl = sizeof(shm->name) - 1;
  memcpy(shm->name, name, nl);
  shm->name[nl] = '\0';
  shm->name_len = (uint8_t) nl;
}

/* macOS registry-log exit/unload hook (shm.c; defined under __APPLE__
   only): removes this process's log and prunes the registry dir once
   every created region is torn down. Registered as a library
   destructor; declared here for the unit tier. */
void mori_log_teardown(void);



/* Host-side teardown of a created region: releases the SHM name (POSIX:
   unlink) / creator handle (Windows) without touching the mapping. */
void mori_shm_host_release(mori_shm *shm);

/* Cold annotation for the error recorders: they sit on every hot verb's
   failure branches, and as returning variadic functions they otherwise
   force the compiler to treat those branches as live (register pressure,
   worse layout, blown inline budgets). */
#if defined(_MSC_VER)
#  define MORI_COLD
#else
#  define MORI_COLD __attribute__((cold))
#endif

// Errors ---------------------------------------------------------------------------

MORI_COLD void mori_err_record_tls(mori_errcat cat, const char *fmt, ...);


// Regions (mori_shm) ----------------------------------------------------------------

/** Heap-only (the stack form stays internal). create pre-faults on
   Linux. open maps read-only; open_rw maps writable (populate
   pre-faults); open_view is the zc consumer open: page 0 RW (the
   refcount word), the rest read-only — and performs the zc counted add
   itself, so a binding cannot hold a view mapping without the count.
   open_view_flags is the flags form: MORI_OPEN_VIEW_NOCOUNT skips the
   counted add — the caller then owns the mori_zc_ref timing and must
   complete it before its consumer-done signal (a binding whose wrap can
   fail between map and count opens first and counts at wrap).
   close unmaps and frees the handle; unlink != 0 also removes the name.
   On failure these return MORI_ERR with the category in the thread-local
   error slot. addr/size/name are borrowed reads, valid until close. */
#define MORI_OPEN_VIEW_NOCOUNT 1u /**< caller performs the counted add itself */
MORI_API mori_status mori_shm_create(mori_shm **out, size_t size);
MORI_API mori_status mori_shm_open(mori_shm **out, const char *name);
MORI_API mori_status mori_shm_open_view_flags(mori_shm **out, const char *name,
                                           uint32_t flags);
MORI_API void mori_shm_close(mori_shm *, int unlink);
MORI_API void *mori_shm_addr(mori_shm *);
MORI_API size_t mori_shm_size(const mori_shm *);
MORI_API const char *mori_shm_region_name(const mori_shm *);

/** Reap /mori_ orphans of dead creators. Returns a malloc'd array of
   malloc'd names (free each, then the array), *n the count; NULL when
   none — including on platforms that cannot enumerate the shm namespace
   (Windows, where a mapping cannot outlive its creator). */
MORI_API char **mori_shm_reap(int *n);


#endif /* MORI_REGION_H */
