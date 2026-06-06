/*
 * cuvs_build_shm.h — POSIX shm segment lifecycle for the CAGRA build payload
 * (ADR-034 §4A-1: allocate the build corpus directly in shared memory so the
 * heap->shm double memcpy is eliminated).
 *
 * Deliberately free of PostgreSQL headers so the laptop unit test
 * (test/unit/test_build_shm.c, built by `make test-unit`) can exercise the
 * grow/finalize/destroy path without PGXS, CUDA, or a daemon.
 *
 * Lifetime model: create() before the heap scan, write vectors directly into
 * base[], resize() to grow capacity, resize() to the exact total + append the
 * (small) TID region after the scan, then destroy() once the daemon has read
 * it. destroy() is idempotent so the caller's PG_FINALLY can reclaim the
 * segment on any longjmp out of the scan.
 */
#ifndef CUVS_BUILD_SHM_H
#define CUVS_BUILD_SHM_H

#include <stddef.h>

/* Handle for one build-payload shm segment. `base` is the current mmap
 * address and moves on every resize() — never cache it across a resize. */
typedef struct CuvsBuildShm {
    int    fd;          /* shm fd, -1 when none is held */
    char   key[64];     /* shm_open name; "" when none is held */
    void  *base;        /* current MAP_SHARED mapping, NULL when none */
    size_t map_bytes;   /* current ftruncate'd / mapped size in bytes */
} CuvsBuildShm;

/*
 * Create a uniquely-named shm segment of `bytes`, mode 0666 (so the daemon,
 * which runs as a different uid, can shm_open it), and mmap it RW MAP_SHARED.
 * On success fills *sh and returns 0. On failure returns -1 with errno set and
 * leaves *sh empty (fd=-1, base=NULL) — already unlinked, nothing to clean up.
 */
int cuvs_build_shm_create(CuvsBuildShm *sh, size_t bytes);

/*
 * Grow or shrink the segment to `new_bytes`: ftruncate then remap
 * (munmap + mmap; portable, no Linux-only mremap). Bytes already written are
 * preserved because they live in the shm object, not the mapping. Updates
 * sh->base and sh->map_bytes. Returns 0 on success, -1 on failure (errno set;
 * on a failed remap sh->base is set NULL so destroy() still closes+unlinks).
 */
int cuvs_build_shm_resize(CuvsBuildShm *sh, size_t new_bytes);

/*
 * munmap + close + shm_unlink. Idempotent: safe to call twice and safe on an
 * empty handle. Clears *sh.
 */
void cuvs_build_shm_destroy(CuvsBuildShm *sh);

#endif /* CUVS_BUILD_SHM_H */
