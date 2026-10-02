# AGENTS.md

Guidance for AI coding agents working **on** the mori package. mori —
Shared Memory for R Objects: POSIX shared memory (Linux, macOS) / Win32
file mappings (Windows) + R’s ALTREP framework let processes on one
machine read the same physical pages. No external dependencies; requires
R \>= 4.3.0 (ALTLIST). R API (all thin `.Call` wrappers; logic is
C-level): [`share()`](https://mori.r-lib.org/dev/reference/share.md),
[`map_shared()`](https://mori.r-lib.org/dev/reference/map_shared.md),
[`shared_name()`](https://mori.r-lib.org/dev/reference/shared_name.md),
[`is_shared()`](https://mori.r-lib.org/dev/reference/is_shared.md),
[`prune_shared()`](https://mori.r-lib.org/dev/reference/prune_shared.md).
ALTREP serialization hooks emit the
[`shared_name()`](https://mori.r-lib.org/dev/reference/shared_name.md)
identifier as the wire form — transparent under
[`serialize()`](https://rdrr.io/r/base/serialize.html) and `mirai`.

Claude Code users: add `.claude/CLAUDE.md` containing `@../AGENTS.md`
(`.claude/` is gitignored).

## Commands

``` r

devtools::test()                                    # full suite (testthat ed 3)
testthat::test_file("tests/testthat/test-nested.R") # single test file
devtools::document()                                # roxygen2 -> man/, NAMESPACE
rmarkdown::render("README.Rmd")                     # rebuild README.md
```

``` bash
R CMD build .
R CMD check --no-manual --compact-vignettes=gs+qpdf mori_*.tar.gz   # matches CI
bash tools/vendor.sh                    # re-vendor both layers: region (shm.c, err_tls.c, mori_region.h) from libmizu, view (mori.h, altrep.c, serialize.c) from mizu
bash tools/vendor.sh --region-only      # region layer only; --view-only for the view layer; --mizu/--libmizu override the refs
```

- `Config/build/compilation-database: true` in DESCRIPTION →
  `compile_commands.json` on install (gitignored); clangd works out of
  the box.
- R’s in-source build ignores header dependencies: after re-vendoring,
  `rm src/*.o src/*.so` before reinstalling — stale objects linked
  against old struct layouts cause sizeof mismatches and garbage fields.
- `src/shm.c`, `src/err_tls.c` and `src/mori_region.h` are vendored from
  libmizu by `tools/vendor.sh` (pinned ref, enumerated substitutions,
  self-grep gate) — never edit them by hand; changes go upstream to
  libmizu and are pulled by re-running the script. `mori_region.h` also
  carries the carved wire-format helpers (`mori_mors_geometry`,
  `mori_morh_validity_set`, `mori_na_build`, the MIZL directory-entry
  reader `mori_morl_elem` and its chain, the type tags) the view layer
  resolves onto.
- `src/mori.h`, `src/altrep.c` and `src/serialize.c` — the view layer:
  the ALTREP consumer classes, the MORH/MORS/MORL layout oracle/writer,
  and the exact-size serialize streams — are vendored from **mizu** by
  the same script (pinned ref, the reverse substitution set, self-grep
  gate). The layouts have a single home: the canonical layer is
  first-class code in mizu and mori tracks it, so the byte format cannot
  drift between the two packages. Never edit these files by hand either;
  changes go upstream to mizu. Unless `--libmizu` is given, the region
  layer’s ref is derived from the libmizu pin recorded in mizu’s own
  `src/vendor/libmizu/VENDOR` — the pairing mizu was built against.

## Storage Model

- **Zero-copy (SHM-backed)**: atomic vectors (any type/attributes) and
  data frame columns are written into SHM and ALTREP-backed on
  consumers; attributes serialize into a trailing region, reapplied on
  open. Numeric `Dataptr_or_null` returns the SHM pointer;
  `Dataptr(writable=TRUE)` triggers COW into a private copy. Strings
  live in an Arrow large_utf8-shaped block (validity bitmap + i64
  offsets + encoding bytes + packed data); `Elt` creates CHARSXPs
  lazily; `Dataptr_or_null` returns NULL to force per-element access.
- **Validity bitmaps**: MORS string blocks carry one always — it is the
  NA test at string `Elt` (strings have no sentinel). The MORH/MORL
  header sections \[40-55\] are cross-language surface only: three
  states ({0,0} absent, {0,-1} known-NA-free, else bitmap offset + null
  count), built solely by the foreign staging mode (`foreign = 1`,
  embedders); the host path leaves them absent (NAs ride the sentinels)
  and no mori reader consults them.
- **int64**: class-only `integer64` vectors (REALSXP storage, no S4,
  non-ALTREP; the `_any` test admits ALTREP storage) ride the wire as
  tag 32 (`MORI_TYPE_INT64`) in the header sexptype / directory entry —
  no attrs blob (the tag carries the class).
- **Nested lists (zero-copy)**: VECSXP/LISTSXP elements are written
  inline as a complete child MORL region at their `data_offset`, each
  level wrapped in its own ALTLIST. Sub-lists are
  [`is_shared()`](https://mori.r-lib.org/dev/reference/is_shared.md)-TRUE
  with path-bearing
  [`shared_name()`](https://mori.r-lib.org/dev/reference/shared_name.md)s;
  `map_shared(shared_name(sub))` opens them directly. The OS region name
  is the prefix before `[` (`sub("\\[.*$", "", shared_name(x))`).
- **Pass-through**: everything else (environments, closures, language,
  NULL) is returned unchanged; no SHM created.

## Concurrency Model

**Write-once on host, read-many on consumers, COW for any mutation.**

- Each [`share()`](https://mori.r-lib.org/dev/reference/share.md)
  allocates a fresh region; existing regions are never mutated. No
  locking anywhere — any change admitting in-place mutation breaks the
  model.
- Names embed creator PID + randomly-seeded per-process counter
  (`mori_<pid>_<counter>`, `mori_region_name`). `mori_shm_create` uses
  `O_EXCL` / `ERROR_ALREADY_EXISTS`; a collision (orphan of a crashed
  same-PID process) is an error (`MORI_ERRCAT_EXISTS`), never worked
  around by reuse.
- [`prune_shared()`](https://mori.r-lib.org/dev/reference/prune_shared.md)
  clears such orphans; it must run while the PID is free — a live
  process cannot reap its own names.
- The host writes the full region before its name is observable —
  partial writes are never seen. Consumer mappings are read-only
  (`PROT_READ` / `FILE_MAP_READ`); mutation triggers COW.
- Fork safety: `mori_shm` records the creator PID; `mori_host_finalizer`
  unlinks only on `getpid()` match, so a forked child’s inherited
  finalizers skip the unlink (its `munmap` is process-local) —
  [`parallel::mclapply`](https://rdrr.io/r/parallel/mclapply.html) forks
  cannot destroy parent regions.

## share() Dispatch (`altrep.c: mori_create`)

0.  Already mori-backed (`mori_view_check`) → return `x` unchanged.
    Idempotence required: re-sharing a sub-list view must not allocate a
    fresh root region (would break
    [`shared_name()`](https://mori.r-lib.org/dev/reference/shared_name.md)).
1.  Otherwise sizes via `mori_layout_size_impl(x, NULL, 0)`
    (`ok = NULL`, `foreign = 0` — no vetoes; non-mori ALTREPs
    materialize through `DATAPTR_RO` at write) and writes via
    `mori_layout_write(base, x, 0)`; 0 size → `x` returned as-is. Type
    ladder: `VECSXP`/`LISTSXP` → MORL (pairlists coerced to VECSXP at
    every level; data frames included), `STRSXP` → MORS, atomics
    (`REALSXP`/`INTSXP`/`LGLSXP`/`RAWSXP`/`CPLXSXP`) → MORH, everything
    else (incl. `NILSXP`) → pass-through.

The result returns via `mori_make_result`, which chains the host extptr
into the ALTREP’s keeper chain (see Internal State and Lifetime).

### Two-pass write invariant

All writers are count-then-write pairs:
`mori_serialize_count`/`mori_serialize_into` (serialize.c, fallback
bytes), `mori_attrs_size`/`mori_attrs_write` (attribute blobs, honoring
the embedder hooks), `mori_nested_size`/`mori_nested_write` (MORL
regions), `morh_size`/`morh_write`, `mors_size`/`mors_write`. **If a
pair disagrees by even one byte, the region is malformed and consumers
read garbage.** Mirror every change in both sides. Exception by design:
the foreign staging mode (`foreign = 1`) reserves validity-section bytes
in the size pass that a clean (NA-free) write never spends — there the
write returns the actual bytes used, always ≤ the size pass; on the host
path (`foreign = 0`) the two agree exactly.

## Embedder C API

Declared in `mori.h`, C-level only (not `.Call`) — for packages
embedding mori layouts under their own SHM management:

- **Layout oracle + writer**: `mori_layout_size(x, foreign)` → region
  size, or 0 for what the writer must not take (ALTREP nodes that are
  neither mori views nor directly readable with no keeper chain —
  `mori_altrep_readable`; R’s S4 data-part wrappers qualify). The S4
  object bit rides the layouts: header flags word at offset 32
  (`MORI_FLAG_S4`) for MORH/MORS/MORL roots and nested lists, bit 30 of
  the directory entry’s `sexptype` (`MORI_ELEM_S4`) for vector/string
  leaves; applied with `Rf_asS4` after attributes land. Vetoes ride the
  size recursion (`mori_layout_size_impl` with `ok != NULL`); the host
  path passes `NULL` and vetoes nothing.
  `mori_layout_write(base, x, foreign)` returns the actual bytes
  written: exactly `mori_layout_size` when `foreign == 0`, at most that
  in the foreign staging mode (clean leaves’ unspent validity reserve);
  it zeroes reserved header bytes \[24-63\] on every write (embedders
  may recycle regions). `foreign = 1` relaxes the ALTREP veto (admitted
  atomic ALTREPs copy through `*_GET_REGION`, never materializing the
  sender’s vector) and builds the validity-bitmap sections.
- **Wrap constructors**: `mori_vec_wrap` / `mori_str_wrap` /
  `mori_list_wrap` build views over embedder memory, pinning `keeper`
  via the data1 extptr’s protected slot; each takes a `release`
  once-hook (see Internal State). `mori_restore_attrs` reapplies a
  trailing attribute blob: the embedder’s encoding via the read hook
  when the blob’s first byte is the ‘I’ interchange magic
  (`MORI_INTEROP_MAGIC`), else `R_Unserialize`.
- **Introspection + resolve**: `mori_view_check`, `mori_shm_name`,
  `mori_parse_id`, `mori_walk_path` (walks an index path over an open
  region; the caller’s keeper flows into the result’s chain),
  `mori_is_int64` / `mori_is_int64_any`, `mori_resolve_id(id, keeper)`
  (the one identifier resolve path: parse, open-or-cache, wrap root or
  walk, fire the resolve hook; a non-NULL `keeper` composes behind a
  fresh chain terminus), `mori_release_now(x)` (fires a view’s armed
  release hook ahead of GC; once-only).
- **Hooks**: `mori_set_wire_hooks(emit, resolve)` — see Serialization
  Hooks. `mori_set_attrs_hooks(size, write, read)` — embedder
  attribute-blob encoding (e.g. the ‘I’ interchange stream); a zero
  `size` return declines and the blob falls back to an R_Serialize
  stream (a decline after a nonzero size is a bug, never a fallback).
  `mori_set_open_hook` — custom consumer open (e.g. page-0-RW for a
  refcount protocol).

## ALTREP Classes

Registered in `mori_altrep_init`. Numerics
(`mori_real`/`integer`/`logical`/`raw`/`complex`) share the `mori_vec`
pattern (SHM-backed data pointer). `mori_string` (`mori_str`) does lazy
per-element `Elt` via `Rf_mkCharLenCE`: NA from the validity bitmap
(`mori_str_valid`), the span `[offsets[i], offsets[i+1])` bounds-checked
against `str_bytes` (and `INT_MAX`), the encoding byte range-checked;
the two end offsets are checked O(1) at wrap (`mori_str_wrap`).
`mori_list` (`mori_list_view`) has a per-element directory and a lazy
`Elt` cache using `R_NilValue` as the uncached sentinel (a fresh VECSXP
is NIL-filled — zero init work). NIL elements are the sole singleton
output of `mori_unwrap_element`’s fallback path, so they’re never cached
and identity is preserved.

### COW invariant for new ALTREP methods

Any method returning a writable pointer (or mutating) **must check
`R_altrep_data2(x)` first** — if non-null, the vector is materialized
and the SHM pointer is no longer authoritative. New `Dataptr` / coercion
methods routinely forget this. COW materialization also fires the view’s
release hook early (see Internal State).

## ALTREP Serialization Hooks

All classes register `Serialized_state` + `Unserialize`.

- **Wire form (single canonical shape)**: `mori_format_chain` is the
  single source of truth for `mori_shm_name` (the `.Call`) and all
  `Serialized_state` methods — walks the keeper chain collecting
  `view->index` (`>= 0` only), formats `<prefix>` (root) or
  `<prefix>[i1+1,...]` (sub-object). 1-based externalisation cues R’s
  `[[i]]`; the parser converts back.
- **Unserialize**: `mori_Unserialize` (vec/list) and
  `mori_string_Unserialize`. Vec/list: a STRSXP state is always an
  identifier (their fallbacks are never STRSXP) — probed via
  `mori_shm_open_and_wrap`, a miss errors (corrupt stream); other states
  are materialized data. Strings: both forms are string-like, so the
  fallback is wrapped in a length-1 VECSXP (`mori_wrap_string_state`) —
  bare STRSXP = identifier, wrapper = data (unwrapped without probing).
  All identifier resolves route through `mori_resolve_id` (parse,
  open-or-cache through the consumer LRU, wrap the root or walk the
  path, fire the resolve hook); the walk’s intermediates step via
  `mori_make_extptr` (bare keeper-chain extptrs, no ALTLIST/attrs —
  never observed), leaf via `mori_unwrap_element`.
- **Wire hooks** (`mori_set_wire_hooks`, set once at embedder load):
  `emit(view)` fires from `Serialized_state` when an identifier (not a
  materialization) is emitted; `resolve(view, shm)` fires after the wrap
  inside `mori_resolve_id` — not `mori_walk_path`, whose direct callers
  fire their own. Supports a cross-process lifetime protocol: flag
  regions on escape, count remote references on arrival.

Fallback to full materialization when: data2 is set (COW’d), or nesting
exceeds `MORI_MAX_PATH` (64).

### Identifier grammar

    identifier ::= prefix [ "[" int1 ("," int1)* "]" ]
    prefix     ::= MORI_PREFIX_LITERAL hex+ "_" hex+
    hex+       ::= [0-9a-f]+         # lowercase
    int1       ::= [1-9][0-9]*       # 1-based, no leading zeros

`MORI_PREFIX_LITERAL` (mori.h: `/mori_` POSIX, `Local\\mori_` Windows)
is the **single source of truth shared by shm.c’s name-format strings
and the parser’s `memcmp`** — change once, both sides stay in sync.
Variable-width hex; `MORI_NAME_MAX = 30` bounds the prefix on both
platforms. `mori_parse_id` is bounded and single-pass
(`MORI_IDENTIFIER_MAX`); indices stored 0-based; the path loop uses NUL
as sentinel (every fail-branch excludes `\0`). Sizing invariant:
`MORI_NAME_MAX + 2 + 11 × MORI_MAX_PATH < MORI_FORMAT_BUFLEN`.

### Validation contract

`map_shared` and unserialize share `mori_parse_id` for shape validation
but differ in failure mode: **malformed → `NULL`** (wrong type/length,
`NA`, bad prefix/path); **well-formed but unmappable → error** (missing
region, bad magic, truncated header, OOB index, non-VECSXP intermediate,
fields inconsistent with mapped size). Consumer validation is total —
corrupt input errors, never reads out of bounds: root headers vs mapped
size (`mori_open_vector`/`mori_open_string`), the format flags word at
every header read (`mori_flags_known` — an unknown set bit rejects as
corrupt-or-newer), directory entries vs data region incl. the 64-byte
alignment of `data_offset` (`mori_unwrap_element`), string blocks vs
region (`mori_str_wrap`, O(1) on the two end offsets), spans at `Elt`.
Preserve this split in `mori_shm_open_and_wrap` — collapsing it either
way breaks the probe-vs-corruption distinction.

## SHM Region Layouts

Magic in the first 4 bytes (`MORI_MAGIC_*`): MORH `0x4D4F5248` vector,
MORL `0x4D4F524C` list, MORS `0x4D4F5253` string. Every layout opens
with a 64-byte header; bytes \[24-31\] are reserved for embedder
cross-process state, \[32-35\] hold the format flags word (bit 0: S4
object bit; any other set bit rejects the region as corrupt-or-newer —
`mori_flags_known`), \[36-39\] are reserved, \[40-47\]/\[48-55\] hold
the optional validity section for MORH and MORL (i64 offset, i64 null
count: {0,0} absent, {0,-1} known-NA-free, else the offset of a
64-byte-aligned bitmap section; MORS carries its bitmap in the string
block instead), \[56-63\] are reserved (all zeroed on every write —
embedders may recycle regions). Tables are the canonical spec;
`mori_nested_write` / `morh_write` / `mors_write` (with
`mori_attrs_write` or `mori_serialize_into` for attrs and fallbacks) are
the implementations. Attributes are serialized R objects (pairlist on R
\< 4.6, named list otherwise) unless the embedder attrs hooks supply
another encoding; `mori_restore_attrs` reapplies them on the consumer.

**MORH — atomic vector.** Data at byte 64 (64-byte aligned for SIMD);
trailing attrs after the data.

| Offset | Size | Field |
|----|----|----|
| 0 | 4 | magic |
| 4 | 4 | sexptype (`MORI_TYPE_INT64` = 32 for class-only integer64 — no attrs) |
| 8 | 8 | length (int64) |
| 16 | 8 | attrs_size (int64, 0 if none) |
| 24 | 8 | reserved (zero) — embedder cross-process state |
| 32 | 4 | flags (bit 0: S4 object bit) |
| 36 | 4 | reserved (zero) |
| 40 | 8 | validity bitmap offset (0 = absent / known-NA-free) |
| 48 | 8 | null count (0 or -1 when offset is 0) |
| 56 | 8 | reserved (zero) |
| 64+ |  | raw vector data |
| 64 + length×elt_size |  | serialized attributes (if `attrs_size` \> 0), then the optional validity bitmap (foreign writes) |

**MORL — ALTLIST.** Header + element directory + per-element data
regions.

| Offset | Size | Field |
|----|----|----|
| 0 | 4 | magic |
| 4 | 4 | n_elements (int32) |
| 8 | 8 | attrs_offset (int64) |
| 16 | 8 | attrs_size (int64) |
| 24 | 8 | reserved (zero) — embedder cross-process state |
| 32 | 4 | flags (bit 0: S4 object bit) |
| 36 | 4 | reserved (zero) |
| 40 | 8 | validity table offset (0 = absent / known-NA-free) |
| 48 | 8 | total null count across leaves (0 or -1 when offset is 0) |
| 56 | 8 | reserved (zero) |
| 64 | 32×n | element directory |
| varies |  | element data (each 64-byte aligned), then serialized attributes, then the optional validity tail (foreign writes) |

Directory entry (32 bytes):
`data_offset(8, 64-byte aligned — enforced at read) + data_size(8) + sexptype(4) + attrs_size(4) + length(8)`.
`sexptype`: `0` → serialized bytes (serialize.c); `STRSXP` → a string
block (the MORS body layout) at `data_offset`; `VECSXP` → nested MORL
region inlined at `data_offset` of size `data_size` (child
header/directory/elements/attrs all inline; parent’s `attrs_size` always
0 for VECSXP children); `MORI_TYPE_INT64` (32) → class-only integer64
leaf (the tag carries the class — no attrs blob); `33` → remote leaf
(the column lives in another region: `data_offset`/`data_size` span its
identifier — name with optional index path, 1-255 bytes — resolved by
reference through the consumer open + path walk, with the entry’s
length/attrs/NA claims cross-checked against the referenced region;
read-side only, no mori writer emits it, and the S4 bit is never set);
other → raw zero-copy data, with bit 30 of `sexptype` (`MORI_ELEM_S4`)
flagging an S4 leaf (masked off at read). Non-VECSXP attrs sit at
`data_offset + data_size - attrs_size`. The foreign-mode validity tail:
one LSB-first bitmap per NA-capable atomic leaf (1 = present, spent only
where NAs are present), then an n-entry table of {i64 offset, i64 null
count} pairs the header’s validity words point at; a VECSXP/STRSXP
leaf’s pair is {0,0} (the nested header / string block carries its own).

**MORS — ALTSTRING.** Header + string block + optional trailing attrs.

| Offset | Size | Field |
|----|----|----|
| 0 | 4 | magic |
| 4 | 4 | attrs_size (int32, 0 if none) |
| 8 | 8 | n_strings (int64) |
| 16 | 8 | str_data_size (int64: the string block’s byte size, incl. padding) |
| 24 | 8 | reserved (zero) — embedder cross-process state |
| 32 | 4 | flags (bit 0: S4 object bit) |
| 36 | 28 | reserved (zero) |
| 64 | str_data_size | the string block |
| 64 + str_data_size |  | serialized attributes (if `attrs_size` \> 0) |

The string block (also the form of a STRSXP element in MORL regions, at
the entry’s `data_offset`; element attrs use the directory entry’s
`attrs_size`): four sections, each 64-byte aligned from the block start
— **validity bitmap** (ceil(n/8) bytes, LSB first: 1 = present, 0 = NA),
**offsets** ((n+1) int64; `offsets[0]` = 0, non-decreasing; string i is
`data[offsets[i], offsets[i+1])`, an NA spans zero bytes), **encoding**
(n bytes; cetype_t: 0=native, 1=UTF-8, 2=Latin-1, 3=bytes), then the
**packed string bytes** (no terminators). The first, second and fourth
are Arrow large_utf8’s three buffers verbatim; the encoding section is
R’s per-CHARSXP mark. Section offsets come from one geometry function
(`mori_mors_geometry` in mori_region.h, wrapped by `mori_str_geometry`
in mori.h). The block carries its own validity bitmap — MORS headers
have no \[40-55\] section.

## Internal State and Lifetime

SHM lifetime is automatic via chained extptr finalizers. Three interned
tags (C globals in altrep.c):

- **`mori_shm_tag`** — SHM mapping extptr (both sides). Addr
  `mori_shm *`; finalizer `munmap`.
- **`mori_host_tag`** — host-only unlink extptr from `mori_make_result`;
  addr `mori_shm *` (name on POSIX, HANDLE on Windows); finalizer
  `shm_unlink`/`CloseHandle`, skipped when creator `pid` ≠ `getpid()`
  (fork guard). Chained above the shm extptr.
- **`mori_owned_tag`** — every malloc-backed extptr: ALTREP data1
  (view/vec/str) and bare path-walk intermediates (`mori_make_extptr`,
  always `mori_list_view *`). Addr type at the ALTREP boundary from
  `TYPEOF(x)`: `VECSXP → mori_list_view *`, `STRSXP → mori_str *`, else
  `mori_vec *`. `index` field: -1 standalone/root, \>= 0 element.
  Finalizer `mori_owned_finalizer`: release hook once, then `free`.

**Release hook**: every owned struct embeds `mori_owned` (`release` +
`release_arg`) as its first member, so the finalizer recovers it from
any addr. Fires exactly once (`mori_release_once`’s NULL store) — at COW
materialization or the finalizer, whichever first; internal callers pass
NULL. ALTLIST views fire at the finalizer only: extracted elements keep
referencing the region.

[`is_shared()`](https://mori.r-lib.org/dev/reference/is_shared.md) =
`mori_view_check` (ALTREP with `mori_owned_tag` data1).
`mori_shm_name()` = `mori_format_chain` (bare prefix for roots, path
form for sub-objects).

### Keeper chain

- **Host**: `mori_shm_create` mmaps (POSIX fd closed after mmap);
  `mori_make_result` splits ownership — ALTREP gets the mapping
  (`mori_shm_tag` extptr), the host unlink extptr is chained as its
  `prot`. Both finalizers run at GC; `R_RegisterCFinalizerEx(..., TRUE)`
  covers session exit.
- **Consumer**: `mori_shm_open` maps read-only (never unlinks).
  Prefix-opened lists: shm extptr → root view extptr (index -1) → data1;
  `Elt` sub-lists chain through parent views. Path-form: intermediates
  are bare extptrs, only the leaf gets an ALTLIST. Element vec/str
  extptrs’ `prot` is the parent view (shm extptr at root). The resolve
  paths (`map_shared`, the ALTREP Unserialize methods) dedupe consumer
  mappings through a process-global name-keyed LRU cache
  (`MORI_CACHE_MAX` = 16 slots, the vendored `mori_open_consumer`;
  eviction drops the cache’s reference, never unmaps under a live view):
  repeated opens of one region share one mapping, which then lives until
  cache eviction + the last view over it is GC’d, and stays read-only (a
  write attempt COWs). Caveat: an unlinked name still resolves while
  cached (stale-but-valid pages instead of “not found”) — this softens
  [`prune_shared()`](https://mori.r-lib.org/dev/reference/prune_shared.md)
  finality until eviction.
- **Lifetime**: leaves pin the root SHM through the chain (leaf → views
  → shm → host); every `R_MakeExternalPtr` retains its `prot`, so the
  chain survives GC of enclosing ALTLISTs. `mori_format_chain` is the
  only chain walker (`mori_owned_tag` hops, always `mori_list_view *`).

## Code Organization

The view layer (`src/mori.h`, `src/altrep.c`, `src/serialize.c`) is
vendored from mizu and the region layer (`src/shm.c`, `src/err_tls.c`,
`src/mori_region.h`) from libmizu, both by `tools/vendor.sh` — all
generated, never edited here; the bullets describe the vendored content.

- **src/mori.h** — types (`mori_shm` incl. creator `pid`, `mori_buf`,
  `mori_owned`, `mori_vec`, `mori_list_view`, `mori_str_geom`), embedder
  API declarations (incl. `mori_set_open_hook` / `mori_set_attrs_hooks`
  / `mori_set_wire_hooks`, `mori_resolve_id`, `mori_release_now` — mori
  itself registers none of the hooks), constants (`MORI_MAGIC_*`,
  `MORI_HEADER_SIZE`, `MORI_TAG_*`, grammar bounds,
  `MORI_PREFIX_LITERAL`, `MORI_CACHE_MAX`,
  `MORI_FLAGS_OFF`/`MORI_FLAG_S4`, `MORI_ELEM_S4`, `MORI_TYPE_INT64`),
  `MORI_ALIGN64`, `mori_sizeof_elt`, `mori_flags_known`,
  `mori_str_geometry`/`mori_str_valid` (wrapping the region header’s
  `mori_mors_geometry`).
- **src/mori_region.h** — assembled from carved libmizu sections:
  status/error enums, region magics + header layout constants
  (incl. `MORI_HDR_FLAGS_OFF`/`MORI_HDR_FLAG_S4`,
  `MORI_HDR_VALID_OFF`/`MORI_HDR_VALID_COUNT`), wire type tags
  (`mori_type_e`, `MORI_TYPE_INT64`) and NA sentinels, binding-tier
  constants (`MORI_ZC_FLOOR`, `MORI_OPEN_CACHE_MAX`,
  `MORI_INTEROP_MAGIC`, the language/capability registries), the
  `mori_shm` struct + region API decls, and the carved layout helpers
  the view layer delegates to (`mori_mors_geometry`,
  `mori_morh_validity_set`, `mori_na_build`, and the bounds-checked MIZL
  directory-entry reader `mori_morl_elem` with its `mori_morl_entry`
  struct and `mori_ext_morl_ent` / `mori_ext_morl_tag_ok` /
  `mori_ext_valid_ok` / `mori_ext_flags_known` / `mori_type_elt_size`
  chain — `MORI_EXT_INLINE` static inlines over `MORI_REGION_ALIGN64`
  arithmetic).
- **src/shm.c** — platform SHM create/open/close + finalizers.
  `mori_shm_reap` enumerates the platform name source (`/dev/shm` on
  Linux; per-user registry dir on macOS — see Platform Notes),
  classifies by embedded PID (`mori_pid_alive`), feeds
  `mori_reap_unlink`; a dead-PID region is reported removed only when
  `shm_unlink` actually reclaimed it (concurrent reaps never
  double-report). Windows / other POSIX can’t reap. `mori_shm_os_unlink`
  is the single unlink seam. Errors: `mori_err_classify` maps native
  errno/GetLastError → `MORI_E*` **before** any close/unlink clobbers
  it; `mori_err_describe` → summary + remediation hint.
- **src/err_tls.c** — the thread-local error slot behind
  `mori_last_error_category` / `mori_last_error_message` for handle-free
  entry points (regions, prune).
- **src/serialize.c** — `mori_serialize_count` / `mori_serialize_into` /
  `mori_unserialize_from`: fallback MORL entries (`sexptype == 0`) and
  (unless the attrs hooks take them) attributes.
- **src/altrep.c** — everything else: ALTREP classes, `mori_create`,
  layout dispatchers + size/write pairs (incl. the foreign staging mode:
  validity sections, `*_GET_REGION` copies of admitted ALTREP atomics,
  the integer64 leaf gate), wrap constructors, consumer open+wrap,
  `mori_resolve_id` + path walk, identifier formatter/parser,
  serialization + wire hooks, `mori_altrep_init`. Create failures:
  `mori_shm_create_failed` composes size (`mori_format_bytes`) +
  `mori_err_describe` into one `Rf_error`.
- **src/init.c** — `R_init_mori`; 5 `.Call` entries (names match C
  functions; all take one `SEXP` except `mori_prune`).
- **R/** — `mori-package.R` (docs), `share.R` (the five wrappers).
  `import-standalone-defer.R` is vendored from withr
  (`usethis::use_standalone("r-lib/withr", "defer")`) — don’t edit by
  hand.

## Testing

testthat edition 3; `tests/testthat.R` entry point;
`tests/testthat/test-*.R` grouped by topic. Nothing gated behind
`skip_on_cran` or env vars. `test-corruption.R` hand-crafts malformed
regions byte-by-byte and is layout-sensitive by design — it must track
the layouts above (directory entries 64-byte aligned; the string block
in the Arrow-shaped form via its `str_block` helper). Those cases run
only on Linux (file-backed `/dev/shm`), so macOS local runs skip them —
CI’s Ubuntu jobs are the ones that exercise them.

## Platform Notes

- **Linux**: `/dev/shm` tmpfs via
  [`open()`](https://rdrr.io/r/base/connections.html)/[`unlink()`](https://rdrr.io/r/base/unlink.html)
  directly (avoids the `-lrt` dependency). `MAP_POPULATE` pre-faults.
- **macOS**: `shm_open`/`shm_unlink` (libc); `MAP_POPULATE` is a no-op.
  The kernel namespace can’t be enumerated (invisible once the creator
  dies), so reaping uses a per-user registry under `TMPDIR`: **one
  append-only log per process** (`<dir>/mori_<pid>`, not a file per
  region): each created region recorded as one 4-byte binary record
  holding its name counter (the pid comes from the filename) — one
  [`write()`](https://rdrr.io/r/base/write.html) ~1 µs vs ~40 µs file
  creation, opened lazily on first share by `mori_log_append` (sole
  creator; retries through `mkdir` on `ENOENT`). Single-writer ⇒ no
  locking; readers are reapers of dead PIDs. `mori_log_release` tracks a
  live-region count; at zero it unlinks the log and `rmdir`s the dir.
  The count is incremented even when logging fails (forfeits
  reapability, never the region). Fork-safe via `mori_log_fork_guard`
  (reopens on PID change). A
  [`write()`](https://rdrr.io/r/base/write.html) is cross-process
  visible without `fsync`, lost only on reboot. All log ops best-effort.
- **Windows**: page-file-backed `CreateFileMappingA`/`MapViewOfFile`.
  The host must keep the mapping handle alive until consumers open it
  (the GC-chained host extptr does).

## Package Conventions

- roxygen2 (markdown); NAMESPACE auto-generated. MIT license. Version
  `major.minor.patch.dev` (dev tag `.9000`).
- `README.md` is generated from `README.Rmd` — edit the `.Rmd` and
  re-knit, never `README.md`.
- `AGENTS.md`, `.claude/`, `.posit/` are in `.Rbuildignore`.
