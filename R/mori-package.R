#' mori: Shared Memory for R Objects
#'
#' Share R objects via shared memory with [share()], access them in
#' other processes with [map_shared()], using R's ALTREP framework for
#' zero-copy memory-mapped access. Shared objects serialize compactly
#' via ALTREP serialization hooks. Shared memory is automatically freed
#' when the R object is garbage collected.
#'
#' @section mori and mizu:
#' The \pkg{mizu} package provides a complete async and parallel framework
#' over the same shared-memory core — task pools, a parallel map, channels,
#' and Python interop. Objects created by [share()] travel through mizu
#' tasks as zero-copy references. mori remains the right choice when a
#' parallel backend such as mirai or parallel is already in place, or when
#' data must be mapped by name between independent processes or outlive
#' any pool.
#'
#' @useDynLib mori, .registration = TRUE
"_PACKAGE"
