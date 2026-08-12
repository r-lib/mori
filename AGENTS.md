# AGENTS.md

Guidance for AI coding agents working **on** the mori package. mori — Shared Memory for R Objects: POSIX shared memory (Linux, macOS) / Win32 file mappings (Windows) + R's ALTREP framework let processes on one machine read the same physical pages. No external dependencies; requires R >= 4.3.0 (ALTLIST). R API (all thin `.Call` wrappers; logic is C-level): `share()`, `map_shared()`, `shared_name()`, `is_shared()`, `prune_shared()`. ALTREP serialization hooks emit the `shared_name()` identifier as the wire form — transparent under `serialize()` and `mirai`.

Claude Code users: add `.claude/CLAUDE.md` containing `@../AGENTS.md` (`.claude/` is gitignored).

## Commands

```r
devtools::test()                                    # full suite (testthat ed 3)
testthat::test_file("tests/testthat/test-nested.R") # single test file
devtools::document()                                # roxygen2 -> man/, NAMESPACE
rmarkdown::render("README.Rmd")                     # rebuild README.md
```

```bash
R CMD build .
R CMD check --no-manual --compact-vignettes=gs+qpdf mori_*.tar.gz   # matches CI
```

- `Config/build/compilation-database: true` in DESCRIPTION → `compile_commands.json` on install (gitignored); clangd works out of the box.
- R's in-source build ignores header dependencies: after editing `mori.h`, `rm src/*.o src/*.so` before reinstalling — stale objects linked against old struct layouts cause sizeof mismatches and garbage fields.

## Storage Model

- **Zero-copy (SHM-backed)**: atomic vectors (any type/attributes) and data frame columns are written into SHM and ALTREP-backed on consumers; attributes serialize into a trailing region, reapplied on open. Numeric `Dataptr_or_null` returns the SHM pointer; `Dataptr(writable=TRUE)` triggers COW into a private copy. String `Elt` creates CHARSXPs lazily; `Dataptr_or_null` returns NULL to force per-element access.
- **Nested lists (zero-copy)**: VECSXP/LISTSXP elements are written inline as a complete child MORL region at their `data_offset`, each level wrapped in its own ALTLIST. Sub-lists are `is_shared()`-TRUE with path-bearing `shared_name()`s; `map_shared(shared_name(sub))` opens them directly. The OS region name is the prefix before `[` (`sub("\\[.*$", "", shared_name(x))`).
- **Pass-through**: everything else (environments, closures, language, NULL) is returned unchanged; no SHM created.

## Concurrency Model

**Write-once on host, read-many on consumers, COW for any mutation.**

- Each `share()` allocates a fresh region; existing regions are never mutated. No locking anywhere — any change admitting in-place mutation breaks the model.
- Names embed creator PID + randomly-seeded per-process counter (`mori_<pid>_<counter>`, `mori_region_name`). `mori_shm_create` uses `O_EXCL` / `ERROR_ALREADY_EXISTS`; a collision (orphan of a crashed same-PID process) is an error (`MORI_EEXIST`), never worked around by reuse.
- `prune_shared()` clears such orphans; it must run while the PID is free — a live process cannot reap its own names.
- The host writes the full region before its name is observable — partial writes are never seen. Consumer mappings are read-only (`PROT_READ` / `FILE_MAP_READ`); mutation triggers COW.
- Fork safety: `mori_shm` records the creator PID; `mori_host_finalizer` unlinks only on `getpid()` match, so a forked child's inherited finalizers skip the unlink (its `munmap` is process-local) — `parallel::mclapply` forks cannot destroy parent regions.

## share() Dispatch (`altrep.c: mori_create`)

