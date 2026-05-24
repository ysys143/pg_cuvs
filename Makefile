# pg_cuvs Makefile
#
# Two layers:
#  1. PGXS targets (build/install) — run on the machine that has the
#     PostgreSQL + CUDA toolchain (typically the GCP GPU VM).
#  2. GCP remote targets (vm-start, sync, gpu-build, gpu-test, ...) —
#     run on the local laptop, SSH into the GPU VM, stream output back.
#
# The local laptop typically does not have a CUDA toolchain, so PGXS
# build commands only succeed when invoked on the VM.

# ---- PGXS configuration -------------------------------------------------
EXTENSION      = pg_cuvs
EXTVERSION     = 0.1.0
DATA           = sql/pg_cuvs--$(EXTVERSION).sql
MODULE_big     = pg_cuvs
REGRESS        = smoke cpu_fallback
REGRESS_OPTS   = --inputdir=test --outputdir=test

# C source files + the CUDA-compiled wrapper (built below by nvcc).
# PGXS only knows how to build .c → .o; the .cu → .o rule is custom,
# but the resulting object MUST be listed in OBJS to be linked into the .so.
OBJS           = src/pg_cuvs.o src/cuvs_ipc.o src/cuvs_wrapper.o

# nvcc settings (Phase 1: brute-force only; CAGRA added later)
NVCC          ?= nvcc
CUDA_ARCH     ?= sm_80
NVCC_FLAGS    ?= -O3 --compiler-options '-fPIC' -arch=$(CUDA_ARCH) -std=c++17 \
                 -DRAFT_SYSTEM_LITTLE_ENDIAN=1

# cuVS / RAPIDS install root (conda env activates default)
CUVS_PREFIX   ?= $(CONDA_PREFIX)
CUVS_INCLUDE  ?= $(CUVS_PREFIX)/include
# cuVS 25.x+ ships bundled libcudacxx under include/rapids/
CUVS_RAPIDS_INCLUDE ?= $(CUVS_PREFIX)/include/rapids
CUVS_LIB      ?= $(CUVS_PREFIX)/lib

PG_CPPFLAGS    = -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src
# -Wl,-rpath embeds the cuVS lib path so postmaster finds libcuvs.so
# without LD_LIBRARY_PATH being set (ADR-007).
SHLIB_LINK     = -L$(CUVS_LIB) -lcuvs -lcudart \
                 -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic \
                 -Wl,-rpath,$(CUVS_LIB) -lrt

PG_CONFIG     ?= pg_config
PGXS         := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Build CUDA object before linking the .so. Pattern rule overrides PGXS
# default for this specific file since .cu needs nvcc, not gcc.
src/cuvs_wrapper.o: src/cuvs_wrapper.cu src/cuvs_wrapper.h
	$(NVCC) $(NVCC_FLAGS) -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -I./src -c $< -o $@

# PGXS generates LLVM bitcode (.bc) from .c sources for JIT. The .cu file
# has no PG-callable functions, so emit a stub bitcode so PGXS doesn't fail.
src/cuvs_wrapper.bc: src/cuvs_wrapper.cu
	@echo 'void cuvs_wrapper_jit_stub(void){}' > /tmp/_cuvs_stub.c
	@clang-15 -emit-llvm -c /tmp/_cuvs_stub.c -o $@ 2>/dev/null \
		|| clang -emit-llvm -c /tmp/_cuvs_stub.c -o $@
	@rm /tmp/_cuvs_stub.c

# ---- pg_cuvs_server binary -----------------------------------------------
# Standalone GPU daemon — NOT a PostgreSQL extension, no PGXS involvement.
# Links against libcuvs + libcudart + libpthread + librt.
SERVER_SRCS    = src/pg_cuvs_server.c src/cuvs_ipc.c
SERVER_OBJS    = src/pg_cuvs_server.o src/cuvs_ipc.o
SERVER_BIN     = pg_cuvs_server

CC             ?= gcc
SERVER_CFLAGS  = -O2 -g -Wall -Wextra -I./src \
                 -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -std=gnu11 \
                 -D_POSIX_C_SOURCE=200809L
SERVER_LDFLAGS = -L$(CUVS_LIB) -lcuvs -lrmm -lcudart -lstdc++ \
                 -Wl,-rpath,$(CUVS_LIB) \
                 -lpthread -lrt

# server .c → .o (not via PGXS — separate rule with no PG headers)
src/pg_cuvs_server.o: src/pg_cuvs_server.c src/cuvs_ipc.h src/cuvs_wrapper.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_ipc.o for server (same source, no PG headers needed)
# Note: PGXS also builds cuvs_ipc.o for the .so; use a separate target.
src/cuvs_ipc_server.o: src/cuvs_ipc.c src/cuvs_ipc.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

