# Test if an Object is Shared

Returns `TRUE` if `x` is an ALTREP object backed by shared memory
(created by [`share()`](https://mori.r-lib.org/dev/reference/share.md)
or
[`map_shared()`](https://mori.r-lib.org/dev/reference/map_shared.md)),
`FALSE` otherwise.

## Usage

``` r
is_shared(x)
```

## Arguments

- x:

  an R object.

## Value

`TRUE` or `FALSE`.

## Examples

``` r
x <- 1:100
y <- share(x)
is_shared(y)
#> [1] TRUE
is_shared(x)
#> [1] FALSE
```
