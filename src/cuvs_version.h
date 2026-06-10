#pragma once

/*
 * cuvs_version.h — single source of truth for the version stamps embedded in
 * the GCS artifact manifest.json (Phase 3C, ADR-013).
 *
 * PG_CUVS_VERSION   — keep in sync with pg_cuvs.control `default_version`.
 * CUVS_BUILD_VERSION — the cuVS release the daemon was built/linked against.
 *   A downloaded artifact serialized by a different cuVS is rejected at load:
 *   CAGRA serialization is cuVS-version-coupled, so cross-version deserialize is
 *   not guaranteed safe. Bump this on a cuVS upgrade. A runtime accessor that
 *   reads the linked cuVS version is a tracked follow-up; until then this
 *   build-time constant records the artifact's cuVS lineage.
 *
 * Both are `#ifndef`-guarded so the Makefile may override via -D if desired.
 */

#ifndef PG_CUVS_VERSION
#define PG_CUVS_VERSION "0.3.0"
#endif

#ifndef CUVS_BUILD_VERSION
#define CUVS_BUILD_VERSION "26.04.00"
#endif
