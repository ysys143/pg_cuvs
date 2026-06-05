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
DATA           = sql/pg_cuvs--$(EXTVERSION).sql \
                 sql/pg_cuvs--0.1.0--0.2.0.sql
MODULE_big     = pg_cuvs
REGRESS        = smoke cpu_fallback edge_cases cpu_hnsw_fallback build_hnsw build_hnsw_edge pg_cuvs_hnsw metrics brute_force pg_cuvs_batch reloption_dir
REGRESS_OPTS   = --inputdir=test --outputdir=test

# C source files + the CUDA-compiled wrapper (built below by nvcc).
# PGXS only knows how to build .c → .o; the .cu → .o rule is custom,
# but the resulting object MUST be listed in OBJS to be linked into the .so.
OBJS           = src/pg_cuvs.o src/cuvs_ipc.o src/cuvs_util.o src/cuvs_wrapper.o src/hnsw_export.o

# nvcc settings (Phase 1: brute-force only; CAGRA added later)
NVCC          ?= nvcc
CUDA_ARCH     ?= sm_80
NVCC_FLAGS    ?= -O3 --compiler-options '-fPIC' -arch=$(CUDA_ARCH) -std=c++17 \
                 -DRAFT_SYSTEM_LITTLE_ENDIAN=1 \
                 -DCUVS_BUILD_CAGRA_HNSWLIB

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

