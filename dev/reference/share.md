# Create a Shared Object

Write an R object into shared memory and return a version that other
processes on the same machine can map without copying.

## Usage

``` r
share(x)
```

## Arguments

- x:

  an R object.

## Value

For atomic vectors (including character vectors and those with
attributes such as names, dim, class, or levels) and lists or data
frames, an ALTREP-backed object that reads directly from shared memory.
For any other object (environments, closures, language objects, `NULL`),
the input is returned unchanged with no shared memory region created.

## Details

Attributes are stored alongside the data in the shared memory region and
restored on the consumer side. Character vectors use a packed layout and
elements are materialised lazily on access. When serialised (e.g. by
[`serialize()`](https://rdrr.io/r/base/serialize.html) or across a
[`mirai()`](https://mirai.r-lib.org/reference/mirai.html) call), a
shared object is represented compactly by its shared memory name (~30
bytes) rather than by its contents.

An S4 object whose data part is an atomic vector, a character vector, or
a list stays an S4 object when it is shared. The class and the slots are
preserved, and S4 method dispatch works on the shared object. The data
part is shared without a copy. The slots are serialised and restored on
the consumer side as copies.

A shared list can hold elements of any type. An element that is not an
atomic vector, a character vector, or a list is serialised and restored
as a copy on access. This applies to environments, closures, and
language objects.

The shared memory region is managed automatically. It stays alive as
long as the returned object (or any element extracted from it) is
referenced in R, and is freed automatically when no references remain or
the session exits cleanly.

`share()` is idempotent: calling it on an object that is already backed
by shared memory returns the input unchanged without allocating a new
region.

**Important**: ensure the return value of `share()` is not garbage
collected before a consumer can map its shared memory.

## Persistence

Direct [`saveRDS()`](https://rdrr.io/r/base/readRDS.html) of a shared
object writes only the shared memory name, so the resulting file is
meaningful only on the same machine while the region is still alive. For
portable storage or transport across machines, materialise into a
regular in-memory copy first with
[`rlang::duplicate()`](https://rlang.r-lib.org/reference/duplicate.html),
which deep-duplicates the object:

    saveRDS(rlang::duplicate(x), file = "x.rds")

## See also

[`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
to open a shared region by name,
[`shared_name()`](https://shikokuchuo.net/mori/dev/reference/shared_name.md)
to extract the shared memory name.

## Examples

``` r
x <- share(1:100)
sum(x)
#> [1] 5050

lst <- share(list(a = 1:3, b = letters))
is_shared(lst)
#> [1] TRUE
```
