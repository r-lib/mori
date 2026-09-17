# Prune Orphaned Shared Memory Regions

Recover shared memory regions leaked by a process that was killed before
it could clean up — for example after a crash, a `SIGKILL`, or the
out-of-memory killer. **You do not normally need this:** mori frees
shared memory automatically when the
[`share()`](https://mori.r-lib.org/dev/reference/share.md) object is
garbage-collected and on a clean R session exit (see Details).

This function is only relevant on Linux and macOS. On Windows, shared
memory cannot be orphaned, so there is never anything to clean up and
calling it has no effect.

## Usage

``` r
prune_shared()
```

## Value

Invisibly, a character vector of the region names that were removed, or
`NULL` if nothing was removed.

## Details

Shared memory is normally managed automatically: a region is unlinked
when the [`share()`](https://mori.r-lib.org/dev/reference/share.md)
object that owns it is garbage-collected, and on a clean R session exit.
A region is only left behind if the owning process is killed before
either can run. `prune_shared()` removes such leftovers.

Pruning is **conservative**: a region is removed only if its creating
process (encoded in the region name) is no longer running, so regions
still in use by a live process are never touched. Removing a region
unlinks only its name; processes that have already mapped it keep
reading until they release it, and the memory is freed once the last
mapping is gone.

## Examples

``` r
# Shared memory is freed automatically - you do not normally call this.
# To prune regions left behind by a crashed process:
prune_shared()
```
