# AGENTS.md

Guidance for AI coding agents working **on** the mori package. mori —
Shared Memory for R Objects. Uses POSIX shared memory (Linux, macOS) and
Win32 file mappings (Windows) with R’s ALTREP framework to let multiple
processes on the same machine read the same physical memory pages. No
external dependencies. Requires R \>= 4.3.0 (for ALTLIST). API:
[`share()`](https://shikokuchuo.net/mori/dev/reference/share.md) →
ALTREP shared object,
[`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
→ open SHM by name,
[`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
→ extract identifier,
[`is_shared()`](https://shikokuchuo.net/mori/dev/reference/is_shared.md)
→ test if shared,
[`prune_shared()`](https://shikokuchuo.net/mori/dev/reference/prune_shared.md)
→ remove orphaned regions of dead processes. ALTREP serialization hooks
emit the
[`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
identifier as the wire form — transparent under
[`serialize()`](https://rdrr.io/r/base/serialize.html) and `mirai`.

Claude Code users: add `.claude/CLAUDE.md` containing `@../AGENTS.md` to
import this file (`.claude/` is gitignored).

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
```

- `Config/build/compilation-database: true` in DESCRIPTION makes R
  generate `compile_commands.json` on install, so clangd C navigation
  works out of the box (gitignored).
- R’s in-source build does not track header dependencies: after editing
  `mori.h`, `rm src/*.o src/*.so` before reinstalling — otherwise stale
  objects compiled against the old struct layouts get linked in (sizeof
  mismatches, garbage fields).

## Storage Model

- **Zero-copy (SHM-backed)**: All atomic vectors (any type, any
  attributes) and data frame columns are written directly into SHM and
  backed by ALTREP on consumers. Attributes serialize into a trailing
  region and are reapplied on the consumer. Numeric `Dataptr_or_null`
  returns the SHM pointer; `Dataptr(writable=TRUE)` triggers COW into a
  private copy. String `Elt` lazily creates CHARSXPs; `Dataptr_or_null`
  returns NULL to force per-element access.
- **Nested lists (zero-copy)**: VECSXP/LISTSXP elements of a shared list
  are written inline as a complete child MORL region at their
  `data_offset`; each level wraps in its own ALTLIST.
  [`is_shared()`](https://shikokuchuo.net/mori/dev/reference/is_shared.md)
  returns TRUE for sub-lists;
  [`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
  returns a path-bearing identifier; `map_shared(shared_name(sub))`
  opens the addressed sub-list / element directly. The OS-level region
  name is the prefix before `[` and is recoverable via
  `sub("\\[.*$", "", shared_name(x))`.
- **Pass-through**: All other R objects (environments, closures,
  language objects, NULL) are returned unchanged. No SHM is created.

## Concurrency Model

mori is **write-once on host, read-many on consumers, COW for any
mutation.** Each
[`share()`](https://shikokuchuo.net/mori/dev/reference/share.md)
allocates a new SHM region with a unique name; existing regions are
never mutated. The name embeds the creator PID (`mori_<pid>_<counter>`);
the counter starts at a per-process random value (seeded on first use in
`mori_shm_name`), so colliding with a dead same-PID incarnation’s names
additionally requires a ~2^-32 counter overlap; `mori_shm_create` opens
with `O_EXCL` (POSIX) / checks `ERROR_ALREADY_EXISTS` (Windows); a name
that already exists — an orphan left by a crashed same-PID process — is
surfaced as an error (`MORI_EEXIST`), never worked around by reusing or
mutating the existing region.
[`prune_shared()`](https://shikokuchuo.net/mori/dev/reference/prune_shared.md)
is the mechanism for clearing such orphans; it must run while the PID is
free (before reuse), since the colliding process reads its own PID as
alive and so cannot reap them itself. The host writes the full region
inside `mori_create` before returning, and consumers obtain the name
only after that call returns — partial writes are not observable.
Consumer mappings are read-only (`PROT_READ` / `FILE_MAP_READ`);
mutations trigger COW into private memory. No locking is implemented or
required; any future change admitting in-place mutation breaks this
model. Fork safety: `mori_shm` records the creator PID at create time
and `mori_host_finalizer` unlinks only when it matches `getpid()` — a
fork()ed child’s inherited finalizers skip the unlink (the child’s
`munmap` via `mori_shm_finalizer` is process-local and safe), so
[`parallel::mclapply`](https://rdrr.io/r/parallel/mclapply.html)-style
forks cannot destroy a parent’s live regions; regions orphaned by a dead
parent remain reapable by
[`prune_shared()`](https://shikokuchuo.net/mori/dev/reference/prune_shared.md)
as usual.

## share() Dispatch (`altrep.c: mori_create`)

All R exported functions are single `.Call` wrappers. `mori_create`
dispatches on `TYPEOF(x)`:

0.  Already a mori-backed ALTREP (`mori_owned_tag` on
    `R_altrep_data1(x)`) → return `x` unchanged. Idempotence required:
    re-sharing a sub-list view must not allocate a fresh root region
    (would break
    [`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)).
1.  `NILSXP` → returned as-is.
2.  `VECSXP`/`LISTSXP` → `mori_shm_create_list_call` — ALTLIST with
    per-element directory. Pairlists are coerced to VECSXP at every
    level. Data frames go through this path.
3.  `STRSXP` → `mori_shm_create_string_call` — ALTSTRING with offset
    table + packed strings.
4.  SHM-eligible atomics (`REALSXP`, `INTSXP`, `LGLSXP`, `RAWSXP`,
    `CPLXSXP`) → `mori_shm_create_vector_call`.
5.  Everything else → returned as-is.

Each path returns via `mori_make_result`, which chains the host extptr
into the ALTREP’s keeper chain (see Internal State and Lifetime).

### Two-pass write invariant

Writing into SHM is count-then-write in two flavours:
`mori_serialize_count` / `mori_serialize_into` (serialize.c, fallback
serialized bytes) and the recursive `mori_nested_size` /
`mori_nested_write` (altrep.c, whole MORL regions). **If the count and
the writer disagree by even one byte, the SHM region is malformed and
consumers will read garbage.** Any change to either side must be
mirrored in the other.

## ALTREP Classes

Registered in `mori_altrep_init`. Numeric types — `mori_real`,
`mori_integer`, `mori_logical`, `mori_raw`, `mori_complex` — share the
`mori_vec` pattern with a SHM-backed data pointer. `mori_string` uses
`mori_str` with lazy per-element access via `Elt` and `Rf_mkCharLenCE`;
`mori_str` records the packed string area size (`str_bytes`), and each
offset-table entry is bounds-checked against it (and the encoding
range-checked) at `Elt` time. `mori_list` uses `mori_list_view` with a
per-element directory and a lazy `Elt` cache that uses `R_NilValue` as
the “uncached” sentinel — a fresh VECSXP is naturally R_NilValue-filled,
so cache init is zero work. NIL-valued elements are the sole singleton
output of `mori_unwrap_element`’s fallback path, so they aren’t cached;
re-extraction returns the same `R_NilValue` and identity is preserved
for free.

### COW invariant for new ALTREP methods

Any method returning a writable pointer (or mutating) **must check
`R_altrep_data2(x)` first** — if non-null, the vector has been
materialized to a private copy and the SHM pointer is no longer
authoritative. New `Dataptr` / coercion methods routinely forget this.
`Dataptr_or_null` returns the materialized copy when data2 is set, the
SHM pointer otherwise.

## ALTREP Serialization Hooks

All classes register `Serialized_state` and `Unserialize`.

- **Identifier wire form (single canonical shape)**. `mori_format_chain`
  is the single source of truth used by both `mori_shm_name` (the
  `.Call`) and the three `Serialized_state` methods. Walks the keeper
  chain through `mori_owned_tag` hops collecting `view->index` (`>= 0`
  only) and formats `<prefix>` (root) or `<prefix>[i1+1,...]`
  (sub-object) into a caller-provided stack buffer. `shm->name_len` is
  cached at create/open time (no per-call `strlen`). 1-based
  externalisation cues R’s `[[i]]` semantics; the parser converts back
  to 0-based.
- **Unserialize**. Two methods: the shared `mori_Unserialize` (vec +
  list classes) and `mori_string_Unserialize`. For vec/list, a STRSXP
  state is always an identifier (their materialized fallbacks are atomic
  vectors / VECSXP, never STRSXP) — it is probed via
  `mori_shm_open_and_wrap` and a miss errors (corrupt stream); any other
  state is materialized data returned as-is. For strings both forms are
  string-like, so the materialized fallback is wrapped in a length-1
  VECSXP (`mori_wrap_string_state`): a bare STRSXP state is always an
  identifier, and the wrapper — whose content could itself parse as an
  identifier — is unwrapped without probing. `mori_shm_open_and_wrap`
  runs `mori_parse_identifier`: prefix-only routes through
  magic-dispatch; path form routes through `mori_open_path_c`, which
  walks each intermediate VECSXP directory via `mori_make_view_extptr`
  (bare keeper-chain extptr — no ALTLIST wrapper, no attribute
  restoration, since intermediates are never observed) and calls
  `mori_unwrap_element` at the leaf.

Fallback to full materialization when: - COW-materialized vectors (data2
is set) — returns the materialized copy. - Nesting depth exceeds
`MORI_MAX_PATH` (64) in `mori_format_chain`.

String fallbacks are wrapped in a length-1 VECSXP
(`mori_wrap_string_state`) to keep the wire forms disjoint; vec/list
fallbacks need no wrapper (their states are never STRSXP).

### Identifier grammar

    identifier ::= prefix [ "[" int1 ("," int1)* "]" ]
    prefix     ::= MORI_PREFIX_LITERAL hex+ "_" hex+
    hex+       ::= [0-9a-f]+         # one or more, lowercase
    int1       ::= [1-9][0-9]*       # 1-based, no leading zeros

`MORI_PREFIX_LITERAL` (in `mori.h`) is `/mori_` (POSIX) / `Local\\mori_`
(Windows) and is the **single source of truth shared between `shm.c`’s
name-format strings** (`MORI_PREFIX_LITERAL "%x_%x"` POSIX,
`MORI_PREFIX_LITERAL "%lx_%x"` Windows) and the parser’s literal
`memcmp`. Change once; both sides stay in sync. Names are variable-width
hex (no zero-padding); `MORI_NAME_MAX = 30` bounds the prefix on either
platform (Windows worst case: `Local\\mori_<8hex>_<8hex>` = 28 chars +
NUL).

`mori_parse_identifier` is bounded and single-pass (length-capped by
`MORI_IDENTIFIER_MAX`); indices are externalised 1-based and stored
0-based. The path-parse loop relies on NUL-termination as a sentinel
(every fail-branch’s predicate excludes `\0`), so trailing `p < eos`
checks aren’t needed there. Sizing invariant:
`MORI_NAME_MAX + 2 + 11 × MORI_MAX_PATH < MORI_FORMAT_BUFLEN`.

### Validation contract

`map_shared` and unserialize share `mori_parse_identifier` for shape
validation but differ in failure mode: **malformed input → `NULL`**
(wrong type/length, `NA`, missing or malformed prefix, malformed
bracketed path); **well-formed identifier that fails to map → error**
(missing region, bad magic, truncated header, OOB path index, non-VECSXP
intermediate, fields inconsistent with the mapped size). Consumer-side
validation is total — corrupt input errors, never reads out of bounds:
root headers are checked against the mapped size (`mori_open_vector` /
`mori_open_string`), directory entries against their data region
(`mori_unwrap_element`: `length × elt_size ≤ data_size − attrs_size`,
`attrs_size >= 0`), string tables must fit their region
(`mori_make_string`), and each offset-table entry is bounds-checked at
`Elt` time. Preserve this split in `mori_shm_open_and_wrap`: collapsing
it either way breaks the probe-vs-corruption distinction.

## SHM Region Layouts

Magic identifier in the first 4 bytes: MORH (`0x4D4F5248`) atomic
vector, MORL (`0x4D4F524C`) ALTLIST, MORS (`0x4D4F5253`) ALTSTRING. The
tables below are the canonical spec; `mori_serialize_into` and
`mori_nested_write` are the implementations.

**MORH — atomic vector.** 64-byte header, data starts at byte 64
(64-byte aligned for SIMD). Trailing serialized attributes (if any)
follow the data.

| Offset | Size | Field |
|----|----|----|
| 0 | 4 | magic (`0x4D4F5248`) |
| 4 | 4 | sexptype (REALSXP, INTSXP, etc.) |
| 8 | 8 | length (int64) |
| 16 | 8 | attrs_size (int64, 0 if no attributes) |
| 24 | 40 | reserved (zero) |
| 64+ |  | raw vector data |
| 64 + length×elt_size |  | serialized attributes (`attrs_size` bytes, if \> 0) |

Attributes are a serialized R object (pairlist on R \< 4.6, named list
otherwise); `mori_restore_attrs` unserializes and reapplies them on the
consumer.

**MORL — ALTLIST.** Header + element directory + per-element data
regions.

| Offset | Size | Field                          |
|--------|------|--------------------------------|
| 0      | 4    | magic (`0x4D4F524C`)           |
| 4      | 4    | n_elements (int32)             |
| 8      | 8    | attrs_offset (int64)           |
| 16     | 8    | attrs_size (int64)             |
| 24     | 32×n | element directory              |
| varies |      | element data (64-byte aligned) |
| varies |      | serialized attributes          |

Each directory entry (32 bytes):
`data_offset(8) + data_size(8) + sexptype(4) + attrs_size(4) + length(8)`.
`sexptype` semantics: `0` → serialized bytes (via serialize.c); `STRSXP`
→ offset table + packed strings at `data_offset`; `VECSXP` → nested MORL
region inlined at `data_offset` of size `data_size` (child’s own header
/ directory / elements / attrs all inline; parent’s `attrs_size` is
always 0 for VECSXP children); other → raw zero-copy data. For
non-VECSXP entries with `attrs_size > 0`, attrs are appended within the
data region at `data_offset + data_size - attrs_size`.

**MORS — ALTSTRING.** 24-byte header + offset table + packed string
bytes + optional trailing attributes.

| Offset | Size | Field |
|----|----|----|
| 0 | 4 | magic (`0x4D4F5253`) |
| 4 | 4 | attrs_size (int32, 0 if none) |
| 8 | 8 | n_strings (int64) |
| 16 | 8 | str_data_size (int64, byte distance from start of offset table to end of packed strings; includes alignment padding) |
| 24 | 16×n | offset table |
| 24 + align64(16×n) |  | packed string bytes |
| 24 + str_data_size |  | serialized attributes (`attrs_size` bytes, if \> 0) |

Each offset table entry (16 bytes):
`str_offset(int64) + str_length(int32) + str_encoding(int32)`.
`str_offset` is relative to the start of the packed string area.
`str_length < 0` means `NA_STRING`. `str_encoding` is a `cetype_t`
(0=native, 1=UTF-8, 2=Latin-1, 3=bytes). The same layout is used inline
for STRSXP elements within ALTLIST regions (offset table starts at the
element’s `data_offset`; element-level attrs use the directory entry’s
`attrs_size` field).

## Internal State and Lifetime

SHM lifetime is fully automatic via chained external pointer finalizers.
Three interned symbols (C globals in `altrep.c`):

- **`mori_shm_tag`** (`Rf_install("mori_shm")`) — tags the SHM mapping
  extptr (host- and consumer-side). Addr is `mori_shm *`; finalizer:
  `munmap`.
- **`mori_host_tag`** (`Rf_install("mori_host")`) — tags the host-only
  unlink extptr created by `mori_make_result`. Addr is `mori_shm *`
  carrying `name` (POSIX) or `HANDLE` (Windows); finalizer: `shm_unlink`
  / `CloseHandle`, skipped when the stored creator `pid` no longer
  matches `getpid()` (fork guard). Sits above the shm terminus in the
  keeper chain.
- **`mori_owned_tag`** (`Rf_install("mori_owned")`) — tags every
  malloc-backed extptr in the keeper chain: each mori ALTREP’s `data1`
  extptr (view, vec, or str) **and** the bare keeper-chain extptrs
  created by `mori_make_view_extptr` for path-walk intermediates (which
  have no ALTREP wrapper but still hold a `mori_list_view *`). Addr type
  for ALTREP data1 is recovered from `TYPEOF(x)` at the ALTREP boundary:
  `VECSXP → mori_list_view *`, `STRSXP → mori_str *`, else `mori_vec *`.
  Bare keeper-chain hops are always `mori_list_view *` (vec/str only
  ever appear as leaves). Standalone-vs-element carried in each struct’s
  `index` field (`-1` standalone/root, `>= 0` element/sub-list);
  redundantly recoverable from `data1.prot`’s tag
  (`shm_tag → standalone`, `owned_tag → element/sub-list`). Common
  finalizer: `mori_owned_finalizer` `free`s the addr.

[`is_shared()`](https://shikokuchuo.net/mori/dev/reference/is_shared.md)
is a pointer comparison against `mori_owned_tag`. `mori_shm_name()`
invokes `mori_format_chain` to emit the bare prefix for roots /
standalones and the path-bearing form (`<prefix>[i1,i2,...]`, 1-based)
for sub-objects.

### Keeper chain

- **Host side (`share`)**: `mori_shm_create` allocates and mmaps the
  region; the POSIX fd is closed after mmap (mapping stays valid).
  `mori_make_result` splits ownership — the ALTREP wrapper gets the
  mapping via the `mori_shm_tag` extptr (finalizer → `munmap`); the host
  extptr (`mori_host_tag`, finalizer → `shm_unlink` / `CloseHandle`) is
  chained as that extptr’s `prot`. Both finalizers run when the ALTREP
  is GC’d. `R_RegisterCFinalizerEx(..., TRUE)` covers session exit.
- **Consumer side (`map_shared` / unserialize)**: `mori_shm_open` maps
  read-only (size via `fstat` / `VirtualQuery`); POSIX fd closed after
  mmap (Windows: handle retained until `UnmapViewOfFile`). The shm
  extptr’s finalizer is `munmap` only — never unlink. For lists opened
  by prefix, the shm extptr’s immediate child is a root view extptr
  (`mori_list_view *`, index=-1) that becomes ALTREP data1; sub-lists
  accessed via `Elt` (index \>= 0) chain through their parent view
  extptr the same way. For path-form `map_shared("/mori_x_y[i,j,...]")`,
  intermediate hops are bare keeper-chain extptrs
  (`mori_make_view_extptr`, no ALTLIST) — only the leaf’s ALTREP exists.
  The chain shape is identical for `mori_format_chain`’s purposes either
  way. Element vectors hold a `mori_vec` / `mori_str` extptr whose
  `prot` is the immediate parent view (or the shm extptr for vec/str at
  the root).
- **Element lifetime**: leaves keep the root SHM alive through the chain
  (leaf → parent view(s) → shm extptr → host extptr). Every
  `R_MakeExternalPtr` retains its `prot`, so the chain survives even if
  enclosing ALTLIST(s) are GC’d. `mori_format_chain` is the only chain
  walker: it walks `mori_owned_tag` hops (always `mori_list_view *` —
  vec/str extptrs are only ever leaves), collecting view indices
  (skipping the root view’s -1) until it hits the shm extptr.

## Code Organization

### src/

- **mori.h** — types (`mori_shm` with creator `pid` fork guard,
  `mori_buf`, `mori_vec` with `index` field, `mori_str` with `str_bytes`
  bound, `mori_list_view`), declarations, `MORI_ALIGN64`,
  `mori_sizeof_elt`, and the identifier grammar constants
  (`MORI_NAME_MAX`, `MORI_MAX_PATH`, `MORI_IDENTIFIER_MAX`,
  `MORI_FORMAT_BUFLEN`, `MORI_PREFIX_LITERAL`).
- **shm.c** — platform SHM create/open/close + finalizers for both
  sides. `mori_shm_reap` enumerates the platform’s name source
  (`#ifdef`-selected) — `/dev/shm` on Linux (entries are region names),
  the per-user registry dir on macOS (per-process logs; see Platform
  Notes) — classifies by the PID embedded in the name via
  `mori_pid_alive` (Linux unlinks each dead-PID region directly; macOS
  reads each dead-PID log via `mori_reap_log` and unlinks the regions it
  names), and feeds the shared per-name core `mori_reap_unlink`
  (shm_unlink + record-on-reclaim). Windows / other POSIX cannot reap →
  `NULL` / `*n == 0`. `mori_shm_os_unlink` is the single region-unlink
  seam (host finalizer, create-fail, reap) and is pure `shm_unlink`; the
  macOS registry is decoupled from it and managed by `mori_log_append` /
  `mori_log_release` (see Platform Notes). Reap semantics A, both
  platforms: a dead-PID region is reported removed only when
  `shm_unlink` actually reclaimed it, so concurrent reaps never
  double-report. Error path: `mori_err_classify` (static; native errno /
  GetLastError → portable `MORI_E*` category) and `mori_err_describe`
  (category summary + platform remediation hint); `mori_shm_create` /
  `mori_shm_create_heap` return `MORI_OK` or a failure category,
  classifying the native code **before** any `close` / `unlink` /
  `CloseHandle` clobbers `errno` / `GetLastError`.
- **serialize.c** — `mori_serialize_count`, `mori_serialize_into`,
  `mori_unserialize_from`. Used for fallback (non-zero-copy) ALTLIST
  entries where directory `sexptype == 0`.
- **altrep.c** — all ALTREP class definitions, `mori_create` dispatcher,
  recursive writers (`mori_nested_size` / `mori_nested_write`), consumer
  open+wrap (`mori_shm_open_and_wrap`), path open (`mori_open_path_c`),
  view constructors (`mori_make_view_extptr` for bare keeper-chain
  extptrs, `mori_make_list_view` for full ALTLIST + attrs),
  identity/name (`mori_is_shared` / `mori_shm_name`), identifier
  formatter / parser (`mori_format_chain` / `mori_parse_identifier`),
  serialization hooks, `mori_altrep_init`. The create-failure message
  lives in `mori_shm_create_failed`, which composes the requested size
  (`mori_format_bytes`) with `mori_err_describe`’s summary + hint into a
  single `Rf_error`.
- **init.c** — `R_init_mori`, `.Call` registration table (5 entries:
  `mori_create`, `mori_shm_open_and_wrap`, `mori_is_shared`,
  `mori_shm_name`, `mori_prune`).

`.Call` names match C function names; all entry points take a single
`SEXP` except `mori_prune`, which takes none.

### R/

`mori-package.R` (package docs) and `share.R` (five `.Call` wrappers —
dispatch and error handling are at the C level;
[`prune_shared()`](https://shikokuchuo.net/mori/dev/reference/prune_shared.md)
delegates to `mori_prune`, which returns `NULL` when nothing was
pruned). `import-standalone-defer.R` is a vendored copy of withr’s
`defer()` (`usethis::use_standalone("r-lib/withr", "defer")`) used only
by tests — do not edit by hand; refresh via `use_standalone`.

## Testing

testthat edition 3. Entry point `tests/testthat.R`; tests in
`tests/testthat/test-*.R` grouped by topic (each file’s `desc()` strings
are authoritative). All tests run unconditionally — nothing gated behind
`skip_on_cran` or environment variables.

## Platform Notes

- **Linux**: SHM in `/dev/shm/` (tmpfs). Accessed via
  [`open()`](https://rdrr.io/r/base/connections.html) /
  [`unlink()`](https://rdrr.io/r/base/unlink.html) directly to avoid the
  `-lrt` link dependency that `shm_open` requires. `MAP_POPULATE`
  pre-faults pages.
- **macOS**: Kernel-backed SHM via `shm_open` / `shm_unlink` (in libc,
  no extra link flags). `MAP_POPULATE` is a no-op (defined to 0). The
  kernel POSIX-shm namespace has no filesystem footprint and cannot be
  enumerated (invisible to `ipcs` / `lsof` once the creator dies), so
  orphan reaping relies on a per-user registry under `TMPDIR` (resolved
  via `$TMPDIR`, then `confstr(_CS_DARWIN_USER_TEMP_DIR)`, then `/tmp`)
  that `mori_shm_reap` scans in place of `/dev/shm`. The registry must
  live in an enumerable namespace — putting it in SHM would face the
  same non-enumerability wall — so it is filesystem-backed, but to keep
  the
  per-[`share()`](https://shikokuchuo.net/mori/dev/reference/share.md)
  cost off the critical path it is **one append-only log per process**
  (`<dir>/mori_<pid>`) rather than a file per region: `mori_log_append`
  (called in `mori_shm_create` before the region is observable) opens
  the log lazily on first share and appends one line per region name — a
  single [`write()`](https://rdrr.io/r/base/write.html) (~1 µs) vs
  creating a file (~40 µs on APFS, where neither path resolution nor a
  cached dir fd helps because the cost is inode allocation +
  journaling). Single-writer per process ⇒ no locking; the only reader
  is a reaper of a *dead* PID, which by definition has no concurrent
  writer. `mori_log_release` (host finalizer / create-fail) decrements a
  process-global live-region count and, at zero, closes+unlinks the log
  and `rmdir`s the now-empty dir — so the dir tracks the live-region set
  as the per-region markers did, but driven by finalization. The count
  is incremented unconditionally so it stays balanced when logging
  itself fails (which forfeits only reapability, never the region).
  Fork-safe: `mori_log_fork_guard` reopens the log when `getpid()`
  changes (the inherited fd is the parent’s — closed, never unlinked).
  The region unlink in `mori_host_finalizer` is fork-guarded the same
  way (creator PID stored in `mori_shm`; see Concurrency Model). Crash
  semantics match the regions’: a
  [`write()`](https://rdrr.io/r/base/write.html) is visible
  cross-process via the page cache without `fsync` and survives the
  writer’s death, lost only on reboot. Path resolution (`mori_log_dir`)
  is pure — builds/caches the path but never creates the dir
  (`mori_log_append` retries through `mkdir` on `ENOENT` and is the sole
  creator), so a reap on a process with no log opens nothing and leaves
  nothing behind. All log ops best-effort.
- **Windows**: Page-file-backed via `CreateFileMappingA` /
  `MapViewOfFile`. `kernel32` is always available. Host must keep the
  mapping handle alive until consumers have opened it (the GC-chained
  host extptr handles this).

## Package Conventions

- roxygen2 with markdown support; NAMESPACE is auto-generated.
- MIT license.
- Version is `major.minor.patch.dev` (current dev tag `.9000`).
- `README.md` is generated from `README.Rmd` — edit the `.Rmd` and
  re-knit, never edit `README.md` directly.
- `AGENTS.md`, `.claude/`, and `.posit/` are in `.Rbuildignore` and
  don’t ship to CRAN.
