#ifndef MORI_REGION_H
#define MORI_REGION_H

/* The shared-memory region layer of mori: region create/map/unlink over
   POSIX shm / Win32 file mappings, plus the region wire-format constants.
   This header and shm.c are host-language independent. */

#include <stddef.h>
#include <stdint.h>

// Identifier grammar constants ------------------------------------------------

#define MORI_NAME_MAX        30                /* size of mori_shm.name; fits Windows worst case (Local\\mori_<8hex>_<8hex> = 28) + NUL with 1 byte slack */

#ifdef _WIN32
#define MORI_PREFIX_LITERAL  "Local\\mori_"
#else
#define MORI_PREFIX_LITERAL  "/mori_"
#endif

// Region layout constants -----------------------------------------------------

/* Region magics (first 4 bytes): atomic vector, string vector, list tree. */
#define MORI_MAGIC_VEC   0x4D4F5248u  /* "MORH" */
#define MORI_MAGIC_STR   0x4D4F5253u  /* "MORS" */
#define MORI_MAGIC_LIST  0x4D4F524Cu  /* "MORL" */

/* Every region layout opens with a 64-byte header. Bytes [24-63] are
   reserved (written zero): embedders keep cross-process state there. */
#define MORI_HEADER_SIZE 64

// Types -----------------------------------------------------------------------

typedef struct mori_shm_s {
  void *addr;
  size_t size;
  char name[MORI_NAME_MAX];
  uint8_t name_len;                /* strlen(name); fits since MORI_NAME_MAX < 256 */
  unsigned int pid;                /* creator PID: fork guard for mori_shm_host_release (read on POSIX only; Windows has no fork) */
#ifdef _WIN32
  void *handle;
#endif
} mori_shm;

/* Outcome of an SHM creation attempt: MORI_OK on success, otherwise a
   portable failure category. The platform layer classifies errno /
   GetLastError into one of these; mori_err_describe supplies the user-facing
   summary and remediation hint. */
enum {
  MORI_OK = 0,
  MORI_ENOSPC,   /* ENOSPC / ERROR_DISK_FULL */
  MORI_ENOMEM,   /* ENOMEM / commit limit exceeded */
  MORI_EEXIST,   /* region name already in use (orphan of a reused PID) */
  MORI_EOTHER
};

// shm.c -----------------------------------------------------------------------

int mori_shm_create(mori_shm *shm, size_t size);
int mori_shm_open(mori_shm *shm, const char *name);
void mori_shm_close(mori_shm *shm, int unlink);
int mori_shm_create_heap(mori_shm **out, size_t size);
mori_shm *mori_shm_open_heap(const char *name);

/* Host-side teardown of a created region: releases the SHM name (POSIX:
   unlink) / creator handle (Windows) without touching the mapping, which is
   released independently via mori_shm_close. */
void mori_shm_host_release(mori_shm *shm);

char **mori_shm_reap(int *n);
void mori_err_describe(int category, const char **summary, const char **hint);

#endif /* MORI_REGION_H */