0. Already mori-backed (`mori_view_check`) → return `x` unchanged. Idempotence required: re-sharing a sub-list view must not allocate a fresh root region (would break `shared_name()`).
1. Otherwise sizes via `mori_layout_size_impl` (`ok = NULL` here — no vetoes; non-mori ALTREPs materialize through `DATAPTR_RO` at write) and writes via `mori_layout_write`; 0 size → `x` returned as-is. Type ladder: `VECSXP`/`LISTSXP` → MORL (pairlists coerced to VECSXP at every level; data frames included), `STRSXP` → MORS, atomics (`REALSXP`/`INTSXP`/`LGLSXP`/`RAWSXP`/`CPLXSXP`) → MORH, everything else (incl. `NILSXP`) → pass-through.

The result returns via `mori_make_result`, which chains the host extptr into the ALTREP's keeper chain (see Internal State and Lifetime).

### Two-pass write invariant

All writers are count-then-write pairs: `mori_serialize_count`/`mori_serialize_into` (serialize.c, fallback bytes), `mori_nested_size`/`mori_nested_write` (MORL regions), `morh_size`/`morh_write`, `mors_size`/`mors_write`. **If a pair disagrees by even one byte, the region is malformed and consumers read garbage.** Mirror every change in both sides.

## Embedder C API

Declared in `mori.h`, C-level only (not `.Call`) — for packages embedding mori layouts under their own SHM management:

