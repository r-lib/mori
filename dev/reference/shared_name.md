# Extract Shared Memory Name

Extract the shared memory name from a shared object. This name can be
passed to
[`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
to open the same region in another process.

## Usage

``` r
shared_name(x)
```

## Arguments

- x:

  a shared object as returned by
  [`share()`](https://shikokuchuo.net/mori/dev/reference/share.md) or
  [`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md).

## Value

A character string identifying the shared object, or `NULL` if `x` is
not a shared object. For a sub-list or element extracted from a shared
list, the string carries a bracketed 1-based index path (e.g.
`"/mori_abc_1[2,3]"`).
[`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
accepts both forms; the path-qualified form returns the addressed
sub-object directly. The underlying shared memory region name is the
prefix before `[` and is recoverable via
`sub("\\[.*$", "", shared_name(x))`.

## See also

[`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md)
to open a shared region by name.

## Examples

``` r
x <- share(1:100)
shared_name(x)
#> [1] "/mori_1a43_92d686be"

# A sub-object extracted from a shared list carries a bracketed index path:
lst <- share(list(a = 1:3, b = letters))
shared_name(lst[[2]])
#> [1] "/mori_1a43_92d686bf[2]"
```
