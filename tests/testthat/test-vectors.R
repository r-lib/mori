test_that("double vector round-trips via map_shared", {
  x <- as.double(1:1000)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_length(y, length(x))
  expect_equal(sum(y), sum(x))
  expect_identical(x, y[])
})

test_that("integer vector round-trips via map_shared", {
  x <- 1:1000L
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y[])
})

test_that("logical vector with NA round-trips via map_shared", {
  x <- c(TRUE, FALSE, NA, TRUE, FALSE)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y[])
})

test_that("raw vector round-trips via map_shared", {
  x <- as.raw(0:255)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y[])
})

test_that("complex vector round-trips via map_shared", {
  x <- complex(10, real = 1:10, imaginary = 10:1)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y[])
})

test_that("matrix preserves dim through share and map_shared", {
  m <- matrix(as.double(1:12), nrow = 3)
  sm <- share(m)
  n <- map_shared(shared_name(sm))
  expect_identical(dim(m), dim(n))
  expect_identical(as.double(m), as.double(n))
})

test_that("empty vector round-trips via map_shared", {
  x <- double(0)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_length(y, 0L)
})

test_that("share returns usable vector", {
  x <- share(as.double(1:10))
  expect_true(is.double(x))
  expect_length(x, 10L)
})

test_that("share round-trips a matrix through itself", {
  m <- matrix(1:12, nrow = 3)
  n <- share(m)
  expect_identical(dim(m), dim(n))
  expect_identical(as.integer(m), as.integer(n))
})

test_that("class-only integer64 round-trips at root and nested", {
  x <- structure(as.double(c(1, 2, 3)), class = "integer64")
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_true(is_shared(y))
  expect_identical(class(y), "integer64")
  expect_identical(unclass(y[]), unclass(x))
  l <- share(list(v = x))
  z <- map_shared(shared_name(l))
  expect_identical(class(z$v), "integer64")
  expect_identical(unclass(z$v[]), unclass(x))
})
