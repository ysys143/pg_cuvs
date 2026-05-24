#!/bin/bash
# postinstall.sh — idempotent post-install setup on the GPU VM.
#
# Runs once after `make gpu-install`. Sets up:
#   - traversal permissions so postmaster can dlopen libs under /home/ubuntu
#   - libstdc++ + libgcc_s system-wide symlinks (cuVS needs GCC 14 runtime,
#     system has GCC 11; target only these two .so, NOT the conda lib dir)
#   - PG role + db for the ubuntu user
#   - shared_preload_libraries = 'pg_cuvs' so backends inherit the 812MB
#     libcuvs.so mapping via fork (95ms -> <1ms first-query planning)
#
# Safe to run multiple times.

set -e

CONDA_ENV="${CONDA_ENV:-cuvs_dev}"
CONDA_LIB="/home/ubuntu/miniforge3/envs/${CONDA_ENV}/lib"

echo "[postinstall] chmod traversal so postgres user can reach ${CONDA_LIB}"
sudo chmod o+x /home/ubuntu
sudo chmod o+x /home/ubuntu/miniforge3
sudo chmod o+x /home/ubuntu/miniforge3/envs
sudo chmod o+x "/home/ubuntu/miniforge3/envs/${CONDA_ENV}"
sudo chmod o+x "${CONDA_LIB}"

echo "[postinstall] symlink libstdc++ + libgcc_s into /usr/local/lib"
# Target only these two libs. NEVER add the conda env's full lib dir to
# /etc/ld.so.conf.d/ — that pulls in conda's libssl/libdbus and breaks sshd.
sudo ln -sf "${CONDA_LIB}/libstdc++.so.6" /usr/local/lib/libstdc++.so.6
sudo ln -sf "${CONDA_LIB}/libgcc_s.so.1" /usr/local/lib/libgcc_s.so.1
sudo ldconfig

echo "[postinstall] PG role + db for ubuntu user"
sudo -u postgres createuser -s ubuntu 2>/dev/null || true
sudo -u postgres createdb ubuntu 2>/dev/null || true

PG_CONF=$(sudo -u postgres find /etc/postgresql/16 -name postgresql.conf)
echo "[postinstall] shared_preload_libraries setup in $PG_CONF"
if ! sudo grep -qE "^shared_preload_libraries\s*=\s*.*pg_cuvs" "$PG_CONF"; then
    sudo sed -i "/^#\\?shared_preload_libraries/d" "$PG_CONF"
    echo "shared_preload_libraries = 'pg_cuvs'" | sudo tee -a "$PG_CONF" >/dev/null
    echo "[postinstall] added shared_preload_libraries"
else
    echo "[postinstall] shared_preload_libraries already present"
fi

echo "[postinstall] restart PostgreSQL"
sudo systemctl restart postgresql

echo "[postinstall] verify"
psql -d postgres -c "SHOW shared_preload_libraries;"
echo "[postinstall] DONE"
