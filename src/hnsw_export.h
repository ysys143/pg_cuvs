#pragma once

/*
 * hnsw_export.h — GPU HNSW index creation for pg_cuvs.
 *
 * Public API: pg_cuvs_build_hnsw(cagra_oid, mode)
 * Internal helpers: fill_hnsw_from_hnswlib / fill_hnsw_from_cagra_ipc
 *   (static, defined in hnsw_export.c, not callable from SQL)
 */

#include "postgres.h"
#include "fmgr.h"

/*
 * pg_cuvs_build_hnsw(cagra_oid regclass, mode text DEFAULT 'nsw')
 *
 * GPU-accelerated HNSW creation without pgvector CPU build (285s eliminated).
 * Creates the HNSW index internally via INDEX_CREATE_SKIP_BUILD.
 *
 * Recommended modes:
 *   'nsw'     — flat NSW, 117s, 2.4x vs native. Default.
 *   'hnswlib' — from_cagra() hierarchy via /dev/shm, 139s, 2.0x.
 *
 * Hidden modes (research only):
 *   'hnsw', 'hnswlib_file'
 *
 * Returns OID of newly created HNSW index (regclass).
 */
Datum pg_cuvs_build_hnsw(PG_FUNCTION_ARGS);
