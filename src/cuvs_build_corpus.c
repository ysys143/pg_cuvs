/*
 * cuvs_build_corpus.c — leak-safe build-payload handoff. See cuvs_build_corpus.h
 * for the contract. PostgreSQL-free by design.
 */
#define _GNU_SOURCE   /* memfd_create, MFD_CLOEXEC */

#include "cuvs_build_corpus.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>     /* flock */
#include <sys/socket.h>

#define BLD_PREFIX     "pg_cuvs_bld_"
#define BLD_PREFIX_LEN (sizeof(BLD_PREFIX) - 1)
#define REAP_MIN_AGE_S 10            /* skip segments younger than this (create-window race) */

/* Process-wide tier state. */
static int g_memfd_probed = 0;       /* 1 once we know memfd availability */
static int g_memfd_ok     = 0;       /* memfd_create usable */
static CorpusKind g_forced = CORPUS_NONE;  /* test override; NONE = auto */

void
cuvs_corpus_force_kind(const char *kind)
{
    if (kind == NULL)              g_forced = CORPUS_NONE;
    else if (!strcmp(kind, "memfd")) g_forced = CORPUS_MEMFD;
    else if (!strcmp(kind, "shm"))   g_forced = CORPUS_SHM;
    else if (!strcmp(kind, "heap"))  g_forced = CORPUS_HEAP;
    else                             g_forced = CORPUS_NONE;
}

/* Unique per-process T2 name (mirrors cuvs_ipc.c:make_shm_key, distinct prefix). */
static void
build_shm_name(char *name, size_t namelen)
{
    static int seq = 0;
    snprintf(name, namelen, "/" BLD_PREFIX "%d_%d", (int) getpid(), seq++);
}

/* ---- tier: memfd (T1) -------------------------------------------------- */
static int
open_memfd(CuvsBuildCorpus *c, size_t bytes)
{
#ifdef __linux__
    int fd = memfd_create("pg_cuvs_build", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t) bytes) < 0)
    {
        int e = errno; close(fd); errno = e; return -1;
    }
    void *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        int e = errno; close(fd); errno = e; return -1;
    }
    c->kind = CORPUS_MEMFD;
    c->fd = fd;
    c->base = mem;
    c->map_bytes = bytes;
    return 0;
#else
    (void) c; (void) bytes;
    errno = ENOSYS;
    return -1;
#endif
}

/* ---- tier: named shm (T2) ---------------------------------------------- */
static int
open_shm(CuvsBuildCorpus *c, size_t bytes)
{
    char  name[64];
    int   fd;
    void *mem;

    build_shm_name(name, sizeof(name));

    fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return -1;
    fchmod(fd, 0666);                 /* daemon (other uid) must be able to open */
    /* Hold an exclusive advisory lock for the segment's life. The kernel
     * auto-releases it on ANY process death (incl. SIGKILL), which is how the
     * reaper distinguishes a dead owner from a live build. Best-effort: on
     * platforms that reject flock() on a shm fd (e.g. macOS) the build segment
     * is still valid; the reaper only runs where /dev/shm exists (Linux), and
     * there flock() on tmpfs succeeds. */
    (void) flock(fd, LOCK_EX);
    if (ftruncate(fd, (off_t) bytes) < 0)
    {
        int e = errno; shm_unlink(name); close(fd); errno = e; return -1;
    }
    mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        int e = errno; shm_unlink(name); close(fd); errno = e; return -1;
    }
    c->kind = CORPUS_SHM;
    c->fd = fd;
    c->base = mem;
    c->map_bytes = bytes;
    strncpy(c->shm_name, name, sizeof(c->shm_name) - 1);
    c->shm_name[sizeof(c->shm_name) - 1] = '\0';
    return 0;
}

int
cuvs_corpus_open(CuvsBuildCorpus *c, size_t init_bytes)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->kind = CORPUS_NONE;

    /* Forced tier (tests). */
    if (g_forced == CORPUS_MEMFD)
        return open_memfd(c, init_bytes) == 0 ? 0 : -1;
    if (g_forced == CORPUS_SHM)
        return open_shm(c, init_bytes) == 0 ? 0 : -1;
    if (g_forced == CORPUS_HEAP)
    {
        c->kind = CORPUS_HEAP;
        return 0;
    }

    /* Auto: memfd -> shm -> heap. */
    if (!g_memfd_probed || g_memfd_ok)
    {
        if (open_memfd(c, init_bytes) == 0)
        {
            g_memfd_probed = 1;
            g_memfd_ok = 1;
            return 0;
        }
        /* ENOSYS (old kernel) / EPERM (seccomp) => never try memfd again. */
        if (errno == ENOSYS || errno == EPERM)
        {
            g_memfd_probed = 1;
            g_memfd_ok = 0;
        }
        /* Other memfd errors: fall through to shm for this build, keep probing. */
    }

    if (open_shm(c, init_bytes) == 0)
        return 0;

    /* Last resort: heap (caller manages the buffer; handoff copies to shm). */
    c->kind = CORPUS_HEAP;
    c->fd = -1;
    c->base = NULL;
    c->map_bytes = 0;
    return 0;
}

