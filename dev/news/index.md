# Changelog

## mori (development version)

- Region layouts now open with a 64-byte header.
- [`share()`](https://shikokuchuo.net/mori/dev/reference/share.md) now
  supports S4 objects built on vectors or lists (previously the S4
  object bit was silently dropped).
- [`share()`](https://shikokuchuo.net/mori/dev/reference/share.md) of
  attribute-heavy objects and nested lists is faster: the write pass no
  longer re-serializes each element just to measure its size.
- Fixed type confusion on unserialize when a shared string’s content
  resembled a shared memory identifier (e.g. a stored
  [`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
  value).
- Fixed a forked child process
  (e.g. [`parallel::mclapply`](https://rdrr.io/r/parallel/mclapply.html))
  unlinking the parent’s live region when garbage-collecting an
  inherited shared object.
- Corrupted regions now raise a clean R error on
  [`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
  or access instead of reading out of bounds or crashing R.

## mori 0.2.2

CRAN release: 2026-07-21

- Region name counters now start at a per-process random value, so a
  process reusing a crashed process’s PID no longer collides with its
  orphaned regions
  ([\#50](https://github.com/shikokuchuo/mori/issues/50)).

## mori 0.2.1

CRAN release: 2026-06-09

- New
  [`prune_shared()`](https://shikokuchuo.net/mori/dev/reference/prune_shared.md)
  removes shared memory regions orphaned by a process that was killed
  before it could clean up
  ([\#25](https://github.com/shikokuchuo/mori/issues/25)).
- When [`share()`](https://shikokuchuo.net/mori/dev/reference/share.md)
  fails to create a shared memory region it now reports the requested
  size and, where applicable, an actionable hint (e.g. raising a
  container’s `/dev/shm` limit) instead of a generic message
  ([\#29](https://github.com/shikokuchuo/mori/issues/29)).

## mori 0.2.0

CRAN release: 2026-05-09

- Wire-format change (breaking): The serialization format and the shared
  memory region naming scheme have both changed.
- [`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
  and
  [`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
  now round-trip for sub-lists and extracted elements, not just whole
  shared objects. Sub-object identifiers carry a bracketed index path
  (e.g. `/mori_abc_1[2,3]`).
- [`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
  now returns `NULL` for non-shared inputs (previously the empty string
  `""`); `shared_name(x) %||% ""` retains the previous behaviour.
- [`share()`](https://shikokuchuo.net/mori/dev/reference/share.md) on
  Linux fails with a clean R error when `/dev/shm` is too small (typical
  in containers, where the limit can be raised at start) instead of
  SIGBUS-ing mid-serialize
  ([\#14](https://github.com/shikokuchuo/mori/issues/14)).
- [`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
  on Linux no longer prefaults the whole region, restoring lazy
  first-touch access to match macOS.

## mori 0.1.0

CRAN release: 2026-04-21

- Initial CRAN release.
