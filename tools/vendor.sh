#!/usr/bin/env bash
# Vendors mori's C layer from its two upstreams in one pass — the region
# layer from libmizu, the view layer from mizu — applying the enumerated
# substitution sets that restore the /mori_ runtime namespace. libmizu is
# the upstream source of truth for the region layer and mizu for the view
# layer; mori tracks both. One script, because the pair is one consistent
# unit: the view layer's region references resolve onto the region header
# assembled here, and the libmizu ref defaults to the one mizu's own
# vendored libmizu records (the pairing mizu was built against).
#
# Generated files (never edit by hand; changes go upstream and are pulled by
# re-running this script):
#   src/mori_region.h  assembled from carved libmizu mizu.h / mizu_ext.h / internal.h / api.c sections
#   src/shm.c          libmizu src/shm.c
#   src/err_tls.c      libmizu src/err_tls.c
#   src/mori.h         mizu src/view.h
#   src/altrep.c       mizu src/view.c
#   src/serialize.c    mizu src/serialize.c
#
# Usage: tools/vendor.sh [--region-only|--view-only] [--mizu ref] [--libmizu ref]
#   --mizu ref      tag, branch, or full commit SHA for the view layer
#                   (default: the pin)
#   --libmizu ref   the same for the region layer (default: the ref mizu's
#                   vendored libmizu VENDOR file records when the view layer
#                   is vendored, else the pin)
#   MIZU_SRC / LIBMIZU_SRC    use local checkouts instead of cloning (must be at ref)
#   MIZU_REPO / LIBMIZU_REPO  override the upstream clone URLs
#
# Idempotent. Each layer greps its own output for stray upstream-namespace
# remnants and fails non-zero on any hit.

set -euo pipefail

MIZU_PIN="901f2e6cbedc55507fb69572ed11b988750e47c3"    # mizu: Bump the libmizu vendor pin to the F2 remote-leaf wire contract
LIBMIZU_PIN="1efb8839b7c0ffef614edeeb398a2f27b36f9518" # libmizu: Add the MIZL remote leaf (directory tag 33) wire contract
MIZU_REPO="${MIZU_REPO:-https://github.com/shikokuchuo/mizu}"
LIBMIZU_REPO="${LIBMIZU_REPO:-https://github.com/shikokuchuo/libmizu}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/src"

do_region=1
do_view=1
mizu_ref=""
libmizu_ref=""
while [ $# -gt 0 ]; do
  case "$1" in
    --region-only) do_view=0 ;;
    --view-only)   do_region=0 ;;
    --mizu)        mizu_ref="$2"; shift ;;
    --libmizu)     libmizu_ref="$2"; shift ;;
    *) echo "usage: tools/vendor.sh [--region-only|--view-only] [--mizu ref] [--libmizu ref]" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$mizu_ref" ] || mizu_ref="$MIZU_PIN"

workdir="$(mktemp -d)"
cleanup() { rm -rf "$workdir"; }
trap cleanup EXIT

# fetch <repo> <ref> <dirname> [local-src] -> prints the source root: the
# local checkout unchanged, else a fresh shallow fetch under workdir. A
# commit SHA is not a ref: fetch it directly (GitHub serves reachable
# SHAs) — clone --branch only takes branch/tag names.
fetch() {
  local repo="$1" ref="$2" dir="$3" local_src="${4:-}"
  if [ -n "$local_src" ]; then printf '%s' "$local_src"; return; fi
  if printf '%s' "$ref" | grep -qE '^[0-9a-f]{40}$'; then
    git init -q "$workdir/$dir"
    git -C "$workdir/$dir" remote add origin "$repo"
    git -C "$workdir/$dir" fetch -q --depth 1 origin "$ref"
    git -C "$workdir/$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
  else
    git clone -q --depth 1 --branch "$ref" "$repo" "$workdir/$dir"
  fi
  printf '%s' "$workdir/$dir"
}

