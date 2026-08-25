test_that("standalone double vector round-trips through serialize", {
  x <- share(rnorm(1000))
  y <- unserialize(serialize(x, NULL))
  expect_length(y, 1000L)
  expect_identical(x[], y[])
})

test_that("standalone integer vector round-trips through serialize", {
  x <- share(1:500L)
  y <- unserialize(serialize(x, NULL))
  expect_identical(x[], y[])
})

test_that("standalone string vector round-trips through serialize", {
  x <- share(letters)
  y <- unserialize(serialize(x, NULL))
  expect_identical(x[], y[])
})

test_that("shared data frame round-trips through serialize", {
  df <- data.frame(a = as.double(1:100), b = 1:100)
  x <- share(df)
  y <- unserialize(serialize(x, NULL))
  expect_s3_class(y, "data.frame")
  expect_identical(x$a[], y$a[])
  expect_identical(x$b[], y$b[])
})

test_that("matrix with dim round-trips through serialize", {
  m <- matrix(rnorm(120), nrow = 10)
  x <- share(m)
  y <- unserialize(serialize(x, NULL))
  expect_identical(dim(x), dim(y))
  expect_identical(x[], y[])
})

test_that("COW-materialized vector falls back to normal serialization", {
  x <- share(as.double(1:100))
  x[1] <- 999
  buf <- serialize(x, NULL)
  y <- unserialize(buf)
  expect_equal(y[1], 999)
  expect_length(y, 100L)
  # normal serialization is much larger than the compact ~30-byte form
  expect_gt(length(buf), 200)
})

test_that("element vector from list serializes compactly", {
  df <- share(data.frame(a = as.double(1:100), b = 1:100))
  col <- df$a
  buf <- serialize(col, NULL)
  expect_lt(length(buf), 1000)
  y <- unserialize(buf)
  expect_identical(col[], y[])
})

test_that("element string from list serializes compactly", {
  x <- share(list(a = 1:100, b = letters))
  elem <- x[[2]]
  buf <- serialize(elem, NULL)
  expect_lt(length(buf), 1000)
  y <- unserialize(buf)
  expect_identical(elem[], y[])
})

test_that("COW-materialized element falls back to normal serialization", {
  x <- share(list(a = 1:10))
  elem <- x[[1]]
  elem[1] <- 99L
  y <- unserialize(serialize(elem, NULL))
  expect_identical(as.integer(y), c(99L, 2:10))
})

test_that("compact serialization is much smaller than full data", {
  x <- share(rnorm(1e5))
  buf <- serialize(x, NULL)
  expect_lt(length(buf), 1000)
})

test_that("COW-materialized string vector round-trips through serialize", {
  x <- share(c("alpha", "beta", "gamma"))
  y <- map_shared(shared_name(x))
  y2 <- y
  y2[1] <- "delta"
  z <- unserialize(serialize(y2, NULL))
  expect_identical(z, c("delta", "beta", "gamma"))
})

test_that("named vector survives a serialize round-trip", {
  x <- share(c(a = 1, b = 2, c = 3))
  y <- unserialize(serialize(x, NULL))
  expect_identical(names(x), names(y))
  expect_identical(x[], y[])
})

test_that("factor survives a serialize round-trip", {
  x <- share(factor(c("x", "y", "x")))
  y <- unserialize(serialize(x, NULL))
  expect_s3_class(y, "factor")
  expect_identical(levels(x), levels(y))
  expect_identical(as.integer(x), as.integer(y))
})

test_that("Date survives a serialize round-trip", {
  x <- share(as.Date("2024-01-01") + 0:4)
  y <- unserialize(serialize(x, NULL))
  expect_s3_class(y, "Date")
  expect_identical(x[], y[])
})

test_that("named character survives a serialize round-trip", {
  x <- share(c(a = "hello", b = "world"))
  y <- unserialize(serialize(x, NULL))
  expect_identical(names(x), names(y))
  expect_identical(x[], y[])
})

test_that("data frame with factor and Date columns round-trips through serialize", {
  df <- data.frame(
    id = factor(c("a", "b")),
    date = as.Date("2024-01-01") + 0:1,
    val = c(10, 20)
  )
  x <- share(df)
  y <- unserialize(serialize(x, NULL))
  expect_s3_class(y, "data.frame")
  expect_s3_class(y$id, "factor")
  expect_s3_class(y$date, "Date")
  expect_identical(df$id, y$id)
  expect_identical(df$date, y$date[])
  expect_identical(df$val, y$val[])
})

test_that("factor column extracted from list serializes compactly with attrs", {
  df <- share(data.frame(id = factor(c("a", "b", "c")), val = 1:3))
  col <- df$id
  y <- unserialize(serialize(col, NULL))
  expect_s3_class(y, "factor")
  expect_identical(levels(col), levels(y))
  expect_identical(as.integer(col), as.integer(y))
})

