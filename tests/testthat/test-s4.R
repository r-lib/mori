test_that("S4 integer vector round-trips through share()", {
  methods::setClass("moriS4Int", contains = "integer")
  x <- methods::new("moriS4Int", 1:5)
  y <- share(x)
  expect_true(is_shared(y))
  expect_true(isS4(y))
  expect_identical(x, y)
})

test_that("S4 object with slots round-trips", {
  methods::setClass("moriS4Slotted", contains = "numeric",
                    slots = c(note = "character"))
  x <- methods::new("moriS4Slotted", c(1.5, 2.5), note = "hello")
  y <- share(x)
  expect_true(isS4(y))
  expect_identical(x, y)
  expect_identical(methods::slot(y, "note"), "hello")
})

test_that("S4 character vector round-trips", {
  methods::setClass("moriS4Chr", contains = "character")
  x <- methods::new("moriS4Chr", c("a", "b"))
  y <- share(x)
  expect_true(is_shared(y))
  expect_true(isS4(y))
  expect_identical(x, y)
})

test_that("S4 list round-trips", {
  methods::setClass("moriS4List", contains = "list")
  x <- methods::new("moriS4List", list(a = 1:3, b = "x"))
  y <- share(x)
  expect_true(is_shared(y))
  expect_true(isS4(y))
  expect_identical(x, y)
})

test_that("S4 vector element of a shared list keeps the bit", {
  methods::setClass("moriS4Elem", contains = "integer")
  x <- methods::new("moriS4Elem", 1:3)
  y <- share(list(a = x, b = 2:4))
  expect_true(isS4(y[[1]]))
  expect_identical(x, y[[1]])
  expect_identical(2:4, y[[2]][])
})

test_that("S4 element two levels deep keeps the bit", {
  methods::setClass("moriS4Deep", contains = "integer")
  x <- methods::new("moriS4Deep", 1:3)
  y <- share(list(list(x)))
  expect_true(isS4(y[[1]][[1]]))
  expect_identical(x, y[[1]][[1]])
})

test_that("map_shared on a path-form S4 element keeps the bit", {
  methods::setClass("moriS4Path", contains = "integer")
  x <- methods::new("moriS4Path", 1:3)
  y <- share(list(x))
  z <- map_shared(shared_name(y[[1]]))
  expect_true(isS4(z))
  expect_identical(x, z)
})

test_that("map_shared on S4 vector and list roots keeps the bit", {
  methods::setClass("moriS4Root", contains = "integer")
  methods::setClass("moriS4RootList", contains = "list")
  x <- methods::new("moriS4Root", 1:3)
  xl <- methods::new("moriS4RootList", list(1:3))
  y <- map_shared(shared_name(share(x)))
  yl <- map_shared(shared_name(share(xl)))
  expect_true(isS4(y))
  expect_true(isS4(yl))
  expect_identical(x, y)
  expect_identical(xl, yl)
})

test_that("data-less S4 object passes through unchanged", {
  methods::setClass("moriS4Bare", slots = c(x = "numeric"))
  x <- methods::new("moriS4Bare", x = 42)
  y <- share(x)
  expect_false(is_shared(y))
  expect_true(isS4(y))
  expect_identical(x, y)
})

test_that("serialize round-trip of a shared S4 vector keeps the bit", {
  methods::setClass("moriS4Ser", contains = "integer")
  x <- methods::new("moriS4Ser", 1:5)
  z <- unserialize(serialize(share(x), NULL))
  expect_true(isS4(z))
  expect_identical(x, z)
})

test_that("mutating a shared S4 vector keeps the bit after COW", {
  methods::setClass("moriS4Cow", contains = "integer")
  x <- methods::new("moriS4Cow", 1:5)
  y <- share(x)
  y[1] <- 99L
  expect_true(isS4(y))
  expect_identical(as.integer(y), c(99L, 2:5))
})
