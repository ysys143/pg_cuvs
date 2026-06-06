/*
 * cuvs_build_shm.c — POSIX shm segment lifecycle for the CAGRA build payload.
 * See cuvs_build_shm.h for the contract. PostgreSQL-free by design.
 */
#include "cuvs_build_shm.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Unique per-process shm name. Mirrors cuvs_ipc.c:make_shm_key but uses a
 * distinct "bld" prefix so a build segment never collides with a search one. */
static void
build_shm_key(char *key, size_t keylen)
{
    static int seq = 0;
    snprintf(key, keylen, "/pg_cuvs_bld_%d_%d", (int) getpid(), seq++);
}

int
cuvs_build_shm_create(CuvsBuildShm *sh, size_t bytes)
{
    char  key[64];
    int   fd;
    void *mem;

    sh->fd = -1;
    sh->base = NULL;
    sh->key[0] = '\0';
    sh->map_bytes = 0;

    build_shm_key(key, sizeof(key));

    fd = shm_open(key, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return -1;

    /* Override umask so the daemon (different uid) can shm_open this segment. */
    fchmod(fd, 0666);

    if (ftruncate(fd, (off_t) bytes) < 0)
    {
        int e = errno;
        shm_unlink(key);
        close(fd);
        errno = e;
        return -1;
    }

    mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        int e = errno;
        shm_unlink(key);
        close(fd);
        errno = e;
        return -1;
    }

    sh->fd = fd;
    sh->base = mem;
    sh->map_bytes = bytes;
    strncpy(sh->key, key, sizeof(sh->key) - 1);
    sh->key[sizeof(sh->key) - 1] = '\0';
    return 0;
}

int
cuvs_build_shm_resize(CuvsBuildShm *sh, size_t new_bytes)
{
    void *mem;

    if (sh->fd < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (new_bytes == sh->map_bytes)
        return 0;

    /* Unmap before ftruncate: on macOS ftruncate returns EINVAL when an active
     * mapping exists on the shm fd. Unmapping first is portable to both macOS
     * and Linux. Written bytes survive in the shm object (not the mapping). */
    if (sh->base != NULL)
    {
        munmap(sh->base, sh->map_bytes);
        sh->base = NULL;
        sh->map_bytes = 0;
    }

    if (ftruncate(sh->fd, (off_t) new_bytes) < 0)
        return -1;  /* errno set; segment exists at old size; destroy() still cleans up */

    mem = mmap(NULL, new_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, sh->fd, 0);
    if (mem == MAP_FAILED)
        return -1;  /* errno set; no mapping; destroy() still closes+unlinks */

    sh->base = mem;
    sh->map_bytes = new_bytes;
    return 0;
}

void
cuvs_build_shm_destroy(CuvsBuildShm *sh)
{
    if (sh->base != NULL)
    {
        munmap(sh->base, sh->map_bytes);
        sh->base = NULL;
    }
    if (sh->key[0] != '\0')
    {
        shm_unlink(sh->key);
        sh->key[0] = '\0';
    }
    if (sh->fd >= 0)
    {
        close(sh->fd);
        sh->fd = -1;
    }
    sh->map_bytes = 0;
}
