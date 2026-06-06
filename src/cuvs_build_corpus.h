/*
 * cuvs_build_corpus.h — leak-safe build-payload handoff for the CAGRA index
 * build (ADR-057). The backend accumulates the corpus (vectors + TIDs) in a
 * single buffer and hands it to the GPU daemon. Three tiers, selected at
 * runtime, all producing the daemon's [vectors][tids] contiguous layout:
 *
 *   T1 CORPUS_MEMFD — anonymous memfd; the fd is passed to the daemon via
 *                     SCM_RIGHTS. No name in /dev/shm, so a crash can never
 *                     leave an orphan: the kernel reclaims the memory when the
 *                     last fd/mapping is gone (incl. SIGKILL). Default.
 *   T2 CORPUS_SHM   — named POSIX shm (/pg_cuvs_bld_<pid>_<seq>), daemon
 *                     shm_open()s it by name. flock(LOCK_EX) is held for the
 *                     segment's life so a reaper can tell a dead owner (lock
 *                     auto-released by the kernel) from a live build. Used only
 *                     when memfd_create is unavailable (old kernel / seccomp).
 *   T3 CORPUS_HEAP  — caller-managed heap buffer + legacy named-shm copy at
 *                     handoff. Degenerate fallback when shm_open also fails
 *                     (a system with no working shared memory — search is dead
 *                     too). Build still succeeds.
 *
 * PostgreSQL-free by design so the laptop/CI unit test exercises the tier
 * selection, grow/finalize byte-identity, memfd refcount semantics, and the
 * flock reaper without PGXS, CUDA, or a daemon.
 */
#ifndef CUVS_BUILD_CORPUS_H
#define CUVS_BUILD_CORPUS_H

#include <stddef.h>

typedef enum {
    CORPUS_NONE = 0,
    CORPUS_MEMFD,   /* T1 */
    CORPUS_SHM,     /* T2 */
    CORPUS_HEAP,    /* T3 — caller owns the buffer */
} CorpusKind;

typedef struct CuvsBuildCorpus {
    CorpusKind kind;
    int    fd;            /* memfd (T1) or shm (T2) fd; -1 for heap/none */
    char   shm_name[64];  /* T2 only ("/pg_cuvs_bld_<pid>_<seq>"); "" otherwise */
    void  *base;          /* writable mapping (T1/T2); NULL for heap */
    size_t map_bytes;     /* current ftruncate'd / mapped size */
} CuvsBuildCorpus;

/*
 * Open a corpus buffer of `init_bytes`. Selects the tier (memfd -> shm), maps
 * it, and fills *c. If neither memfd nor shm is available, sets kind=CORPUS_HEAP
 * with base=NULL (the caller then manages a heap buffer itself) and returns 0.
 * Returns 0 on success (any tier), -1 only on an unexpected fatal error.
 * The memfd-vs-shm probe result is cached process-wide after the first call.
 */
int  cuvs_corpus_open(CuvsBuildCorpus *c, size_t init_bytes);

/*
 * Grow or shrink the mapped buffer to `new_bytes` (ftruncate + remap). Updates
 * c->base and c->map_bytes; written bytes survive (backed by the fd, not the
 * mapping). No-op returning 0 for CORPUS_HEAP. Returns 0 / -1 (errno set).
 */
int  cuvs_corpus_resize(CuvsBuildCorpus *c, size_t new_bytes);

/*
 * munmap + close (+ shm_unlink for T2). Idempotent; safe on an empty handle.
 * For T1 the close drops this process's reference (the daemon keeps its own
 * passed copy alive until it closes). Clears *c.
 */
void cuvs_corpus_close(CuvsBuildCorpus *c);

/*
 * Release this process's mapping/fd WITHOUT shm_unlink, returning the segment
 * name (copied into `name_out`, size `name_len`) so another process can open it
 * by name (§4A-2 parallel build: a worker hands its T2 partial to the leader,
 * which opens + unlinks it after merge). Only meaningful for CORPUS_SHM; for
 * memfd/heap there is no name to hand off — name_out is set "" and the segment
 * is closed normally. Clears *c.
 */
void cuvs_corpus_detach(CuvsBuildCorpus *c, char *name_out, size_t name_len);

/*
 * T2 reaper: scan /dev/shm for `pg_cuvs_bld_*` segments whose owner is dead
 * (no live flock holder) and, if do_unlink, shm_unlink them. Segments younger
 * than a few seconds are skipped (create-window race guard). Returns the number
 * of dead-owner orphans found (whether or not unlinked); -1 on scan failure.
 * No-op returning 0 where /dev/shm is absent. Reaper is meaningful only in the
 * T2 fallback environment (T1 creates no named segment).
 */
int  cuvs_corpus_reap_orphans(int do_unlink);

/* ----------------------------------------------------------------
 * SCM_RIGHTS fd passing (generic; used for the T1 handoff). Kept here so the
 * plumbing is unit-testable over a socketpair without PG or a daemon.
 * ---------------------------------------------------------------- */

/*
 * Send `len` bytes of `payload` over `sock` and, if fd >= 0, attach it as an
 * SCM_RIGHTS ancillary fd. Returns 0 / -1. (T2/T3 call with fd=-1, so the same
 * path carries the index_dir whether or not an fd rides along.)
 */
int  cuvs_fd_send(int sock, int fd, const void *payload, size_t len);

/*
 * Receive `len` bytes into `payload` and any SCM_RIGHTS fd into *out_fd
 * (-1 if none). Returns 0 / -1. Treats truncated ancillary data (MSG_CTRUNC)
 * as an error.
 */
int  cuvs_fd_recv(int sock, void *payload, size_t len, int *out_fd);

/* ----------------------------------------------------------------
 * Test seam: force a tier regardless of availability ("memfd"|"shm"|"heap"),
 * or NULL to restore auto-probe. Driven by CUVS_FORCE_CORPUS in tests.
 * ---------------------------------------------------------------- */
void cuvs_corpus_force_kind(const char *kind);

#endif /* CUVS_BUILD_CORPUS_H */
