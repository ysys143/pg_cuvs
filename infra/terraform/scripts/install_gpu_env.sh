#!/bin/bash
# pg_cuvs GPU dev VM startup script
# Ubuntu 22.04 LTS + NVIDIA L4 (sm_89)
# Installs: NVIDIA driver + CUDA 12 + Miniforge + cuvs_dev env + PG16 + pgvector
set -euo pipefail
exec > /var/log/pg_cuvs_setup.log 2>&1

echo "=== pg_cuvs GPU env setup start ==="
date

# ---- 1. System packages ------------------------------------------------
apt-get update -q
apt-get install -y \
    build-essential gcc-12 g++-12 \
    cmake \
    git curl wget rsync \
    pkg-config \
    libssl-dev \
    software-properties-common \
    gnupg2 \
    lsb-release

update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100

# ---- 2. NVIDIA driver + CUDA 12 ----------------------------------------
# Use CUDA network repo (driver 535+ required for L4)
wget -q https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
dpkg -i cuda-keyring_1.1-1_all.deb
apt-get update -q
apt-get install -y cuda-toolkit-12-4 cuda-drivers

# Persist PATH/LD_LIBRARY_PATH across SSH sessions
cat >> /etc/environment << 'EOF'
PATH="/usr/local/cuda/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
LD_LIBRARY_PATH="/usr/local/cuda/lib64"
EOF

echo 'export PATH=/usr/local/cuda/bin:$PATH' >> /etc/profile.d/cuda.sh
echo 'export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH' >> /etc/profile.d/cuda.sh

# ---- 3. Miniforge + cuvs_dev conda environment -------------------------
# Install as ubuntu user so Makefile ssh commands can activate without sudo
su - ubuntu << 'CONDA_SETUP'
set -euo pipefail

# Miniforge (conda-forge default, lighter than Anaconda)
wget -q https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh \
    -O /tmp/miniforge.sh
bash /tmp/miniforge.sh -b -p ~/miniforge3
rm /tmp/miniforge.sh

~/miniforge3/bin/conda init bash
source ~/miniforge3/bin/activate

# cuVS dev environment via rapidsai channel
# libcuvs is the unified C/C++ package (headers bundled with libs in 25.x+)
~/miniforge3/bin/conda create -n cuvs_dev -y \
    -c rapidsai -c conda-forge -c nvidia \
    libcuvs=26.04 \
    cuda-toolkit=12.* \
    python=3.11

echo "cuvs_dev env created"
CONDA_SETUP

# ---- 4. PostgreSQL 16 from PGDG ----------------------------------------
# PGDG repo for PG16 on Ubuntu 22.04
sh -c 'echo "deb https://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" \
    > /etc/apt/sources.list.d/pgdg.list'
wget -q -O - https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -
apt-get update -q
apt-get install -y postgresql-16 postgresql-server-dev-16

systemctl enable postgresql
systemctl start postgresql

# Allow local connections without password for dev
PG_HBA=$(find /etc/postgresql/16 -name "pg_hba.conf")
sed -i 's/^local\s\+all\s\+all\s\+peer/local   all   all   trust/' "$PG_HBA"
systemctl reload postgresql

# ---- 5. pgvector --------------------------------------------------------
# pg_cuvs depends on pgvector for vector type + HNSW fallback
cd /tmp
git clone --branch v0.8.0 https://github.com/pgvector/pgvector.git
cd pgvector
make PG_CONFIG=/usr/bin/pg_config
make install PG_CONFIG=/usr/bin/pg_config
cd /
rm -rf /tmp/pgvector

# ---- 6. pg_cuvs workspace for ubuntu user ------------------------------
su - ubuntu -c "mkdir -p ~/pg_cuvs"

echo "=== pg_cuvs GPU env setup complete ==="
date