# Path-depth fallback: MORI_MAX_PATH is 64, so > 64 levels of sub-list
# nesting force Serialized_state to materialize instead of emitting a
# compact (name, path) reference. Exercised for vec / string / sub-list
# leaves respectively.

test_that("vec leaf beyond MORI_MAX_PATH falls back to materialization", {
  x <- 1:5
  for (i in seq_len(70)) {
    x <- list(x)
  }
  sx <- share(x)
  deep <- sx
  for (i in seq_len(70)) {
    deep <- deep[[1]]
  }
  expect_true(is_shared(deep))
  y <- unserialize(serialize(deep, NULL))
  expect_identical(y[], 1:5)
})

test_that("string leaf beyond MORI_MAX_PATH falls back to materialization", {
  x <- c("alpha", "beta", "gamma")
  for (i in seq_len(70)) {
    x <- list(x)
  }
  sx <- share(x)
  deep <- sx
  for (i in seq_len(70)) {
    deep <- deep[[1]]
  }
  expect_true(is_shared(deep))
  y <- unserialize(serialize(deep, NULL))
  expect_identical(y[], c("alpha", "beta", "gamma"))
})

test_that("length-1 string leaf beyond MORI_MAX_PATH unserializes as-is", {
  # Deep nesting forces Serialized_state to materialize; the materialized
  # STRSXP is wrapped in a length-1 VECSXP so it is never probed as an SHM
  # identifier, and mori_string_Unserialize unwraps it.
  x <- "alpha"
  for (i in seq_len(70)) {
    x <- list(x)
  }
  sx <- share(x)
  deep <- sx
  for (i in seq_len(70)) {
    deep <- deep[[1]]
  }
  expect_true(is_shared(deep))
  y <- unserialize(serialize(deep, NULL))
  expect_identical(y[], "alpha")
})

test_that("deep string leaf whose content is a live identifier round-trips as data", {
  # Regression: a materialized length-1 STRSXP whose content parses as a
  # mori identifier must not be probed — it previously unserialized to the
  # named region's contents (wrong object, wrong type).
  holder <- share(1:10)
  nm <- shared_name(holder)
  x <- nm
  for (i in seq_len(70)) {
    x <- list(x)
  }
  sx <- share(x)
  deep <- sx
  for (i in seq_len(70)) {
    deep <- deep[[1]]
  }
  expect_true(is_shared(deep))
  y <- unserialize(serialize(deep, NULL))
  expect_identical(y, nm)
})

test_that("deep string leaf naming an absent region round-trips as data", {
  # Regression: this previously errored with "region not found".
  x <- "/mori_1_1"
  for (i in seq_len(70)) {
    x <- list(x)
  }
  sx <- share(x)
  deep <- sx
  for (i in seq_len(70)) {
    deep <- deep[[1]]
  }
  expect_identical(unserialize(serialize(deep, NULL)), "/mori_1_1")
})

test_that("sub-list beyond MORI_MAX_PATH falls back to materialization", {
  x <- list(leaf = 1:3)
  for (i in seq_len(70)) {
    x <- list(x)
  }
  sx <- share(x)
  deep <- sx
  for (i in seq_len(68)) {
    deep <- deep[[1]]
  }
  expect_true(is_shared(deep))
  expect_true(is.list(deep))
  y <- unserialize(serialize(deep, NULL))
  final <- y
  while (is.list(final) && is.null(names(final))) {
    final <- final[[1]]
  }
  expect_identical(final$leaf[], 1:3)
})

# Corrupt-stream guards: for the vector and list classes a STRSXP state is
# always an SHM identifier, and for the string class a bare STRSXP state is —
# one that no longer parses as an identifier means a corrupt stream, and
# unserialize must error rather than fall through. The identifier is rewritten
# in place (same length, so the stream stays structurally valid).

test_that("unserialize errors on a tampered vector identifier", {
  x <- share(1:10)
  blob <- serialize(x, NULL)
  pos <- grepRaw(shared_name(x), blob, fixed = TRUE, all = TRUE)
  expect_length(pos, 1L)
  blob[pos[[1]] + 6L] <- charToRaw("z") # first hex digit -> non-hex
  expect_error(
    unserialize(blob),
    "invalid serialized state for a shared object"
  )
})

test_that("unserialize errors on a tampered string identifier", {
  x <- share(c("alpha", "beta"))
  blob <- serialize(x, NULL)
  pos <- grepRaw(shared_name(x), blob, fixed = TRUE, all = TRUE)
  expect_length(pos, 1L)
  blob[pos[[1]] + 6L] <- charToRaw("z")
  expect_error(
    unserialize(blob),
    "invalid serialized state for a shared string vector"
  )
})