- **Layout oracle + writer**: `mori_layout_size(x)` → region size, or 0 for what the writer must not take (non-mori ALTREP nodes — would materialize via `DATAPTR_RO`; S4 bits — don't survive the layouts). Vetoes ride the size recursion (`mori_layout_size_impl` with `ok != NULL`); the host path passes `NULL` and vetoes nothing. `mori_layout_write(base, x)` emits exactly `mori_layout_size` bytes and zeroes reserved header bytes [24-63] on every write (embedders may recycle regions).
- **Wrap constructors**: `mori_vec_wrap` / `mori_str_wrap` / `mori_list_wrap` build views over embedder memory, pinning `keeper` via the data1 extptr's protected slot; each takes a `release` once-hook (see Internal State). `mori_restore_attrs` reapplies trailing serialized attributes.
- **Introspection + path walk**: `mori_view_check`, `mori_shm_name`, `mori_parse_id`, `mori_walk_path` (walks an index path over an open region; the caller's keeper flows into the result's chain).
- **Wire hooks**: `mori_set_wire_hooks(emit, resolve)` — see Serialization Hooks.

## ALTREP Classes

Registered in `mori_altrep_init`. Numerics (`mori_real`/`integer`/`logical`/`raw`/`complex`) share the `mori_vec` pattern (SHM-backed data pointer). `mori_string` (`mori_str`) does lazy per-element `Elt` via `Rf_mkCharLenCE`, bounds-checking each offset-table entry against `str_bytes` and range-checking the encoding. `mori_list` (`mori_list_view`) has a per-element directory and a lazy `Elt` cache using `R_NilValue` as the uncached sentinel (a fresh VECSXP is NIL-filled — zero init work). NIL elements are the sole singleton output of `mori_unwrap_element`'s fallback path, so they're never cached and identity is preserved.

### COW invariant for new ALTREP methods

Any method returning a writable pointer (or mutating) **must check `R_altrep_data2(x)` first** — if non-null, the vector is materialized and the SHM pointer is no longer authoritative. New `Dataptr` / coercion methods routinely forget this. COW materialization also fires the view's release hook early (see Internal State).

## ALTREP Serialization Hooks

All classes register `Serialized_state` + `Unserialize`.

- **Wire form (single canonical shape)**: `mori_format_chain` is the single source of truth for `mori_shm_name` (the `.Call`) and all `Serialized_state` methods — walks the keeper chain collecting `view->index` (`>= 0` only), formats `<prefix>` (root) or `<prefix>[i1+1,...]` (sub-object). 1-based externalisation cues R's `[[i]]`; the parser converts back.
- **Unserialize**: `mori_Unserialize` (vec/list) and `mori_string_Unserialize`. Vec/list: a STRSXP state is always an identifier (their fallbacks are never STRSXP) — probed via `mori_shm_open_and_wrap`, a miss errors (corrupt stream); other states are materialized data. Strings: both forms are string-like, so the fallback is wrapped in a length-1 VECSXP (`mori_wrap_string_state`) — bare STRSXP = identifier, wrapper = data (unwrapped without probing). Path-form identifiers route through `mori_open_path_c` → `mori_walk_path`: intermediates step via `mori_make_view_extptr` (bare keeper-chain extptrs, no ALTLIST/attrs — never observed), leaf via `mori_unwrap_element`.
- **Wire hooks** (`mori_set_wire_hooks`, set once at embedder load): `emit(view)` fires from `Serialized_state` when an identifier (not a materialization) is emitted; `resolve(view, shm)` fires after the wrap on the resolve paths (`mori_shm_open_and_wrap`, `mori_open_path_c` — not `mori_walk_path`, whose direct callers fire their own). Supports a cross-process lifetime protocol: flag regions on escape, count remote references on arrival.

Fallback to full materialization when: data2 is set (COW'd), or nesting exceeds `MORI_MAX_PATH` (64).

### Identifier grammar

```
identifier ::= prefix [ "[" int1 ("," int1)* "]" ]
prefix     ::= MORI_PREFIX_LITERAL hex+ "_" hex+
hex+       ::= [0-9a-f]+         # lowercase
int1       ::= [1-9][0-9]*       # 1-based, no leading zeros
```

`MORI_PREFIX_LITERAL` (mori.h: `/mori_` POSIX, `Local\\mori_` Windows) is the **single source of truth shared by shm.c's name-format strings and the parser's `memcmp`** — change once, both sides stay in sync. Variable-width hex; `MORI_NAME_MAX = 30` bounds the prefix on both platforms. `mori_parse_id` is bounded and single-pass (`MORI_IDENTIFIER_MAX`); indices stored 0-based; the path loop uses NUL as sentinel (every fail-branch excludes `\0`). Sizing invariant: `MORI_NAME_MAX + 2 + 11 × MORI_MAX_PATH < MORI_FORMAT_BUFLEN`.

### Validation contract

`map_shared` and unserialize share `mori_parse_id` for shape validation but differ in failure mode: **malformed → `NULL`** (wrong type/length, `NA`, bad prefix/path); **well-formed but unmappable → error** (missing region, bad magic, truncated header, OOB index, non-VECSXP intermediate, fields inconsistent with mapped size). Consumer validation is total — corrupt input errors, never reads out of bounds: root headers vs mapped size (`mori_open_vector`/`mori_open_string`), directory entries vs data region (`mori_unwrap_element`), string tables vs region (`mori_str_wrap`), offset entries at `Elt`. Preserve this split in `mori_shm_open_and_wrap` — collapsing it either way breaks the probe-vs-corruption distinction.

## SHM Region Layouts

Magic in the first 4 bytes (`MORI_MAGIC_*`): MORH `0x4D4F5248` vector, MORL `0x4D4F524C` list, MORS `0x4D4F5253` string. Every layout opens with a 64-byte header; bytes [24-63] are reserved (zeroed on every write — embedders may recycle regions) for embedder cross-process state. Tables are the canonical spec; `mori_nested_write` / `morh_write` / `mors_write` (with `mori_serialize_into` for fallbacks and attrs) are the implementations. Attributes are serialized R objects (pairlist on R < 4.6, named list otherwise); `mori_restore_attrs` reapplies them on the consumer.

**MORH — atomic vector.** Data at byte 64 (64-byte aligned for SIMD); trailing attrs after the data.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic |
| 4 | 4 | sexptype |
| 8 | 8 | length (int64) |
| 16 | 8 | attrs_size (int64, 0 if none) |
| 24 | 40 | reserved (zero) |
| 64+ | | raw vector data |
| 64 + length×elt_size | | serialized attributes (if `attrs_size` > 0) |

**MORL — ALTLIST.** Header + element directory + per-element data regions.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic |
| 4 | 4 | n_elements (int32) |
| 8 | 8 | attrs_offset (int64) |
| 16 | 8 | attrs_size (int64) |
| 24 | 40 | reserved (zero) |
| 64 | 32×n | element directory |
| varies | | element data (64-byte aligned), then serialized attributes |

Directory entry (32 bytes): `data_offset(8) + data_size(8) + sexptype(4) + attrs_size(4) + length(8)`. `sexptype`: `0` → serialized bytes (serialize.c); `STRSXP` → offset table + packed strings at `data_offset`; `VECSXP` → nested MORL region inlined at `data_offset` of size `data_size` (child header/directory/elements/attrs all inline; parent's `attrs_size` always 0 for VECSXP children); other → raw zero-copy data. Non-VECSXP attrs sit at `data_offset + data_size - attrs_size`.

**MORS — ALTSTRING.** Header + offset table + packed string bytes + optional trailing attrs.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic |
| 4 | 4 | attrs_size (int32, 0 if none) |
| 8 | 8 | n_strings (int64) |
| 16 | 8 | str_data_size (int64: offset-table start → end of packed strings, incl. padding) |
| 24 | 40 | reserved (zero) |
| 64 | 16×n | offset table |
| 64 + align64(16×n) | | packed string bytes |
| 64 + str_data_size | | serialized attributes (if `attrs_size` > 0) |

Offset-table entry (16 bytes): `str_offset(int64, relative to packed area) + str_length(int32, < 0 = NA) + str_encoding(int32, cetype_t: 0=native, 1=UTF-8, 2=Latin-1, 3=bytes)`. The same layout is inlined for STRSXP elements in MORL regions (table at the element's `data_offset`; element attrs use the directory entry's `attrs_size`).

## Internal State and Lifetime

SHM lifetime is automatic via chained extptr finalizers. Three interned tags (C globals in altrep.c):

- **`mori_shm_tag`** — SHM mapping extptr (both sides). Addr `mori_shm *`; finalizer `munmap`.
- **`mori_host_tag`** — host-only unlink extptr from `mori_make_result`; addr `mori_shm *` (name on POSIX, HANDLE on Windows); finalizer `shm_unlink`/`CloseHandle`, skipped when creator `pid` ≠ `getpid()` (fork guard). Chained above the shm extptr.
- **`mori_owned_tag`** — every malloc-backed extptr: ALTREP data1 (view/vec/str) and bare path-walk intermediates (`mori_make_view_extptr`, always `mori_list_view *`). Addr type at the ALTREP boundary from `TYPEOF(x)`: `VECSXP → mori_list_view *`, `STRSXP → mori_str *`, else `mori_vec *`. `index` field: -1 standalone/root, >= 0 element. Finalizer `mori_owned_finalizer`: release hook once, then `free`.

**Release hook**: every owned struct embeds `mori_owned` (`release` + `release_arg`) as its first member, so the finalizer recovers it from any addr. Fires exactly once (`mori_release_once`'s NULL store) — at COW materialization or the finalizer, whichever first; internal callers pass NULL. ALTLIST views fire at the finalizer only: extracted elements keep referencing the region.

`is_shared()` = `mori_view_check` (ALTREP with `mori_owned_tag` data1). `mori_shm_name()` = `mori_format_chain` (bare prefix for roots, path form for sub-objects).

### Keeper chain

- **Host**: `mori_shm_create` mmaps (POSIX fd closed after mmap); `mori_make_result` splits ownership — ALTREP gets the mapping (`mori_shm_tag` extptr), the host unlink extptr is chained as its `prot`. Both finalizers run at GC; `R_RegisterCFinalizerEx(..., TRUE)` covers session exit.
- **Consumer**: `mori_shm_open` maps read-only (never unlinks). Prefix-opened lists: shm extptr → root view extptr (index -1) → data1; `Elt` sub-lists chain through parent views. Path-form: intermediates are bare extptrs, only the leaf gets an ALTLIST. Element vec/str extptrs' `prot` is the parent view (shm extptr at root).
- **Lifetime**: leaves pin the root SHM through the chain (leaf → views → shm → host); every `R_MakeExternalPtr` retains its `prot`, so the chain survives GC of enclosing ALTLISTs. `mori_format_chain` is the only chain walker (`mori_owned_tag` hops, always `mori_list_view *`).

## Code Organization

- **src/mori.h** — types (`mori_shm` incl. creator `pid`, `mori_buf`, `mori_owned`, `mori_vec`, `mori_list_view`), embedder API declarations, constants (`MORI_MAGIC_*`, `MORI_HEADER_SIZE`, `MORI_TAG_*`, grammar bounds, `MORI_PREFIX_LITERAL`), `MORI_ALIGN64`, `mori_sizeof_elt`.
- **src/shm.c** — platform SHM create/open/close + finalizers. `mori_shm_reap` enumerates the platform name source (`/dev/shm` on Linux; per-user registry dir on macOS — see Platform Notes), classifies by embedded PID (`mori_pid_alive`), feeds `mori_reap_unlink`; a dead-PID region is reported removed only when `shm_unlink` actually reclaimed it (concurrent reaps never double-report). Windows / other POSIX can't reap. `mori_shm_os_unlink` is the single unlink seam. Errors: `mori_err_classify` maps native errno/GetLastError → `MORI_E*` **before** any close/unlink clobbers it; `mori_err_describe` → summary + remediation hint.
- **src/serialize.c** — `mori_serialize_count` / `mori_serialize_into` / `mori_unserialize_from`: fallback MORL entries (`sexptype == 0`) and attributes.
- **src/altrep.c** — everything else: ALTREP classes, `mori_create`, layout dispatchers + size/write pairs, wrap constructors, consumer open+wrap, path walk, identifier formatter/parser, serialization + wire hooks, `mori_altrep_init`. Create failures: `mori_shm_create_failed` composes size (`mori_format_bytes`) + `mori_err_describe` into one `Rf_error`.
- **src/init.c** — `R_init_mori`; 5 `.Call` entries (names match C functions; all take one `SEXP` except `mori_prune`).
- **R/** — `mori-package.R` (docs), `share.R` (the five wrappers). `import-standalone-defer.R` is vendored from withr (`usethis::use_standalone("r-lib/withr", "defer")`) — don't edit by hand.

## Testing

testthat edition 3; `tests/testthat.R` entry point; `tests/testthat/test-*.R` grouped by topic. Nothing gated behind `skip_on_cran` or env vars.

## Platform Notes

- **Linux**: `/dev/shm` tmpfs via `open()`/`unlink()` directly (avoids the `-lrt` dependency). `MAP_POPULATE` pre-faults.
- **macOS**: `shm_open`/`shm_unlink` (libc); `MAP_POPULATE` is a no-op. The kernel namespace can't be enumerated (invisible once the creator dies), so reaping uses a per-user registry under `TMPDIR`: **one append-only log per process** (`<dir>/mori_<pid>`, not a file per region — one `write()` ~1 µs vs ~40 µs file creation), opened lazily on first share by `mori_log_append` (sole creator; retries through `mkdir` on `ENOENT`). Single-writer ⇒ no locking; readers are reapers of dead PIDs. `mori_log_release` tracks a live-region count; at zero it unlinks the log and `rmdir`s the dir. The count is incremented even when logging fails (forfeits reapability, never the region). Fork-safe via `mori_log_fork_guard` (reopens on PID change). A `write()` is cross-process visible without `fsync`, lost only on reboot. All log ops best-effort.
- **Windows**: page-file-backed `CreateFileMappingA`/`MapViewOfFile`. The host must keep the mapping handle alive until consumers open it (the GC-chained host extptr does).

## Package Conventions

- roxygen2 (markdown); NAMESPACE auto-generated. MIT license. Version `major.minor.patch.dev` (dev tag `.9000`).
- `README.md` is generated from `README.Rmd` — edit the `.Rmd` and re-knit, never `README.md`.
- `AGENTS.md`, `.claude/`, `.posit/` are in `.Rbuildignore`.
