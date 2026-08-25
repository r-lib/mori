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
