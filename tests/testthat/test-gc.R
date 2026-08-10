test_that("GC cleans up a shared vector", {
  x <- share(1:100)
  nm <- shared_name(x)
  rm(x)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("GC cleans up a shared list/data frame", {
  x <- share(data.frame(a = 1:10, b = as.double(1:10)))
  nm <- shared_name(x)
  rm(x)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("GC cleans up a shared string vector", {
  x <- share(letters)
  nm <- shared_name(x)
  rm(x)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("element reference keeps parent SHM alive through GC", {
  x <- share(data.frame(a = 1:10, b = as.double(1:10)))
  nm <- shared_name(x)
  col <- x$a
  rm(x)
  gc()

  y <- map_shared(nm)
  expect_s3_class(y, "data.frame")

  rm(col, y)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("GC in a forked child does not unlink the parent's region", {
  skip_on_os("windows")
  # Fork is unavailable in some front-ends (e.g. a Positron session)
  fork_ok <- tryCatch(
    {
      parallel::mccollect(parallel::mcparallel(TRUE))
      TRUE
    },
    error = function(e) FALSE
  )
  skip_if_not(fork_ok, "fork not available in this session")

  x <- share(1:10)
  nm <- shared_name(x)
  # The child drops its inherited reference and GCs, running the inherited
  # host finalizer; the parent's region name must survive.
  job <- parallel::mcparallel({
    rm(x)
    gc()
    TRUE
  })
  expect_identical(parallel::mccollect(job)[[1]], TRUE)

  expect_silent(y <- map_shared(nm))
  expect_identical(y[], 1:10)
})
