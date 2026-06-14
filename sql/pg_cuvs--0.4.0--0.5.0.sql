/* pg_cuvs 0.4.0 -> 0.5.0 — ADR-075 Phase 1: hardware-profile introspection.
 * Read-only; exposes the physical constants the daemon measures at boot. The
 * cost model does not consume these yet (Phase 2). */

CREATE FUNCTION pg_cuvs_hw_profile(
    OUT gpu_name                text,
    OUT n_gpus                  integer,
    OUT total_vram_bytes        bigint,
    OUT link_bw_bytes_per_us    double precision,
    OUT hbm_bw_bytes_per_us     double precision,
    OUT gpu_bf_tput             double precision,
    OUT ipc_rtt_us              double precision,
    OUT measured_at_epoch       bigint,
    OUT probe_status            integer,
    OUT source                  text,
    OUT matches_running_daemon  boolean,
    OUT cpu_dist_tput           double precision,
    OUT gpu_cagra_lat_us        double precision
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_hw_profile'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_hw_profile() IS
  'Measured (or DEFAULT) hardware profile written by the pg_cuvs daemon at boot '
  '(ADR-075 Phase 1). source = measured|default; matches_running_daemon flags a '
  'stale profile vs the running daemon (GPU swap / migration). Bandwidths are '
  'bytes per microsecond; gpu_bf_tput is (vectors*dim) per microsecond. Read-only; '
  'not yet consumed by the cost model.';
