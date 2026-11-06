# mori: Shared Memory for R Objects

Share R objects via shared memory with
[`share()`](https://mori.r-lib.org/dev/reference/share.md), access them
in other processes with
[`map_shared()`](https://mori.r-lib.org/dev/reference/map_shared.md),
using R's ALTREP framework for zero-copy memory-mapped access. Shared
objects serialize compactly via ALTREP serialization hooks. Shared
memory is automatically freed when the R object is garbage collected.

## mori and mizu

The mizu package provides a complete async and parallel framework over
the same shared-memory core — task pools, a parallel map, channels, and
Python interop. Objects created by
[`share()`](https://mori.r-lib.org/dev/reference/share.md) travel
through mizu tasks as zero-copy references. mori remains the right
choice when a parallel backend such as mirai or parallel is already in
place, or when data must be mapped by name between independent processes
or outlive any pool.

## See also

Useful links:

- <https://mori.r-lib.org>

- <https://github.com/r-lib/mori>

- Report bugs at <https://github.com/r-lib/mori/issues>

## Author

**Maintainer**: Charlie Gao <charlie.gao@posit.co>
([ORCID](https://orcid.org/0000-0002-0750-061X))

Authors:

- Charlie Gao <charlie.gao@posit.co>
  ([ORCID](https://orcid.org/0000-0002-0750-061X))

Other contributors:

- Posit Software, PBC ([ROR](https://ror.org/03wc8by49)) \[copyright
  holder, funder\]
