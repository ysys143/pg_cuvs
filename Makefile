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
NVCC_FLAGS    ?= -O3 --compiler-options '-fPIC' -arch=$(CUDA_ARCH) -std=c++17

# cuVS / RAPIDS install root (conda env activates default)
CUVS_PREFIX   ?= $(CONDA_PREFIX)
CUVS_INCLUDE  ?= $(CUVS_PREFIX)/include
CUVS_LIB      ?= $(CUVS_PREFIX)/lib

PG_CPPFLAGS    = -I$(CUVS_INCLUDE) -I./src
SHLIB_LINK     = -L$(CUVS_LIB) -lcuvs -lcudart -lstdc++

PG_CONFIG     ?= pg_config
PGXS         := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Build CUDA object before linking the .so. Pattern rule overrides PGXS
# default for this specific file since .cu needs nvcc, not gcc.
src/cuvs_wrapper.o: src/cuvs_wrapper.cu src/cuvs_wrapper.h
	$(NVCC) $(NVCC_FLAGS) -I$(CUVS_INCLUDE) -I./src -c $< -o $@

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

gpu-bench:
	@mkdir -p design
	ssh $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make benchmark 2>&1" | tee design/bench_$(shell date +%Y%m%d_%H%M).log

gpu-shell:
	ssh -tt $(GCP_VM)

# Convenience: full cycle on the VM (sync → build → install → test).
gpu-cycle: sync gpu-build gpu-install gpu-test
