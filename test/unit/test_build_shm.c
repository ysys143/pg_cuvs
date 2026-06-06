/*
 * test_build_shm.c — standalone unit tests for src/cuvs_build_shm.{h,c}.
 *
 * No framework, no PostgreSQL, no CUDA, no GPU. Build + run via `make test-unit`.
 * Returns non-zero on any failure.
 *
 * These prove the ADR-034 §4A-1 shm build-payload path is correct WITHOUT a
 * daemon: the bytes the daemon will read ([vectors][tids] contiguous) are
 * byte-for-byte identical to the legacy heap->shm layout, the final segment is
 * exactly `total` bytes at mode 0666, and destroy() leaves no /dev/shm residue
 * and is idempotent.
 *
 * Growth coverage (the resize path) is Linux-only: a POSIX shm object on macOS
 * rejects a second ftruncate (verified — fails even after unmapping the
 * segment), so the grow path cannot be exercised on a Mac laptop. The extension
 * only ever builds/runs on the Linux GPU VM and the no-GPU CI runner is Linux,
 * so the grow path is covered where it matters; the Mac laptop runs the no-grow
 * golden/perms/residue subset. The exact-size assertion is likewise Linux-only
 * (macOS rounds shm objects up to a page; the daemon maps exactly `total`).
 */

#include "cuvs_build_shm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) {                                                         \
        g_pass++;                                                       \
    } else {                                                            \
        g_fail++;                                                       \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
    }                                                                   \
} while (0)

/* Build a deterministic reference corpus. */
static void
make_corpus(float **vecs, uint64_t **tids, int64_t n, int dim)
{
    *vecs = malloc((size_t) n * dim * sizeof(float));
    *tids = malloc((size_t) n * sizeof(uint64_t));
    for (int64_t i = 0; i < n; i++)
    {
        for (int d = 0; d < dim; d++)
            (*vecs)[i * dim + d] = (float) (i * 1000 + d);
        (*tids)[i] = ((uint64_t) i << 16) | (uint64_t) (i & 0xFFFF);
    }
}

/* Golden buffer == exactly what legacy shm_write_build_payload writes and what
 * the daemon's handle_build maps: vectors then tids, contiguous. */
static char *
make_golden(const float *vecs, const uint64_t *tids, int64_t n, int dim,
            size_t *out_vec_bytes, size_t *out_total)
{
    size_t vec_bytes = (size_t) n * dim * sizeof(float);
    size_t tid_bytes = (size_t) n * sizeof(uint64_t);
    char *golden = malloc(vec_bytes + tid_bytes);
    memcpy(golden, vecs, vec_bytes);
    memcpy(golden + vec_bytes, tids, tid_bytes);
    *out_vec_bytes = vec_bytes;
    *out_total = vec_bytes + tid_bytes;
    return golden;
}

/* Assert the live segment matches golden, is exactly `total` bytes, mode 0666,
 * then destroy and assert no /dev/shm residue. */
