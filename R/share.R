#' Create a Shared Object
#'
#' Write an R object into shared memory and return a version that other
#' processes on the same machine can map without copying.
#'
#' @param x an R object.
#'
#' @return For atomic vectors (including character vectors and those with
#'   attributes such as names, dim, class, or levels) and lists or data
#'   frames, an ALTREP-backed object that
#'   reads directly from shared memory. For any other object (environments,
#'   closures, language objects, `NULL`), the input is returned unchanged
#'   with no shared memory region created.
#'
#' @details
#' Attributes are stored alongside the data in the shared memory region and
#' restored on the consumer side. Character vectors use a packed layout and
#' elements are materialised lazily on access. When serialised (e.g. by
#' [serialize()] or across a `mirai()` call), a shared object is represented
#' compactly by its shared memory name (~30 bytes) rather than by its
#' contents.
#'
#' An S4 object whose data part is an atomic vector, a character vector,
#' or a list stays an S4 object when it is shared. The class and the slots
#' are preserved, and S4 method dispatch works on the shared object. The
#' data part is shared without a copy. The slots are serialised and
#' restored on the consumer side as copies.
#'
#' A shared list can hold elements of any type. An element that is not an
#' atomic vector, a character vector, or a list is serialised and restored
#' as a copy on access. This applies to environments, closures, and
#' language objects.
#'
#' The shared memory region is managed automatically. It stays alive as long
#' as the returned object (or any element extracted from it) is referenced
#' in R, and is freed automatically when no references remain or the session
#' exits cleanly.
#'
#' `share()` is idempotent: calling it on an object that is already backed by
#' shared memory returns the input unchanged without allocating a new region.
#'
#' **Important**: ensure the return value of `share()` is not garbage
#' collected before a consumer can map its shared memory.
#'
#' @section Persistence:
#' Direct [saveRDS()] of a shared object writes only the shared memory name,
#' so the resulting file is meaningful only on the same machine while the
#' region is still alive. For portable storage or transport across machines,
#' materialise into a regular in-memory copy first with `rlang::duplicate()`,
#' which deep-duplicates the object:
#'
#' ```r
#' saveRDS(rlang::duplicate(x), file = "x.rds")
#' ```
#'
#' @examples
#' x <- share(1:100)
#' sum(x)
#'
#' lst <- share(list(a = 1:3, b = letters))
#' is_shared(lst)
#'
#' @seealso [map_shared()] to open a shared region by name,
#'   [shared_name()] to extract the shared memory name.
#'
#' @export
share <- function(x) .Call(mori_create, x)

#' Open Shared Memory by Name
#'
#' Open a shared memory region identified by a name string and return an
#' ALTREP-backed R object that reads directly from shared memory.
#'
#' @param name a character string as returned by [shared_name()]: either a
#'   bare shared memory name (opens the root) or a name with a 1-based
#'   bracketed index path (e.g. `"/mori_abc_1[2,3]"`, opens the addressed
#'   sub-list or element directly).
#'
#' @return The R object stored at the named region (or sub-object at the
#'   given path), or `NULL` if `name` is not a valid shared memory name
#'   (wrong type, length, `NA`, missing or malformed prefix, or malformed
#'   bracketed path). If `name` parses as valid but the region is absent or
#'   corrupted — or the path doesn't address a valid sub-object — an error
#'   is raised.
#'
#' @examples
#' x <- share(1:100)
#' nm <- shared_name(x)
#' map_shared(nm)
#'
#' # A bracketed index path opens the addressed sub-object directly:
#' lst <- share(list(a = 1:3, b = letters))
#' map_shared(shared_name(lst[[2]]))
#'
#' @seealso [share()] to create a shared object, [shared_name()] to extract
#'   the shared memory name.
#'
#' @export
map_shared <- function(name) .Call(mori_shm_open_and_wrap, name)

#' Test if an Object is Shared
#'
#' Returns `TRUE` if `x` is an ALTREP object backed by shared memory
#' (created by [share()] or [map_shared()]), `FALSE` otherwise.
#'
#' @param x an R object.
#'
#' @return `TRUE` or `FALSE`.
#'
#' @examples
#' x <- 1:100
#' y <- share(x)
#' is_shared(y)
#' is_shared(x)
#'
#' @export
is_shared <- function(x) .Call(mori_is_shared, x)

#' Extract Shared Memory Name
#'
#' Extract the shared memory name from a shared object. This name can be
#' passed to [map_shared()] to open the same region in another process.
#'
#' @param x a shared object as returned by [share()] or [map_shared()].
#'
#' @return A character string identifying the shared object, or `NULL` if
#'   `x` is not a shared object. For a sub-list or element extracted from a
#'   shared list, the string carries a bracketed 1-based index path (e.g.
#'   `"/mori_abc_1[2,3]"`). [map_shared()] accepts both forms; the
#'   path-qualified form returns the addressed sub-object directly. The
#'   underlying shared memory region name is the prefix before `[` and is
#'   recoverable via `sub("\\[.*$", "", shared_name(x))`.
#'
#' @examples
#' x <- share(1:100)
#' shared_name(x)
#'
#' # A sub-object extracted from a shared list carries a bracketed index path:
#' lst <- share(list(a = 1:3, b = letters))
#' shared_name(lst[[2]])
#'
#' @seealso [map_shared()] to open a shared region by name.
#'
#' @export
shared_name <- function(x) .Call(mori_shm_name, x)

#' Prune Orphaned Shared Memory Regions
#'
#' @description
#' Recover shared memory regions leaked by a process that was killed before it
#' could clean up — for example after a crash, a `SIGKILL`, or the
#' out-of-memory killer. **You do not normally need this:** mori frees shared
#' memory automatically when the [share()] object is garbage-collected and on a
#' clean R session exit (see Details).
#'
#' This function is only relevant on Linux and macOS. On Windows, shared memory
#' cannot be orphaned, so there is never anything to clean up and calling it has
#' no effect.
#'
#' @details
#' Shared memory is normally managed automatically: a region is unlinked when
#' the [share()] object that owns it is garbage-collected, and on a clean R
#' session exit. A region is only left behind if the owning process is killed
#' before either can run. `prune_shared()` removes such leftovers.
#'
#' Pruning is **conservative**: a region is removed only if its creating
#' process (encoded in the region name) is no longer running, so regions
#' still in use by a live process are never touched. Removing a region unlinks
#' only its name; processes that have already mapped it keep reading until they
#' release it, and the memory is freed once the last mapping is gone.
#'
#' @return Invisibly, a character vector of the region names that were
#'   removed, or `NULL` if nothing was removed.
#'
#' @examples
#' # Shared memory is freed automatically - you do not normally call this.
#' # To prune regions left behind by a crashed process:
#' prune_shared()
#'
#' @export
prune_shared <- function() invisible(.Call(mori_prune))
