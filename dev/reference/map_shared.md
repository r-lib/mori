# Open Shared Memory by Name

Open a shared memory region identified by a name string and return an
ALTREP-backed R object that reads directly from shared memory.

## Usage

``` r
map_shared(name)
```

## Arguments

- name:

  a character string as returned by
  [`shared_name()`](https://mori.r-lib.org/dev/reference/shared_name.md):
  either a bare shared memory name (opens the root) or a name with a
  1-based bracketed index path (e.g. `"/mori_abc_1[2,3]"`, opens the
  addressed sub-list or element directly).

## Value

The R object stored at the named region (or sub-object at the given
path), or `NULL` if `name` is not a valid shared memory name (wrong
type, length, `NA`, missing or malformed prefix, or malformed bracketed
path). If `name` parses as valid but the region is absent or corrupted —
or the path doesn't address a valid sub-object — an error is raised.

## See also

[`share()`](https://mori.r-lib.org/dev/reference/share.md) to create a
shared object,
[`shared_name()`](https://mori.r-lib.org/dev/reference/shared_name.md)
to extract the shared memory name.

## Examples

``` r
x <- share(1:100)
nm <- shared_name(x)
map_shared(nm)
#>   [1]   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18
#>  [19]  19  20  21  22  23  24  25  26  27  28  29  30  31  32  33  34  35  36
#>  [37]  37  38  39  40  41  42  43  44  45  46  47  48  49  50  51  52  53  54
#>  [55]  55  56  57  58  59  60  61  62  63  64  65  66  67  68  69  70  71  72
#>  [73]  73  74  75  76  77  78  79  80  81  82  83  84  85  86  87  88  89  90
#>  [91]  91  92  93  94  95  96  97  98  99 100

# A bracketed index path opens the addressed sub-object directly:
lst <- share(list(a = 1:3, b = letters))
map_shared(shared_name(lst[[2]]))
#>  [1] "a" "b" "c" "d" "e" "f" "g" "h" "i" "j" "k" "l" "m" "n" "o" "p" "q" "r" "s"
#> [20] "t" "u" "v" "w" "x" "y" "z"
```