static void
check_segment_and_destroy(CuvsBuildShm *sh, const char *golden, size_t total)
{
    ASSERT(memcmp(sh->base, golden, total) == 0, "golden byte-identity");

    struct stat st;
    ASSERT(fstat(sh->fd, &st) == 0, "fstat");
    /* The daemon mmaps exactly `total` (from n_vecs/dim in the frame), so the
     * object only needs to be at least that big. Linux tmpfs ftruncate sets the
     * exact size (no /dev/shm waste); macOS rounds shm objects up to a page. */
    ASSERT((size_t) st.st_size >= total, "segment at least total bytes");
#ifdef __linux__
    ASSERT((size_t) st.st_size == total, "segment size == exact total (Linux)");
#endif
    ASSERT((st.st_mode & 0777) == 0666, "segment mode 0666 (daemon-readable)");

    char key[64];
    strncpy(key, sh->key, sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    cuvs_build_shm_destroy(sh);

    int fd = shm_open(key, O_RDONLY, 0);
    ASSERT(fd < 0 && errno == ENOENT, "no /dev/shm residue after destroy");
    if (fd >= 0) { close(fd); shm_unlink(key); }
}

/* Portable: create at the exact final size, write vectors then tids, verify the
 * daemon-compatible layout. (No resize — runs on macOS and Linux.) */
static void
test_golden_no_grow(int64_t n, int dim)
{
    float *vecs; uint64_t *tids;
    make_corpus(&vecs, &tids, n, dim);
    size_t vec_bytes, total;
    char *golden = make_golden(vecs, tids, n, dim, &vec_bytes, &total);

    CuvsBuildShm sh;
    ASSERT(cuvs_build_shm_create(&sh, total) == 0, "create at exact total");
    memcpy((char *) sh.base, vecs, vec_bytes);
    memcpy((char *) sh.base + vec_bytes, tids, (size_t) n * sizeof(uint64_t));

    check_segment_and_destroy(&sh, golden, total);
    free(vecs); free(tids); free(golden);
}

/* Mirror the real backend path — create small, append vectors one tuple at a
 * time with capacity doubling (grow_build_buffers), then shrink to the exact
 * total and append the TID region (finalize). Prove byte-identity. */
static void
test_golden_with_grow(int64_t n, int dim, int64_t init_rows)
{
    float *vecs; uint64_t *tids;
    make_corpus(&vecs, &tids, n, dim);
    size_t vec_bytes, total;
    char *golden = make_golden(vecs, tids, n, dim, &vec_bytes, &total);

    CuvsBuildShm sh;
    ASSERT(cuvs_build_shm_create(&sh, (size_t) init_rows * dim * sizeof(float)) == 0,
           "create initial");

    int64_t cap = init_rows;
    for (int64_t i = 0; i < n; i++)
    {
        if (i >= cap)
        {
            cap *= 2;
            ASSERT(cuvs_build_shm_resize(&sh, (size_t) cap * dim * sizeof(float)) == 0,
                   "grow resize");
        }
        memcpy((char *) sh.base + (size_t) i * dim * sizeof(float),
               &vecs[i * dim], (size_t) dim * sizeof(float));
    }

    ASSERT(cuvs_build_shm_resize(&sh, total) == 0, "finalize resize to exact total");
    memcpy((char *) sh.base + vec_bytes, tids, (size_t) n * sizeof(uint64_t));

    check_segment_and_destroy(&sh, golden, total);
    free(vecs); free(tids); free(golden);
}

/* Growth across many remaps must preserve every previously-written byte. */
static void
test_grow_preserves_bytes(void)
{
    const int dim = 8;
    const int64_t n = 5000;             /* forces several doublings from 64 */
    float *vecs; uint64_t *tids;
    make_corpus(&vecs, &tids, n, dim);

    CuvsBuildShm sh;
    ASSERT(cuvs_build_shm_create(&sh, (size_t) 64 * dim * sizeof(float)) == 0, "create");

    int64_t cap = 64;
    for (int64_t i = 0; i < n; i++)
    {
        if (i >= cap)
        {
            cap *= 2;
            ASSERT(cuvs_build_shm_resize(&sh, (size_t) cap * dim * sizeof(float)) == 0,
                   "resize");
        }
        memcpy((char *) sh.base + (size_t) i * dim * sizeof(float),
               &vecs[i * dim], (size_t) dim * sizeof(float));
    }

    int ok = 1;
    for (int64_t i = 0; i < n && ok; i++)
        ok = (memcmp((char *) sh.base + (size_t) i * dim * sizeof(float),
                     &vecs[i * dim], (size_t) dim * sizeof(float)) == 0);
    ASSERT(ok, "all bytes preserved across grow remaps");

    cuvs_build_shm_destroy(&sh);
    free(vecs); free(tids);
}

/* destroy() is idempotent and safe on an empty handle. */
static void
test_destroy_idempotent(void)
{
    CuvsBuildShm sh;
    ASSERT(cuvs_build_shm_create(&sh, 4096) == 0, "create");
    cuvs_build_shm_destroy(&sh);
    ASSERT(sh.fd == -1 && sh.base == NULL && sh.key[0] == '\0', "destroy clears handle");
    cuvs_build_shm_destroy(&sh);  /* must not crash / double-free */
    ASSERT(1, "double destroy survived");

    CuvsBuildShm empty;
    memset(&empty, 0, sizeof(empty));
    empty.fd = -1;
    cuvs_build_shm_destroy(&empty);  /* empty handle */
    ASSERT(1, "destroy of empty handle survived");
}

int
main(void)
{
    test_golden_no_grow(/*n=*/64,   /*dim=*/4);
    test_golden_no_grow(/*n=*/1000, /*dim=*/16);
    test_golden_no_grow(/*n=*/333,  /*dim=*/3);
    test_golden_with_grow(/*n=*/1000, /*dim=*/16, /*init_rows=*/64);
    test_golden_with_grow(/*n=*/333,  /*dim=*/3,  /*init_rows=*/333); /* exact fit */
    test_grow_preserves_bytes();
    test_destroy_idempotent();

    fprintf(stderr, "test_build_shm: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