$(SERVER_BIN): src/pg_cuvs_server.o src/cuvs_ipc_server.o src/cuvs_wrapper.o
	$(CXX) -o $@ $^ $(SERVER_LDFLAGS)

server: $(SERVER_BIN)

install-server: server
	install -m 755 $(SERVER_BIN) $(shell $(PG_CONFIG) --bindir)/

.PHONY: server install-server

# ---- GCP remote orchestration ------------------------------------------
# Load .env.gpu (gitignored) for GCP_VM, GCP_INSTANCE, GCP_ZONE, etc.
-include .env.gpu
export

.PHONY: vm-start vm-stop sync gpu-build gpu-test gpu-bench gpu-shell

vm-start:
	@test -n "$(GCP_INSTANCE)" || (echo "ERROR: set GCP_INSTANCE in .env.gpu"; exit 1)
	gcloud compute instances start $(GCP_INSTANCE) --zone $(GCP_ZONE)
	@echo "Waiting for SSH..."
	@until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no $(GCP_VM) true 2>/dev/null; \
		do sleep 3; done
	@echo "VM ready: $(GCP_VM)"

vm-stop:
	gcloud compute instances stop $(GCP_INSTANCE) --zone $(GCP_ZONE)

# rsync local → VM. Excludes build artifacts to avoid clobbering remote .o files
# that were produced on the VM with the correct nvcc toolchain.
sync:
	rsync -avz --delete \
		--exclude '.git' \
		--exclude 'src/*.o' \
		--exclude 'src/*.bc' \
		--exclude '*.so' \
		--exclude '.env.gpu' \
		./ $(GCP_VM):~/pg_cuvs/

gpu-build:
	ssh -tt $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make 2>&1 | tee /tmp/pg_cuvs_build.log"

gpu-install:
	ssh -tt $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		sudo -E make install"

gpu-test:
	ssh -tt $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make installcheck"

gpu-server:
	ssh -tt $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make server && sudo make install-server"

# Idempotent post-install: home traversal perms, libstdc++ symlinks,
# PG role, and shared_preload_libraries setup. Run once after gpu-install.
gpu-postinstall:
	ssh -tt $(GCP_VM) '\
		set -e; \
		echo "--- chmod traversal (postgres -> conda env) ---"; \
		sudo chmod o+x /home/ubuntu /home/ubuntu/miniforge3 \
			/home/ubuntu/miniforge3/envs /home/ubuntu/miniforge3/envs/$(CONDA_ENV) \
			/home/ubuntu/miniforge3/envs/$(CONDA_ENV)/lib; \
		echo "--- libstdc++ + libgcc_s symlinks (system-wide) ---"; \
		sudo ln -sf /home/ubuntu/miniforge3/envs/$(CONDA_ENV)/lib/libstdc++.so.6 \
			/usr/local/lib/libstdc++.so.6; \
		sudo ln -sf /home/ubuntu/miniforge3/envs/$(CONDA_ENV)/lib/libgcc_s.so.1 \
			/usr/local/lib/libgcc_s.so.1; \
		sudo ldconfig; \
		echo "--- PG role + db for ubuntu user ---"; \
		sudo -u postgres createuser -s ubuntu 2>/dev/null || true; \
		sudo -u postgres createdb ubuntu 2>/dev/null || true; \
		echo "--- shared_preload_libraries = pg_cuvs ---"; \
		PG_CONF=$$(sudo -u postgres find /etc/postgresql/16 -name postgresql.conf); \
		if ! sudo grep -qE "^shared_preload_libraries\s*=\s*.*pg_cuvs" $$PG_CONF; then \
			sudo sed -i "/^#\?shared_preload_libraries/d" $$PG_CONF; \
			echo "shared_preload_libraries = '"'"'pg_cuvs'"'"'" | sudo tee -a $$PG_CONF >/dev/null; \
			echo "Added shared_preload_libraries to $$PG_CONF"; \
		else \
			echo "shared_preload_libraries already set"; \
		fi; \
		echo "--- restart PostgreSQL ---"; \
		sudo systemctl restart postgresql; \
		echo "--- verify ---"; \
		psql -d postgres -c "SHOW shared_preload_libraries;" || true; \
		echo "DONE"'

gpu-server-start:
	ssh -tt $(GCP_VM) "pg_cuvs_server \
		--socket /tmp/.s.pg_cuvs \
		--index-dir \$$(psql -t -c 'SHOW data_directory' | tr -d ' ')/cuvs_indexes \
		--max-vram-mb 20480 &"

gpu-bench:
	@mkdir -p design
	ssh $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make benchmark 2>&1" | tee design/bench_$(shell date +%Y%m%d_%H%M).log

gpu-shell:
	ssh -tt $(GCP_VM)

# Convenience: full cycle on the VM (sync → build → install → test).
gpu-cycle: sync gpu-build gpu-install gpu-test
