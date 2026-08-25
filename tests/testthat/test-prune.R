# prune_shared() reclaims regions orphaned by a dead process: it enumerates
# /dev/shm on Linux and mori's own per-user registry directory (<TMPDIR>/mori)
# on macOS. Windows cannot orphan shared memory and is skipped at file level.

skip_on_os("windows")

# Resolve mori's macOS registry directory exactly as the C side does (TMPDIR with
# any trailing slash stripped, plus "/mori").
mori_registry_dir <- function() {
  tmp <- Sys.getenv("TMPDIR")
  if (!nzchar(tmp)) {
    skip("TMPDIR unset; cannot locate mori registry directory")
  }
  file.path(sub("/+$", "", tmp), "mori")
}

# The on-disk record pruning scans for, for a region with the given "/mori_..."
# name created by `pid`: on Linux the region file in /dev/shm itself; on macOS
# the process's registry log <TMPDIR>/mori/mori_<pid>, whose lines name that
# process's regions. Pruning classifies purely by the PID in the name.
orphan_record_path <- function(pid, shm_name) {
  if (Sys.info()[["sysname"]] == "Linux") {
    file.path("/dev/shm", sub("^/", "", shm_name))
  } else {
    file.path(mori_registry_dir(), sprintf("mori_%x", pid))
  }
}

# Fabricate such a record on disk, returning its path and the "/mori_..." name.
fabricate_orphan_record <- function(pid, counter = "0") {
  shm_name <- sprintf("/mori_%x_%s", pid, counter)
  record <- orphan_record_path(pid, shm_name)
  if (Sys.info()[["sysname"]] == "Linux") {
    file.create(record) # the region file itself
  } else {
    # Darwin: a per-process log
    dir.create(dirname(record), showWarnings = FALSE, mode = "0700")
    writeLines(shm_name, record) # one region name per line
  }
  list(record = record, shm_name = shm_name)
}

test_that("prune_shared() returns invisibly", {
  expect_invisible(prune_shared())
})

test_that("prune_shared() leaves live regions untouched", {
  x <- share(rnorm(100))
  nm <- shared_name(x)

  prune_shared()
  expect_silent(y <- map_shared(nm)) # region of a live process survives pruning
  expect_equal(sum(y), sum(x))
})

test_that("prune_shared() returns NULL when nothing is orphaned", {
  prune_shared() # clear any pre-existing orphans
  expect_null(prune_shared()) # a second prune finds nothing to remove
})

test_that("prune_shared() removes a dead process's orphan", {
  # Spawn a short-lived shell, capture its PID, and let R reap it (system()
  # waits before returning) so the PID is dead. Fabricate an orphan record
  # named for that PID; pruning classifies purely by the PID in the name, so
  # this exercises the dead-PID branch deterministically.
  dead_pid <- as.integer(system("echo $$", intern = TRUE))

  orphan <- fabricate_orphan_record(dead_pid)
  defer(unlink(orphan$record))

  pruned <- prune_shared()

  # The record is cleared on every platform. On Linux the /dev/shm file is a
  # real region, so its name is also reported as removed; on macOS the record is
  # a log naming a region that does not exist, so shm_unlink finds nothing to
  # reclaim and the name is not reported (reported only when a region was).
  expect_false(file.exists(orphan$record))
  if (Sys.info()[["sysname"]] == "Linux") {
    expect_true(orphan$shm_name %in% pruned)
  } else {
    expect_false(orphan$shm_name %in% pruned)
  }
})

test_that("prune_shared() keeps a live process's record", {
  # Pruning must skip the current (live) process. Share a real object so this
  # process owns a registry record, then prune and confirm the record — and the
  # region — survive (the live PID is classified alive).
  x <- share(rnorm(100))
  nm <- shared_name(x)
  record <- orphan_record_path(Sys.getpid(), nm)

  pruned <- prune_shared()
  expect_false(nm %in% pruned)
  expect_true(file.exists(record))
  expect_silent(map_shared(nm))

  rm(x)
  gc()
})

# macOS registry-directory lifecycle: the directory <TMPDIR>/mori is created on
# first share(), pruned once this process's last region is finalised, and
# recreated on demand. macOS-only — Linux enumerates /dev/shm directly, Windows
# has no registry. Pruning is finalisation-driven (the process owns one log for
# all its regions).

test_that("a live region keeps the registry directory", {
  skip_on_os(c("linux", "solaris"))
  dir <- mori_registry_dir()

  x <- share(rnorm(10)) # holds the process's log, and so the dir, open
  expect_true(dir.exists(dir))

  rm(x)
  gc()
})