# PGXS's implicit .c->.o rule does NOT track header dependencies, so a change to
# a shared header (e.g. a wire struct in cuvs_ipc.h) would otherwise leave stale
# extension objects linked against the old layout — an ABI mismatch vs the
# daemon. Force every extension object to rebuild when ANY project header
# changes. (The server objects already list their header prereqs explicitly.)
$(OBJS): $(wildcard src/*.h)

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
SERVER_SRCS    = src/pg_cuvs_server.c src/cuvs_ipc.c src/cuvs_util.c
SERVER_OBJS    = src/pg_cuvs_server.o src/cuvs_ipc.o src/cuvs_util.o
SERVER_BIN     = pg_cuvs_server

CC             ?= gcc
SERVER_CFLAGS  = -O2 -g -Wall -Wextra -I./src \
                 -I$(CUVS_INCLUDE) -I$(CUVS_RAPIDS_INCLUDE) -std=gnu11 \
                 -D_POSIX_C_SOURCE=200809L
SERVER_LDFLAGS = -L$(CUVS_LIB) -lcuvs -lrmm -lcudart -lstdc++ \
                 -Wl,-rpath,$(CUVS_LIB) \
                 -lpthread -lrt \
                 -lcurl -lssl -lcrypto

# server .c → .o (not via PGXS — separate rule with no PG headers)
src/pg_cuvs_server.o: src/pg_cuvs_server.c src/cuvs_ipc.h src/cuvs_util.h src/cuvs_wrapper.h src/cuvs_objstore.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_objstore.o for server (GCS client — not linked into the PG extension .so)
src/cuvs_objstore_server.o: src/cuvs_objstore.c src/cuvs_objstore.h src/cuvs_ipc.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_ipc.o for server (same source, no PG headers needed)
# Note: PGXS also builds cuvs_ipc.o for the .so; use a separate target.
src/cuvs_ipc_server.o: src/cuvs_ipc.c src/cuvs_ipc.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

# cuvs_util.o for server (same source, no PG headers needed)
# Note: PGXS also builds cuvs_util.o for the .so; use a separate target.
src/cuvs_util_server.o: src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) $(SERVER_CFLAGS) -c $< -o $@

$(SERVER_BIN): src/pg_cuvs_server.o src/cuvs_ipc_server.o src/cuvs_util_server.o src/cuvs_objstore_server.o src/cuvs_wrapper.o
	$(CXX) -o $@ $^ $(SERVER_LDFLAGS)

server: $(SERVER_BIN)

install-server: server
	install -m 755 $(SERVER_BIN) $(shell $(PG_CONFIG) --bindir)/

.PHONY: server install-server

# ---- pg_cuvs_server_test binary (fault injection) ------------------------
# Same sources as the production daemon, but compiled with -DCUVS_TEST_HOOKS
# so the cuvs_fault() env-var hooks in handle_build are live. NEVER install
# this over the production binary. Object files use a _test suffix so they do
# not clobber the production .o files.
SERVER_TEST_BIN     = pg_cuvs_server_test
SERVER_TEST_CFLAGS  = $(SERVER_CFLAGS) -DCUVS_TEST_HOOKS

src/pg_cuvs_server_test.o: src/pg_cuvs_server.c src/cuvs_ipc.h src/cuvs_util.h src/cuvs_wrapper.h src/cuvs_objstore.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_ipc_server_test.o: src/cuvs_ipc.c src/cuvs_ipc.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_util_server_test.o: src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

src/cuvs_objstore_server_test.o: src/cuvs_objstore.c src/cuvs_objstore.h src/cuvs_ipc.h
	$(CC) $(SERVER_TEST_CFLAGS) -c $< -o $@

$(SERVER_TEST_BIN): src/pg_cuvs_server_test.o src/cuvs_ipc_server_test.o src/cuvs_util_server_test.o src/cuvs_objstore_server_test.o src/cuvs_wrapper.o
	$(CXX) -o $@ $^ $(SERVER_LDFLAGS)

server-test: $(SERVER_TEST_BIN)

install-server-test: server-test
	install -m 755 $(SERVER_TEST_BIN) $(shell $(PG_CONFIG) --bindir)/

.PHONY: server-test install-server-test

# ---- Local unit tests ----------------------------------------------------
# No-framework unit tests for the dependency-free helpers in cuvs_util.c.
# Deliberately independent of PGXS/pg_config/CUDA so it runs on a laptop.
# librt exists on Linux (CI/VM) but not macOS — link it only where present.
TEST_RT_LIB := $(shell [ "$$(uname -s)" = "Linux" ] && echo -lrt)
test-unit: test/unit/test_cuvs_util.c src/cuvs_util.c src/cuvs_util.h src/cuvs_ipc.h
	$(CC) -I src -DCUVS_TEST_HOOKS -o test-unit test/unit/test_cuvs_util.c src/cuvs_util.c $(TEST_RT_LIB)
	./test-unit

.PHONY: test-unit

# ---- No-GPU regression target -------------------------------------------
# Runs the subset of tests that do NOT require a live GPU daemon.
# Currently this is just the C-level unit tests (test-unit).
#
# All pgregress tests (smoke, cpu_fallback, hnsw_import, …) require a running
# pg_cuvs_server daemon and therefore a GPU.  Use `make installcheck` on the
# GPU VM for the full suite.
#
# Intended for CI jobs on standard (CPU-only) runners — see .github/workflows/ci.yml.
installcheck-nogpu: test-unit
	@echo "[OK] No-GPU checks passed (test-unit)"

.PHONY: installcheck-nogpu

# ---- Benchmark harness (Phase 1.5 #5) ----------------------------------
# Parameterized large-dataset benchmark. Runs the bash harness; N/DIM/K/M
# are passed through ONLY when set on the make command line, so the script's
# own small sanity defaults apply otherwise. Invoked on the VM by `gpu-bench`
# (and `gpu-bench-1m` for the PLAN 1M/1536 completion gate).
benchmark:
	$(if $(N),N=$(N)) $(if $(DIM),DIM=$(DIM)) $(if $(K),K=$(K)) $(if $(M),M=$(M)) \
		bash infra/scripts/benchmark.sh

.PHONY: benchmark

# ---- GCP remote orchestration ------------------------------------------
# Load .env.gpu (gitignored) for GCP_VM, GCP_INSTANCE, GCP_ZONE, etc.
-include .env.gpu
export

.PHONY: vm-start vm-stop sync gpu-build gpu-test gpu-bench gpu-bench-1m gpu-shell \
	gpu-test-unit gpu-test-regress gpu-test-daemon gpu-test-e2e gpu-test-all

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
# The script is piped over stdin (bash -s) rather than inlined to avoid
# fragile nested quoting; plain ssh (no -tt) since it needs no remote TTY.
gpu-postinstall:
	CONDA_ENV=$(CONDA_ENV) ssh $(GCP_VM) "CONDA_ENV=$(CONDA_ENV) bash -s" \
		< infra/scripts/postinstall.sh

# End-to-end durability smoke: build index, restart daemon, verify reload.
# Piped over stdin (bash -s); plain ssh, no remote TTY needed.
gpu-e2e:
	ssh $(GCP_VM) "bash -s" < infra/scripts/e2e-smoke.sh

# ---- Integration test suite (Phase 1.5 #3) -----------------------------
# Layered test targets. Unit tests run locally (no toolchain needed);
# the rest run on the GPU VM where PG + CUDA + daemon are available.

# Unit tests for the dependency-free helpers (cuvs_util). Runs on the VM
# for parity with the build toolchain; identical to local `make test-unit`.
gpu-test-unit:
	ssh -tt $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make test-unit"

# PG regression suite (smoke + cpu_fallback). Requires a daemon up for the
# CREATE INDEX paths; the production pg-cuvs-server systemd unit must be
# active and cuvs.index_dir set to its --index-dir.
gpu-test-regress:
	ssh -tt $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make installcheck"

# Fault-injection daemon integration tests. Builds the CUVS_TEST_HOOKS
# daemon, drives daemon-down / persist-fault / clean-build scenarios on a
# TEST socket + index dir, then restores the production daemon. Piped over
# stdin (bash -s); CONDA_ENV is forwarded so the script can compile.
gpu-test-daemon:
	ssh $(GCP_VM) "source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		CONDA_ENV=$(CONDA_ENV) bash -s" \
		< infra/scripts/integration-test.sh

# End-to-end durability smoke (alias of gpu-e2e for naming symmetry).
gpu-test-e2e:
	ssh $(GCP_VM) "bash -s" < infra/scripts/e2e-smoke.sh

# Full ladder: unit -> regress -> daemon faults -> e2e durability.
gpu-test-all: gpu-test-unit gpu-test-regress gpu-test-daemon gpu-test-e2e

gpu-server-start:
	ssh -tt $(GCP_VM) "pg_cuvs_server \
		--socket /tmp/.s.pg_cuvs \
		--index-dir \$$(psql -t -c 'SHOW data_directory' | tr -d ' ')/cuvs_indexes \
		--max-vram-mb 20480 &"

# Default sanity benchmark on the VM (small size from benchmark.sh defaults).
# N/DIM/K/M forwarded to the remote `make benchmark` only when set locally.
gpu-bench:
	@mkdir -p design
	ssh $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		make benchmark $(if $(N),N=$(N)) $(if $(DIM),DIM=$(DIM)) $(if $(K),K=$(K)) $(if $(M),M=$(M)) 2>&1" \
		| tee design/bench_$(shell date +%Y%m%d_%H%M).log

# PLAN completion-gate run: 1M rows x 1536 dim (large VRAM-stress case).
# Explicit target so the heavy run is never the default. Reuses gpu-bench.
gpu-bench-1m:
	$(MAKE) gpu-bench N=1000000 DIM=1536

gpu-cohere:
	@echo "[gpu-cohere] Launching Cohere 1M benchmark (nohup, async)"
	ssh $(GCP_VM) "cd ~/pg_cuvs && \
		source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		nohup bash bench/run_cohere.sh \
			--n $(if $(N),$(N),1000000) \
			--gpu $(if $(GPU),$(GPU),0) \
			> /tmp/cohere_bench.log 2>&1 &" && \
	echo "[gpu-cohere] Started. Poll with:" && \
	echo "  make gpu-cohere-log" && \
	echo "  make gpu-cohere-result"

gpu-cohere-log:
	ssh $(GCP_VM) "tail -50 /tmp/cohere_bench.log"

gpu-cohere-result:
	@N=$(if $(N),$(N),1000000); \
	ssh $(GCP_VM) "cat ~/pg_cuvs/bench/results/cohere_N$${N}_summary.csv 2>/dev/null \
		|| echo 'Not ready yet — check: make gpu-cohere-log'"

.PHONY: gpu-cohere gpu-cohere-log gpu-cohere-result

gpu-shell:
	ssh -tt $(GCP_VM)

# Report the VM's current power state and external IP (ephemeral IPs change on
# stop/start, so .env.gpu's GCP_VM can go stale). Usage: make vm-ip
vm-ip:
	@gcloud compute instances describe $(GCP_INSTANCE) --zone $(GCP_ZONE) \
		$(if $(GCP_PROJECT),--project $(GCP_PROJECT)) \
		--format='value(status,networkInterfaces[0].accessConfigs[0].natIP)'
.PHONY: vm-ip

# Run ad-hoc SQL on the VM's postgres DB from stdin. For introspection and
# Phase 3K integration checks. Usage: make gpu-sql < query.sql  (or via heredoc)
gpu-sql:
	ssh -o StrictHostKeyChecking=accept-new $(GCP_VM) \
		"source ~/miniforge3/bin/activate $(CONDA_ENV) && \
		psql -d $(if $(DB),$(DB),postgres) -P pager=off -A -F '|'"
.PHONY: gpu-sql

# ---- Comparative ANN benchmark (infra/anbench) -------------------------
# pg_cuvs vs pgvector(hnsw/ivfflat) vs raw cuvs vs faiss-gpu/cpu on the same
# real dataset (Cohere wiki en, 1024d, cosine). Runs entirely on the VM.
.PHONY: gpu-anbench gpu-anbench-5m gpu-anbench-agg

# One corpus size (default 1M). Override: make gpu-anbench N=5000000
gpu-anbench:
	@mkdir -p design/anbench
	ssh $(GCP_VM) "cd ~/pg_cuvs && N=$(if $(N),$(N),1000000) KS=$(if $(KS),$(KS),10,100) \
		bash infra/anbench/run_all.sh 2>&1" \
		| tee design/anbench/run_N$(if $(N),$(N),1000000)_$(shell date +%Y%m%d_%H%M).log

# Large tier (GPU-feasible max at 1024d).
gpu-anbench-5m:
	$(MAKE) gpu-anbench N=5000000

# Aggregate results into summary.csv/txt + Pareto plots, then pull back locally.
gpu-anbench-agg:
	@mkdir -p design/anbench
	ssh $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate cuvs_py && \
		pip install -q matplotlib 2>/dev/null; python infra/anbench/aggregate.py"
	rsync -avz $(GCP_VM):~/pg_cuvs/design/anbench/ design/anbench/

# Convenience: full cycle on the VM (sync → build → install → test).
gpu-cycle: sync gpu-build gpu-install gpu-test
