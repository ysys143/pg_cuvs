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

/*
 * pg_cuvs_import_cagra(cagra_oid regclass, hnsw_oid regclass)
 *
 * Phase 3J: direct CAGRA→pgvector HNSW conversion without hnswlib intermediate.
 * Retrieves the CAGRA adjacency list from the daemon via IPC and writes a flat
 * pgvector HNSW (all nodes at level 0) directly.  Does NOT require
 * cuvs.cpu_hnsw_fallback=on; no .hnsw sidecar file is needed.
 *
 * Trade-off: flat HNSW may require higher ef_search for equivalent recall vs
 * pg_cuvs_import_hnsw() which produces a multi-level graph.
 */
/*
 * mode: 'nsw' = flat (level 0 only), 'hnsw' = hierarchical (heuristic selection)
 */
Datum pg_cuvs_import_cagra(PG_FUNCTION_ARGS);

/*
 * pg_cuvs_import(cagra_oid, mode) — unified GPU import.
 * Creates HNSW index on parent table WITHOUT calling pgvector CPU build.
 * Returns OID of new HNSW index.
 */
Datum pg_cuvs_import(PG_FUNCTION_ARGS);