test_that("the registry directory is pruned once the last region is finalised", {
  skip_on_os(c("linux", "solaris"))
  dir <- mori_registry_dir()

  # Isolate the assertion: finalise any unreferenced shared objects (releasing
  # them from the log) and prune dead-process orphans, so the registry holds
  # nothing we do not control. If something still lingers, skip rather than
  # assert racily.
  gc()
  prune_shared()
  if (length(list.files(dir, pattern = "^mori_")) != 0) {
    skip("registry not empty; cannot isolate the prune assertion")
  }

  x <- share(rnorm(10)) # the sole live region; holds the log open
  expect_true(dir.exists(dir))

  rm(x)
  gc() # finalise it -> last region gone -> dir pruned
  expect_false(dir.exists(dir))
})

test_that("share() recreates the registry directory on demand", {
  skip_on_os(c("linux", "solaris"))
  dir <- mori_registry_dir()

  x <- share(rnorm(10)) # present whether or not the dir was just pruned
  nm <- shared_name(x)
  expect_true(dir.exists(dir))

  log <- file.path(dir, sprintf("mori_%x", Sys.getpid()))
  expect_true(file.exists(log)) # this process's registry log
  expect_true(nm %in% readLines(log)) # the region is recorded for pruning

  rm(x)
  gc()
})

test_that("pruning an idle process does not create the registry directory", {
  skip_on_os(c("linux", "solaris"))
  dir <- mori_registry_dir()

  gc() # finalise unreferenced shared objects (prunes their dir)
  prune_shared() # prune dead-process orphans
  if (dir.exists(dir)) {
    skip("registry still present; cannot isolate")
  }

  # Path resolution is pure: a prune that finds no registry must not create one.
  expect_null(prune_shared())
  expect_false(dir.exists(dir))
})

test_that("prune_shared() reaps regions of a SIGKILLed process", {
  # A process killed with SIGKILL runs no finalizers, leaving a genuine
  # orphan: the region still exists when pruned, so unlinking it succeeds and
  # its name is reported (a gracefully-exited process's record names regions
  # already gone, which exercises only the skip path). The subprocess is
  # detached — reparented to init — so the OS, not this session, reaps it.
  rbin <- file.path(R.home("bin"), "R")
  info <- tempfile()
  script <- sprintf(
    paste(
      "x <- mori::share(1:10);",
      "writeLines(c(Sys.getpid(), mori::shared_name(x)), %s);",
      "Sys.sleep(120)"
    ),
    shQuote(info)
  )
  system2(
    "sh",
    c(
      "-c",
      shQuote(paste(
        shQuote(rbin),
        "--vanilla",
        "-q",
        "-e",
        shQuote(script),
        ">/dev/null 2>&1 &"
      ))
    ),
    env = paste0("R_LIBS=", paste(.libPaths(), collapse = .Platform$path.sep))
  )

  deadline <- Sys.time() + 15
  while (!file.exists(info) && Sys.time() < deadline) {
    Sys.sleep(0.05)
  }
  if (!file.exists(info)) {
    skip("detached process did not create a region in time")
  }
  l <- readLines(info)
  pid <- as.integer(l[1L])
  nm <- l[2L]

  tools::pskill(pid, tools::SIGKILL)
  deadline <- Sys.time() + 5
  while (tools::pskill(pid, 0) && Sys.time() < deadline) {
    Sys.sleep(0.05)
  }
  if (tools::pskill(pid, 0)) {
    skip("killed process not reaped by the OS in time")
  }

  pruned <- prune_shared()
  expect_true(nm %in% pruned)
  expect_error(map_shared(nm), "not found")
})

test_that("share() works with TMPDIR unset (confstr fallback)", {
  skip_on_os(c("linux", "solaris"))
  # With TMPDIR empty the macOS registry dir resolves via confstr's per-user
  # temp dir; sharing must still work (and the region is released normally at
  # exit, so nothing leaks into the registry).
  rbin <- file.path(R.home("bin"), "R")
  script <- "library(mori); x <- share(1:5); stopifnot(is_shared(x)); cat('OK\n')"
  out <- system2(
    rbin,
    c("--vanilla", "-q", "-e", shQuote(script)),
    stdout = TRUE,
    stderr = TRUE,
    env = c(
      "TMPDIR=",
      paste0("R_LIBS=", paste(.libPaths(), collapse = .Platform$path.sep))
    )
  )
  expect_true(any(grepl("OK", out, fixed = TRUE)))
})
