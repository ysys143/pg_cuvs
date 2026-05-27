#pragma once

/*
 * cuvs_objstore.h — GCS object storage client for pg_cuvs artifact snapshots.
 *
 * Phase 3C: after a successful CAGRA build, upload .cagra + .tids + manifest
 * to GCS so that a new VM/replica with a compatible PostgreSQL heap can skip the
 * full heap-scan + GPU rebuild and hydrate from GCS instead.
 *
 * Core invariant (ADR-013, OBJSTORE-03, OBJSTORE-04):
 *   GCS holds DERIVED index artifact caches, not heap data. A node must already
 *   have a heap-compatible PostgreSQL instance before loading an artifact.
 *   Heap compatibility is verified via the relfilenode stored in the manifest:
 *   any mismatch is a hard reject — the artifact is skipped and REINDEX is
 *   required.
 *
 * Authentication:
 *   1. Try GCP instance metadata server (standard GCP VM service account).
 *   2. If that fails, parse the service account JSON at cuvs.gcs_key_file and
 *      sign a JWT to exchange for an access token.
 *
 * Daemon only (NOT linked into the PostgreSQL .so extension).
 * Requires: -lcurl -lssl -lcrypto
 */

#include <stdint.h>

/* ----------------------------------------------------------------
 * Manifest struct
 *
 * Captures every field of the on-disk manifest.json needed for heap
 * compatibility verification and artifact integrity checking.
 * ---------------------------------------------------------------- */
typedef struct CuvsManifest {
    char     pg_cuvs_version[32];
    char     metric_name[8];       /* "l2", "cosine", "ip" */
    uint32_t database_oid;
    uint32_t table_oid;
    uint32_t index_oid;
    uint32_t relfilenode;          /* heap table relfilenode: HEAP COMPAT IDENTITY */
    uint32_t base_generation;      /* .tids body_crc32 at build time */
    uint32_t dim;
    int64_t  vector_count;
    int64_t  build_timestamp;      /* time(NULL) at upload — also used as GCS version path */
    char     cagra_sha256[65];
    int64_t  cagra_size_bytes;
    char     tids_sha256[65];
    int64_t  tids_size_bytes;
} CuvsManifest;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * cuvs_objstore_upload — upload .cagra + .tids + manifest to GCS.
 *
 * Called (in a detached pthread) from handle_build() after disk commit.
 * Upload failure is non-fatal: the local artifact is already durable.
 * Logs WARN on failure; logs INFO on success.
 *
 * GCS paths written:
 *   gs://<bucket>/<prefix>/pg_cuvs/<cluster>/<db>/<idx>/<ts>/index.cagra
 *   gs://<bucket>/<prefix>/pg_cuvs/<cluster>/<db>/<idx>/<ts>/index.tids
 *   gs://<bucket>/<prefix>/pg_cuvs/<cluster>/<db>/<idx>/<ts>/manifest.json
 *   gs://<bucket>/<prefix>/pg_cuvs/<cluster>/<db>/<idx>/latest/manifest.json (alias)
 *
 * Returns 0 on success, -1 on any failure.
 */
int cuvs_objstore_upload(
    const char *snapshot_uri,    /* "gs://bucket[/prefix]" */
    const char *cluster_id,
    const char *gcs_key_file,    /* "" = instance metadata only */
    const char *cagra_path,      /* local .cagra file */
    const char *tids_path,       /* local .tids file */
    uint32_t    db_oid,
    uint32_t    table_oid,
    uint32_t    index_oid,
    uint32_t    relfilenode,
    uint32_t    metric,          /* CUVS_METRIC_* */
    uint32_t    dim,
    int64_t     vector_count,
    uint32_t    base_generation  /* .tids body_crc32 */
);

/*
 * cuvs_objstore_download — fetch .cagra + .tids from GCS into index_dir.
 *
 * Called from startup_load_indexes() when a local .cagra is missing.
 * Verifies heap compatibility (relfilenode) before writing any local files.
 * Verifies SHA256 of each downloaded file before accepting.
 *
 * local_relfilenode: value from the local .relfilenode sidecar (written by
 *   the extension at build time). If 0, the compat check is skipped (the
 *   artifact is still downloaded but without the heap identity guarantee).
 *
 * Returns 0 on success (files written to index_dir), -1 on any failure
 * (logs reason, leaves no partial files).
 */
int cuvs_objstore_download(
    const char *snapshot_uri,
    const char *cluster_id,
    const char *gcs_key_file,
    const char *index_dir,
    uint32_t    db_oid,
    uint32_t    index_oid,
    uint32_t    local_relfilenode,
    uint64_t   *build_timestamp_out  /* may be NULL */
);

/*
 * cuvs_sha256_file — compute SHA256 of a local file, hex-encoded.
 *
 * hex_out must be at least 65 bytes. Returns 0 on success, -1 on error.
 */
int cuvs_sha256_file(const char *path, char hex_out[65]);
