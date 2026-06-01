#pragma once

/*
 * hnsw_export.h — Phase 3I-2: read .hnsw sidecar (hnswlib binary) and
 * bulk-write a pgvector-compatible HNSW index into an existing pgvector
 * HNSW relation.
 */

#include "postgres.h"
#include "fmgr.h"

/*
 * pg_cuvs_import_hnsw(cagra_oid regclass, hnsw_oid regclass)
 *
 * Reads the .hnsw sidecar written by Phase 3I-1 alongside the CAGRA index
 * identified by cagra_oid, then bulk-writes all element and neighbor pages
 * into the existing pgvector HNSW index identified by hnsw_oid.
 *
 * The target HNSW index is truncated to 0 pages first and fully rebuilt from
 * the sidecar data — do not call this on a live production index.
 */
Datum pg_cuvs_import_hnsw(PG_FUNCTION_ARGS);