# stamp <src_root> <paths...> -> prints "<head-sha> <iso-date> <-dirty?>" for
# the generated-file headers; -dirty marks a local checkout with uncommitted
# changes under the given paths.
stamp() {
  local root="$1"; shift
  local commit date
  commit="$(git -C "$root" rev-parse HEAD)"
  date="$(git -C "$root" log -1 --format=%cI HEAD)"
  if [ -n "$(git -C "$root" status --porcelain -- "$@")" ]; then
    commit="$commit-dirty"
  fi
  printf '%s %s' "$commit" "$date"
}

# Carve a section between banner anchors (end anchor excluded).
carve() {
  grep -q -- "$2" "$1" || { echo "carve: start boundary '$2' missing in $1" >&2; exit 1; }
  grep -q -- "$3" "$1" || { echo "carve: end boundary '$3' missing in $1" >&2; exit 1; }
  sed -n "\|$2|,\|$3|p" "$1" | sed '$d'
}

# Drop declarations of functions the region layer does not vendor (the
# transport side: shm_rw.c / spill.c / handle errors), each with its
# preceding comment block. $1: space-separated names. Fails safe: a name
# that disappears upstream simply matches nothing.
filter_decls() {
  awk -v names="$1" '
    BEGIN {
      n = split(names, nm, " ")
      for (i = 1; i <= n; i++) pat = pat (i > 1 ? "|" : "") nm[i]
      re = "(" pat ")\\("
    }
    indecl                { if (/;/) indecl = 0; next }
    incom                 { cbuf = cbuf $0 "\n"; if ($0 ~ /\*\//) incom = 0; next }
    /^[[:space:]]*\/\*/   { cbuf = cbuf $0 "\n"; if ($0 !~ /\*\//) incom = 1; next }
    /^[[:space:]]*$/      { printf "%s", cbuf; cbuf = ""; print; next }
    $0 ~ re               { cbuf = ""; if ($0 !~ /;/ && $0 !~ /^[[:space:]]*#/) indecl = 1; next }
                          { printf "%s", cbuf; cbuf = ""; print }
    END                   { printf "%s", cbuf }
  '
}

# --- The view layer's upstream is resolved first: the region layer's default
# ref is read from the libmizu copy mizu vendors (src/vendor/libmizu/VENDOR).

if [ "$do_view" -eq 1 ]; then
  mizu_root="$(fetch "$MIZU_REPO" "$mizu_ref" mizu "${MIZU_SRC:-}")"
  read mizu_commit mizu_date <<< "$(stamp "$mizu_root" src)"
  if [ -z "$libmizu_ref" ] && [ "$do_region" -eq 1 ]; then
    derived="$(sed -n 's/^ref: //p' "$mizu_root/src/vendor/libmizu/VENDOR" 2>/dev/null || true)"
    if [ -n "$derived" ]; then
      libmizu_ref="$derived"
      echo "region layer ref $libmizu_ref derived from mizu's vendored libmizu"
    fi
  fi
fi

# --- Region layer (libmizu) ---------------------------------------------------

if [ "$do_region" -eq 1 ]; then
  [ -n "$libmizu_ref" ] || libmizu_ref="$LIBMIZU_PIN"
  libmizu_root="$(fetch "$LIBMIZU_REPO" "$libmizu_ref" libmizu "${LIBMIZU_SRC:-}")"
  read libmizu_commit libmizu_date <<< "$(stamp "$libmizu_root" src include)"

  {
    cat <<EOF
/* Generated from libmizu by tools/vendor.sh — do not edit.
   Upstream: $LIBMIZU_REPO
   Ref: $libmizu_ref (commit $libmizu_commit, $libmizu_date)

   The shared-memory region layer of mori, vendored from libmizu: region
   create/map/unlink over POSIX shm / Win32 file mappings, plus the region
   wire-format constants and the layout helpers (geometry, validity, NA
   bitmaps, MIZL directory entries) the vendored view layer resolves onto.
   This header, shm.c and err_tls.c are host-language independent. */

#ifndef MORI_REGION_H
#define MORI_REGION_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* Vendored static build: no export decoration. */
#define MORI_API

typedef struct mori_shm_s mori_shm;

EOF
    carve "$libmizu_root/include/mizu.h" '^// Status and errors' '^// Opaque handles'
    echo
    carve "$libmizu_root/include/mizu.h" '^// Wire format: MIZH' '^// Wire format: pool region'
    echo
    # The wire type tags and NA sentinels the layout helpers switch on. The
    # mizu_type_elt_size declaration is dropped: the vendored static build
    # carves the api.c definition as an inline below instead.
    carve "$libmizu_root/include/mizu.h" '^/\*\* Wire type tags' '^/\*\* Zero-copy view protocol' |
      filter_decls "mizu_type_elt_size"
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^// Binding-tier constants' '^// Handle views'
    echo
    # MORI_EXT_INLINE: the carved layout helpers are static inline.
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* Some static inlines below' '^/\*\* The dual-form inlines touch atomics'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^// The region handle struct' '^// Parker'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^// Utilities' '^#ifdef __cplusplus' |
      filter_decls "mizu_now mizu_self_pid mizu_rng_jump mizu_tune"
    echo
    # The layout helpers the view layer delegates to (mizu's view.h sees the
    # whole ext tier through internal.h; mori carves just these). Emission
    # order is C declaration order: each helper precedes its callers.
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* The MIZS string block.s four section offsets' '^/\*\* One MIZL directory entry'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* The string block.s section offsets for n strings' '^/\*\* The \[32-35\] format flags word'
    echo
    # The element-size switch: libmizu exports it from api.c (MIZU_API); the
    # vendored static build wants the inline, so the definition is carved
    # with the MORI_EXT_INLINE prefix (the mizu.h declaration is filtered
    # out above — a static definition may not follow the extern one).
    # carve() is banner-delimited and would drop the closing brace, so a
    # bare sed range keeps the whole function body.
    printf 'MORI_EXT_INLINE '
    sed -n '\|^size_t mizu_type_elt_size|,\|^}|p' "$libmizu_root/src/api.c"
    echo
    # The MIZL directory-entry reader chain the view layer's path walk
    # resolves onto: the entry struct, the flags/validity/tag helpers, the
    # per-entry checks, and the bounds-checked accessor.
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* One MIZL directory entry plus its validity-table pair' '^/\*\* The string block.s section offsets for n strings'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* The \[32-35\] format flags word' '^/\*\* The three-state validity pair'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* The three-state validity pair' '^/\*\* A directory entry.s sexptype'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* A directory entry.s sexptype' '^/\*\* One per-element NA test'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* One per-element NA test' '^/\*\* Write the wire type.s missing sentinel'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* Stamp the validity pair' '^/\*\* Validate the MIZS header'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* One MIZL directory entry.s checks' '^/\*\* The bodies behind the larger dual-form'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^MIZU_EXT_INLINE int mizu_ext_mizl_elem_impl' '^MIZU_EXT_INLINE uint64_t mizu_ext_na_build_impl'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^MIZU_EXT_INLINE uint64_t mizu_ext_na_build_impl' '^MIZU_EXT_INLINE uint64_t mizu_ext_na_apply_impl'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* The sentinels-to-bitmap scan' '^/\*\* The bitmap-to-sentinels copy'
    echo
    carve "$libmizu_root/include/mizu_ext.h" '^/\*\* One bounds-checked MIZL directory entry' '^/\*\* The sentinels-to-bitmap scan'
    echo
    carve "$libmizu_root/src/internal.h" '^// Region layer internals' '^// Spin machinery' |
      filter_decls "mizu_region_unlink mizu_err_record mizu_shm_open_rw_stack mizu_shm_open_rw_heap mizu_shm_create_populate mizu_shm_open_ro_heap MIZU_ALIGN64"
    echo
    carve "$libmizu_root/include/mizu.h" '^// Regions (mizu_shm)' '^/\*\* The consumer side of the zc' |
      filter_decls "mizu_shm_open_rw mizu_shm_open_view"
    echo
    echo "#endif /* MORI_REGION_H */"
  } > "$DEST/mori_region.h"

  cp "$libmizu_root/src/shm.c" "$DEST/shm.c"
  cp "$libmizu_root/src/err_tls.c" "$DEST/err_tls.c"

  # The enumerated substitution set — the only transformation performed.

  # 1. The user-facing reaper is mori's prune_shared() (the hint string and
  #    its comment; the C entry point keeps its name via rule 3).
  # 2. The region name accessor collides with mori's R-facing mori_shm_name
  #    (SEXP), so it vendors as mori_shm_region_name. Rules 1-2 must run
  #    before the generic identifier rules.
  # 3-4. Identifiers and macros, in comments and strings alike (region names,
  #    the macOS registry-log filename, and the reaper's scan filter all
  #    derive from the prefix literal). MIZU_ALIGN64 takes a distinctive
  #    name: the view layer's own MORI_ALIGN64 (a MIZU_VIEW_ rename) would
  #    otherwise collide with the generic rule's product. The generic MIZU_
  #    rule double-applies to MIZU_PYMIZU_CODEC_MAGIC's PYMIZU_ infix; the
  #    last rule repairs the resulting MORI_PYMORI back to MORI_PYMIZU.
  # 5. Region magics back to MORH/MORS/MORL: mori regions are immortal and
  #    never refcounted — distinct magics keep the namespaces from aliasing
  #    under a hand-crafted identifier.
  # 6. Magic names in banner comments and comment strings, then the
  #    lowercase layout infixes (mori's helpers are morh_/mors_/morl_,
  #    matching the view layer's rename convention).
  # 7. The macOS registry dir.
  # 8. Includes: the vendored TUs' libmizu headers resolve to the generated
  #    header here.
  # 9. Comment mentions of the internal header (err_tls.c's cold-annotation
  #    note) — the gate forbids the literal string anywhere.
  sed -i.bak \
    -e 's|mizu_shm_reap()|prune_shared()|g' \
    -e 's|mizu_shm_name|mori_shm_region_name|g' \
    -e 's|mizu_|mori_|g' \
    -e 's|an mori_|a mori_|g' \
    -e 's|MIZU_ALIGN64|MORI_REGION_ALIGN64|g' \
    -e 's|MIZU_|MORI_|g' \
    -e 's|0x4D495A48u|0x4D4F5248u|g' \
    -e 's|0x4D495A53u|0x4D4F5253u|g' \
    -e 's|0x4D495A4Cu|0x4D4F524Cu|g' \
    -e 's|MIZH|MORH|g' \
    -e 's|MIZS|MORS|g' \
    -e 's|MIZL|MORL|g' \
    -e 's|mizh_|morh_|g' \
    -e 's|mizs_|mors_|g' \
    -e 's|mizl_|morl_|g' \
    -e 's|/mizu"|/mori"|g' \
    -e 's|"internal\.h"|"mori_region.h"|' \
    -e 's|mizu_ext\.h|mori_region.h|g' \
    -e 's|mizu\.h|mori_region.h|g' \
    -e 's|internal\.h|the internal header|g' \
    -e 's|MORI_PYMORI_CODEC_MAGIC|MORI_PYMIZU_CODEC_MAGIC|g' \
    "$DEST/mori_region.h" "$DEST/shm.c" "$DEST/err_tls.c"

  rm -f "$DEST/mori_region.h.bak" "$DEST/shm.c.bak" "$DEST/err_tls.c.bak"

  # The gate: no stray mizu/MIZU remnants may survive substitution.
  # MORI_PYMIZU_CODEC_MAGIC is the one intentional MIZU_ infix (see the
  # repair rule above).
  if grep -nE 'PYMORI|mizu_|MIZU_|0x4D495A|MIZH|MIZS|MIZL|mizh_|mizs_|mizl_|mizu\.h|mizu_ext\.h|internal\.h' \
      "$DEST/mori_region.h" "$DEST/shm.c" "$DEST/err_tls.c" | grep -v 'MORI_PYMIZU_CODEC_MAGIC'; then
    echo "vendor: FAIL — stray mizu remnants above (region layer)" >&2
    exit 1
  fi

  echo "vendored libmizu @ $libmizu_ref ($libmizu_commit, $libmizu_date) into $DEST"
fi

# --- View layer (mizu) --------------------------------------------------------

if [ "$do_view" -eq 1 ]; then
  cp "$mizu_root/src/view.h" "$DEST/mori.h"
  cp "$mizu_root/src/view.c" "$DEST/altrep.c"
  cp "$mizu_root/src/serialize.c" "$DEST/serialize.c"

  # The enumerated substitution set — the only transformation performed.

  # 1. Names the cutover de-stuttered in mizu's namespace map back to their
  #    historical mori names. Word-boundaried so mizu_view_list_wrap /
  #    mizu_view_list_class and friends fall through to the generic rules.
  #    Must run before them.
  # 2. The view layer's own symbols and macros (the "mizu_view_shm" extptr
  #    tags and the MIZU_VIEW_H include guard land here too).
  # 3. The header's own filename, in includes and comments.
  sed -i.bak \
    -e 's|mizu_view_check|mori_view_check|g' \
    -e 's|[[:<:]]mizu_view_list[[:>:]]|mori_list_view|g' \
    -e 's|[[:<:]]mizu_view_list_s[[:>:]]|mori_list_view_s|g' \
    -e 's|mizu_view_|mori_|g' \
    -e 's|MIZU_VIEW_|MORI_|g' \
    -e 's|view\.h|mori.h|g' \
    "$DEST/mori.h" "$DEST/altrep.c" "$DEST/serialize.c"

  # 4. Region-layer references rewire onto mori's vendored region layer
  #    (mori_region.h, assembled from libmizu above). The bare struct type
  #    matches only when not followed by an identifier character. The
  #    layout-helper and wire-constant rules must precede the MIZH/MIZS/
  #    MIZL and lowercase-infix rules, which would otherwise mangle their
  #    infixes (mizu_mizs_geometry -> mizu_mors_geometry).
  # 5. The region header include resolves to the assembled header.
  # 6. ALTREP class names + registering package: a loaded mizu keeps its own
  #    classes (class identity is name + package + DllInfo).
  # 7. Error message prefix.
  sed -i.bak \
    -e 's|mizu_shm_create_heap|mori_shm_create_heap|g' \
    -e 's|mizu_shm_open_heap|mori_shm_open_heap|g' \
    -e 's|mizu_shm_close_stack|mori_shm_close_stack|g' \
    -e 's|mizu_shm_host_release|mori_shm_host_release|g' \
    -e 's|mizu_shm_reap|mori_shm_reap|g' \
    -e 's|mizu_err_describe|mori_err_describe|g' \
    -e 's|mizu_shm_s|mori_shm_s|g' \
    -e 's|MIZU_MAGIC_|MORI_MAGIC_|g' \
    -e 's|MIZU_HEADER_SIZE|MORI_HEADER_SIZE|g' \
    -e 's|MIZU_NAME_MAX|MORI_NAME_MAX|g' \
    -e 's|MIZU_PREFIX_LITERAL|MORI_PREFIX_LITERAL|g' \
    -e 's|"vendor/libmizu/internal\.h"|"mori_region.h"|' \
    -e 's|[[:<:]]mizu_shm[[:>:]]|mori_shm|g' \
    -e 's|mizu_mizs_geometry|mori_mors_geometry|g' \
    -e 's|mizu_mizs_geom|mori_mors_geom|g' \
    -e 's|mizu_mizh_validity_set|mori_morh_validity_set|g' \
    -e 's|mizu_mizl_entry|mori_morl_entry|g' \
    -e 's|mizu_mizl_elem|mori_morl_elem|g' \
    -e 's|mizu_ext_mizl_ent|mori_ext_morl_ent|g' \
    -e 's|mizu_ext_na_real_bits|mori_ext_na_real_bits|g' \
    -e 's|mizu_na_build|mori_na_build|g' \
    -e 's|mizu_type_e|mori_type_e|g' \
    -e 's|MIZU_HDR_VALID_OFF|MORI_HDR_VALID_OFF|g' \
    -e 's|MIZU_HDR_VALID_COUNT|MORI_HDR_VALID_COUNT|g' \
    -e 's|MIZU_MIZL_S4|MORI_MORL_S4|g' \
    -e 's|MIZU_NA_INT32|MORI_NA_INT32|g' \
    -e 's|MIZU_NA_INT64|MORI_NA_INT64|g' \
    -e 's|MIZU_TYPE_|MORI_TYPE_|g' \
    -e 's|MIZU_INTEROP_MAGIC|MORI_INTEROP_MAGIC|g' \
    -e 's|MIZU_OPEN_CACHE_MAX|MORI_OPEN_CACHE_MAX|g' \
    -e 's|mizu_ext\.h|mori_region.h|g' \
    -e 's|MIZH|MORH|g' \
    -e 's|MIZS|MORS|g' \
    -e 's|MIZL|MORL|g' \
    -e 's|mizh_|morh_|g' \
    -e 's|mizs_|mors_|g' \
    -e 's|"mizu_list"|"mori_list"|g' \
    -e 's|"mizu_real"|"mori_real"|g' \
    -e 's|"mizu_integer"|"mori_integer"|g' \
    -e 's|"mizu_logical"|"mori_logical"|g' \
    -e 's|"mizu_raw"|"mori_raw"|g' \
    -e 's|"mizu_complex"|"mori_complex"|g' \
    -e 's|"mizu_string"|"mori_string"|g' \
    -e 's|, "mizu", dll)|, "mori", dll)|g' \
    -e 's|"mizu: |"mori: |g' \
    "$DEST/mori.h" "$DEST/altrep.c" "$DEST/serialize.c"

  rm -f "$DEST/mori.h.bak" "$DEST/altrep.c.bak" "$DEST/serialize.c.bak"

  # The gate: no mizu-namespace reference may remain (a missed identifier
  # fails loud at link time; a missed string forks the namespace silently).
  if grep -nE 'mizu_view_|MIZU_VIEW_|mizu_shm|MIZU_MAGIC_|MIZU_HEADER_SIZE|MIZU_NAME_MAX|MIZU_PREFIX_LITERAL|MIZU_TYPE_|MIZU_INTEROP_MAGIC|MIZU_OPEN_CACHE_MAX|MIZU_HDR_|MIZU_NA_|MIZU_MIZL|mizu_mizs|mizu_mizh|mizu_mizl|mizu_ext_|mizu_na_build|mizu_type_e|mizu_ext\.h|libmizu|view\.h|MIZH|MIZS|MIZL|mizh_|mizs_|mizl_|"mizu_list"|"mizu_real"|"mizu_integer"|"mizu_logical"|"mizu_raw"|"mizu_complex"|"mizu_string"|"mizu: |, "mizu", dll\)' \
      "$DEST/mori.h" "$DEST/altrep.c" "$DEST/serialize.c"; then
    echo "vendor: FAIL — mizu-namespace remnants above (view layer)" >&2
    exit 1
  fi

  echo "vendored mizu @ $mizu_ref ($mizu_commit, $mizu_date) into $DEST"
fi
