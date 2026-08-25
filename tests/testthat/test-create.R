# share() picks a fresh name per call (mori_<pid>_<counter>) and creates it with
# O_EXCL, so it never reuses or mutates an existing region. A name collision with
# an orphan left by a previous process that reused the same PID is therefore
# surfaced as an error (prune_shared() is the mechanism for clearing such
# orphans), not worked around. Deterministic only where regions are files we can
# pre-create (Linux /dev/shm).

test_that("share() errors when the name it would use is already taken", {
  if (Sys.info()[["sysname"]] != "Linux") {
    skip("requires file-backed /dev/shm (Linux only)")
  }

  x <- share(1:10)
  nm <- shared_name(x) # "/mori_<pid>_<counter>"
  parts <- regmatches(nm, regexec("^/mori_([0-9a-f]+)_([0-9a-f]+)$", nm))[[1]]
  expect_length(parts, 3L)
  pid_hex <- parts[2]
  # The counter is a random uint32; parse as double (strtoi() overflows R's
  # signed int above 2^31) and format the next value as C's "%x" would.
  counter <- as.numeric(paste0("0x", parts[3]))
  next_counter <- (counter + 1) %% 2^32
  hi <- next_counter %/% 0x10000
  lo <- next_counter %% 0x10000
  next_hex <- if (hi > 0) sprintf("%x%04x", hi, lo) else sprintf("%x", lo)

  # Occupy the exact name share() would use next, so its O_EXCL create collides.
  collide <- sprintf("/dev/shm/mori_%s_%s", pid_hex, next_hex)
  file.create(collide)
  defer(unlink(collide))

  # The error envelope carries the requested size, the collision summary, and
  # the prune_shared() remediation hint.
  expect_error(
    share(1:10),
    "cannot create region.*already in use.*prune_shared"
  )
})

# 1:1e15 is a compact ALTREP seq (O(1) memory), so share() requests a ~7 PB
# region without the test ever allocating it, hitting mori_err_classify on the
# live create failure. The mapped category varies by platform (ENOMEM on macOS,
# ENOSPC on Linux's size-capped tmpfs), so we assert only the size envelope.
test_that("share() errors cleanly when the region is too large to back", {
  if (.Machine$sizeof.pointer < 8) {
    skip("long vectors unsupported on 32-bit; PB-region path unreachable")
  }

  expect_error(
    share(1:1e15),
    "cannot create region \\(requested .*PB\\)"
  )
})

# File-descriptor exhaustion: with the process at its fd limit the create fails
# with EMFILE — neither space, memory, nor a name collision — exercising the
# catch-all error category. R needs a few hundred fds to start and its
# connection table caps below that, so the shell pre-opens fds that R inherits
# and R's own connections take the rest.
test_that("share() errors cleanly when file descriptors are exhausted", {
  skip_on_os("windows")

  rbin <- file.path(R.home("bin"), "R")
  script <- paste(
    "library(mori);",
    "invisible(list(share, is_shared));", # force lazy-load before exhaustion
    "cat('STARTED\\n');",
    "cons <- list();",
    "for (i in 1:200) {",
    "  con <- try(file('/dev/null', 'r'), silent = TRUE);",
    "  if (inherits(con, 'try-error')) break;",
    "  cons[[length(cons) + 1L]] <- con;",
    "};",
    "msg <- tryCatch({ share(1:10); 'NO ERROR' }, error = conditionMessage);",
    "cat('share:', msg, '\\n')"
  )
  of <- tempfile()
  cmd <- paste0(
    "ulimit -n 256 || exit 3; ",
    "i=10; while [ $i -lt 160 ]; do eval \"exec ${i}< /dev/null\"; i=$((i+1)); done; ",
    "R_LIBS=",
    shQuote(paste(.libPaths(), collapse = .Platform$path.sep)),
    " ",
    shQuote(rbin),
    " --vanilla -q -e ",
    shQuote(script),
    " > ",
    shQuote(of),
    " 2>&1"
  )
  system2("sh", c("-c", shQuote(cmd)))
  out <- readLines(of)
  if (!any(grepl("STARTED", out, fixed = TRUE))) {
    skip("R subprocess could not start with a 256-fd limit")
  }
  expect_true(any(grepl("cannot create region.*unexpected error", out)))
})