int
cuvs_corpus_resize(CuvsBuildCorpus *c, size_t new_bytes)
{
    void *mem;

    if (c->kind == CORPUS_HEAP || c->kind == CORPUS_NONE)
        return 0;                    /* caller-managed / nothing to do */
    if (c->fd < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (new_bytes == c->map_bytes)
        return 0;

    /* Unmap before ftruncate: on macOS ftruncate rejects an active mapping;
     * unmapping first is portable. Written bytes survive in the fd's object. */
    if (c->base != NULL)
    {
        munmap(c->base, c->map_bytes);
        c->base = NULL;
        c->map_bytes = 0;
    }
    if (ftruncate(c->fd, (off_t) new_bytes) < 0)
        return -1;                   /* errno set; segment exists; close() still cleans */

    mem = mmap(NULL, new_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
    if (mem == MAP_FAILED)
        return -1;                   /* errno set; close() still closes/unlinks */

    c->base = mem;
    c->map_bytes = new_bytes;
    return 0;
}

void
cuvs_corpus_close(CuvsBuildCorpus *c)
{
    if (c->base != NULL)
    {
        munmap(c->base, c->map_bytes);
        c->base = NULL;
    }
    if (c->kind == CORPUS_SHM && c->shm_name[0] != '\0')
    {
        shm_unlink(c->shm_name);
        c->shm_name[0] = '\0';
    }
    if (c->fd >= 0)
    {
        close(c->fd);
        c->fd = -1;
    }
    c->map_bytes = 0;
    c->kind = CORPUS_NONE;
}

void
cuvs_corpus_detach(CuvsBuildCorpus *c, char *name_out, size_t name_len)
{
    if (name_out != NULL && name_len > 0)
        name_out[0] = '\0';

    /* Hand off the name only for a named (T2) segment. */
    if (c->kind == CORPUS_SHM && c->shm_name[0] != '\0'
        && name_out != NULL && name_len > 0)
    {
        strncpy(name_out, c->shm_name, name_len - 1);
        name_out[name_len - 1] = '\0';
        /* drop our mapping/fd but keep the segment linked for the opener */
        if (c->base != NULL)
        {
            munmap(c->base, c->map_bytes);
            c->base = NULL;
        }
        if (c->fd >= 0)
        {
            close(c->fd);
            c->fd = -1;
        }
        c->shm_name[0] = '\0';   /* not ours to unlink anymore */
        c->map_bytes = 0;
        c->kind = CORPUS_NONE;
        return;
    }

    /* memfd/heap (or no name requested): nothing to hand off — close normally. */
    cuvs_corpus_close(c);
}

/* ---- T2 reaper --------------------------------------------------------- */
int
cuvs_corpus_reap_orphans(int do_unlink)
{
    DIR           *d;
    time_t         now;
    int            found = 0;
    long           min_age = REAP_MIN_AGE_S;
    const char    *age_env = getenv("CUVS_REAP_MIN_AGE");  /* test seam */
    struct dirent *ent;

    if (age_env != NULL)
        min_age = strtol(age_env, NULL, 10);

    d = opendir("/dev/shm");
    if (d == NULL)
        return (errno == ENOENT) ? 0 : -1;   /* no /dev/shm (e.g. macOS) => nothing */

    now = time(NULL);
    while ((ent = readdir(d)) != NULL)
    {
        char        path[300];
        char        shmname[300];
        struct stat st;
        int         fd;

        if (strncmp(ent->d_name, BLD_PREFIX, BLD_PREFIX_LEN) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/shm/%s", ent->d_name);
        if (stat(path, &st) == 0 && (now - st.st_mtime) < min_age)
            continue;                /* too young: a live build may not have flock'd yet */

        snprintf(shmname, sizeof(shmname), "/%s", ent->d_name);
        fd = shm_open(shmname, O_RDWR, 0);
        if (fd < 0)
            continue;                /* vanished (raced) or unreadable */

        /* If we can take the lock, no live process holds it => owner is dead. */
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            found++;
            if (do_unlink)
                shm_unlink(shmname);
            /* close releases our lock; if we did not unlink, the next reaper
             * will see it lock-free again and can finish the job. */
        }
        close(fd);
    }
    closedir(d);
    return found;
}

/* ---- SCM_RIGHTS fd passing --------------------------------------------- */
int
cuvs_fd_send(int sock, int fd, const void *payload, size_t len)
{
    struct msghdr msg;
    struct iovec  iov;
    char          cbuf[CMSG_SPACE(sizeof(int))];
    ssize_t       n;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *) payload;
    iov.iov_len  = len;
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    if (fd >= 0)
    {
        struct cmsghdr *cm;
        memset(cbuf, 0, sizeof(cbuf));
        msg.msg_control    = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type  = SCM_RIGHTS;
        cm->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }

    /* The ancillary fd must ride with the first byte and cannot be re-sent, so
     * we require the (small, fixed) payload to go in one sendmsg. */
    do { n = sendmsg(sock, &msg, 0); } while (n < 0 && errno == EINTR);
    return (n == (ssize_t) len) ? 0 : -1;
}

int
cuvs_fd_recv(int sock, void *payload, size_t len, int *out_fd)
{
    struct msghdr   msg;
    struct iovec    iov;
    char            cbuf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cm;
    ssize_t         n;
    size_t          got;

    *out_fd = -1;
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = payload;
    iov.iov_len  = len;
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    do { n = recvmsg(sock, &msg, 0); } while (n < 0 && errno == EINTR);
    if (n <= 0)
        return -1;
    if (msg.msg_flags & MSG_CTRUNC)
        return -1;                   /* ancillary truncated — refuse */

    for (cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm))
    {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
            memcpy(out_fd, CMSG_DATA(cm), sizeof(int));
    }

    /* The fd arrives with the first byte; read any remaining payload plainly. */
    got = (size_t) n;
    while (got < len)
    {
        ssize_t r = read(sock, (char *) payload + got, len - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t) r;
    }
    return 0;
}
