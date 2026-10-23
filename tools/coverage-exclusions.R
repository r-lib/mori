# Compute covr `line_exclusions` for mori's vendored C layer.
#
# The view layer (src/mori.h, src/altrep.c — vendored from mizu) and the
# region layer (src/mori_region.h, src/shm.c — from libmizu) carry code mori
# can never execute but tools/vendor.sh cannot carve without editing live
# functions:
#
#   * the cross-language staging mode (foreign = 1) and its NA bitmap
#     machinery — mori_create hard-wires foreign = 0;
#   * the embedder veto walk (ok != NULL, sp != NULL) — mori sizes with
#     ok = NULL and no pre-measured sums;
#   * ALTREP methods R never calls while Dataptr_or_null returns the SHM
#     pointer (the Get_region set) or the vector is a list (Dataptr on
#     VECSXP);
#   * the release-hook fire branch — the hook setters are not vendored, so
#     no hook is ever armed;
#   * the write-unwind cleanup's failure arm (a write that raises into a
#     fresh region).
#
# Ranges are located by text anchors, not line numbers, so they track
# re-vendoring. Every anchor must match exactly as many times as declared:
# drift is an error in the coverage workflow, never a silent wrong range.

mori_coverage_exclusions <- function(pkg = ".") {
  read_src <- function(name) {
    lines <- readLines(file.path(pkg, "src", name), warn = FALSE)
    attr(lines, "name") <- name
    lines
  }

  # Positions of all matches; fails unless exactly `n`.
  at <- function(lines, pattern, n = 1L) {
    m <- grep(pattern, lines, perl = TRUE)
    if (length(m) != n) {
      stop(
        "coverage-exclusions: anchor '",
        pattern,
        "' matched ",
        length(m),
        " times (expected ",
        n,
        ") in ",
        attr(lines, "name")
      )
    }
    m
  }

  # Whole-function span: the signature line to the next '}' at column 0.
  fn_span <- function(lines, sig) {
    s <- at(lines, sig)
    close <- grep("^}", lines, perl = TRUE)
    close <- close[close > s][1L]
    if (is.na(close)) {
      stop(
        "coverage-exclusions: no closing brace after '",
        sig,
        "' in ",
        attr(lines, "name")
      )
    }
    seq.int(s, close)
  }

  # Offsets relative to each of the n anchor matches.
  rel <- function(lines, pattern, lo, hi, n = 1L) {
    unlist(lapply(at(lines, pattern, n), function(m) seq.int(m + lo, m + hi)))
  }

  # From the line after the anchor to the first '}' at two-space indent
  # (an `if (foreign ...) { ... }` block; the condition line itself is
  # executed and stays in coverage).
  block_close <- function(lines, pattern) {
    s <- at(lines, pattern)
    close <- grep("^  }$", lines, perl = TRUE)
    close <- close[close > s][1L]
    if (is.na(close)) {
      stop(
        "coverage-exclusions: no closing brace after '",
        pattern,
        "' in ",
        attr(lines, "name")
      )
    }
    seq.int(s + 1L, close)
  }

  altrep <- read_src("altrep.c")
  altrep_ex <- c(
    # Get_region: unreachable while Dataptr_or_null returns the SHM pointer
    # (every region-iterating consumer takes the pointer fast path).
    fn_span(altrep, "^static R_xlen_t mori_vec_region\\("),
    fn_span(altrep, "^static R_xlen_t mori_real_Get_region\\("),
    fn_span(altrep, "^static R_xlen_t mori_integer_Get_region\\("),
    fn_span(altrep, "^static R_xlen_t mori_logical_Get_region\\("),
    fn_span(altrep, "^static R_xlen_t mori_raw_Get_region\\("),
    fn_span(altrep, "^static R_xlen_t mori_complex_Get_region\\("),
    # DATAPTR on a VECSXP: no base R code path requests one.
    fn_span(altrep, "^static const void \\*mori_list_Dataptr_or_null\\("),
    fn_span(altrep, "^static void \\*mori_list_Dataptr\\("),
    # foreign = 1 staging: the NA gate and the validity-section writes.
    fn_span(
      altrep,
      "^static int mori_na_present\\(int type, const void \\*src, uint64_t n\\) \\{"
    ),
    block_close(altrep, "^  if \\(foreign\\) \\{$"),
    block_close(altrep, "^  if \\(foreign && type != RAWSXP\\) \\{$"),
    rel(altrep, "^  if \\(foreign && TYPEOF\\(x\\) != RAWSXP\\)$", 1L, 1L),
    rel(
      altrep,
      "if \\(foreign && type != STRSXP && type != RAWSXP\\) \\{$",
      1L,
      3L
    ),
    # The embedder veto walk (mori_create passes ok = NULL, sp = NULL).
    rel(altrep, "if \\(ok != NULL && !foreign\\) \\{$", 1L, 4L, n = 2L),
    rel(
      altrep,
      "if \\(ok != NULL\\) \\{ \\*ok = 0; UNPROTECT\\(1\\); return 0; \\}$",
      0L,
      0L
    ),
    rel(altrep, "if \\(ok != NULL && !\\*ok\\) return 0;$", 0L, 0L),
    rel(altrep, "if \\(sp != NULL\\) \\{$", 1L, 4L),
    # The unwind cleanup's failure arm.
    rel(altrep, "^static void mori_write_cleanup\\(", 2L, 5L),
    # The hook branches: the setters are not vendored, so no hook is ever
    # armed — the release fire and the attrs blob hook dispatch are dead.
    rel(altrep, "^static inline void mori_release_once\\(", 2L, 4L),
    rel(altrep, "^  if \\(mori_attrs_size_hook != NULL\\) \\{$", 1L, 2L),
    rel(altrep, "^  if \\(mori_attrs_write_hook != NULL\\) \\{$", 1L, 4L),
    rel(altrep, "mori_attrs_read_hook != NULL\\)$", 0L, 1L)
  )

  view <- read_src("mori.h")
  view_ex <- fn_span(view, "^static inline int mori_altrep_readable\\(")

  region <- read_src("mori_region.h")
  region_ex <- c(
    # The foreign staging mode's NA bitmap helpers.
    fn_span(region, "^MORI_EXT_INLINE int mori_ext_na_real_bits\\("),
    fn_span(region, "^MORI_EXT_INLINE int mori_ext_na_at\\("),
    fn_span(region, "^MORI_EXT_INLINE void mori_morh_validity_set\\("),
    fn_span(region, "^MORI_EXT_INLINE uint64_t mori_ext_na_build_impl\\("),
    fn_span(region, "^MORI_EXT_INLINE uint64_t mori_na_build\\(")
  )

  list(
    "src/altrep.c" = sort(unique(altrep_ex)),
    "src/mori.h" = sort(unique(view_ex)),
    "src/mori_region.h" = sort(unique(region_ex))
  )
}
