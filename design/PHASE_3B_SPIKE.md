# Phase 3B Spike — cuVS Vamana (GPU build) -> MS DiskANN (CPU search) round-trip

> Status: **3B-0a complete (2026-05-28)** + **3B-0b complete (2026-05-29)**.
> Verdict: **GO** for the in-memory path (L2, recall to 0.999); **NO-GO** for the
> disk/PQFlash path in cuVS 26.04 (the serialized disk index loads in MS DiskANN
> but search recall collapses — on-disk layout incompatible). Verified on hardware,
> not just headers. No product code changed.
>
> ### Phase 3B ladder
> - **3B-0a** Graph compatibility spike (in-memory) — **DONE, GO**.
> - **3B-0b** PQ/disk (PQFlash) compatibility spike — **DONE, NO-GO (cuVS 26.04)**.
> - **3B-1** pg_cuvs DiskANN in-memory-index prototype (RAM-bound; the viable path).
> - **3B-2** pg_cuvs NVMe/PQ DiskANN cold tier — blocked on a DiskANN-compatible
>   disk serializer (see 3B-0b verdict for options).

## Why this spike

Phase 3B-0 (header/doc spike) established that cuVS Vamana provides **build +
serialize only — no search of any kind** (`cuvsVamanaSearch` does not exist in
C or C++; the only namespace free functions are `build`, `serialize`,
`deserialize_codebooks`). The serialize doc-comment in the installed 26.04 header
states verbatim:

> "Matches the file format used by the DiskANN open-source repository ... Serialized
> Index is to be used by the DiskANN open-source repository for graph search."

So a Vamana index in pg_cuvs is only useful if cuVS's serialized output can
actually be **loaded and searched by Microsoft DiskANN**. The GCS work taught us
that "documented / present in headers" != "works at round-trip" (PUT-vs-POST,
whitespace-JSON bugs surfaced only against a real bucket). spike-2 closes the same
gap for DiskANN **before** any 3B design or code is written.

## Scope (locked, per decision)

Allowed: install diskannpy on the dev VM; isolated experiment dir; small synthetic
data; **L2 only**; cuVS Vamana build + serialize; MS DiskANN load + search; recall
vs brute-force ground truth.

Forbidden (and not done): pg_cuvs daemon / AM / SQL / artifact-contract / GCS
changes; CAGRA-DiskANN hybrid design; cosine/IP (deferred until L2 proven).

## Environment (installed, verified)

| Component | Version |
|---|---|
| libcuvs (conda `cuvs_dev`) | **26.04** |
| GPU / driver | A100-SXM4-40GB / 610.43.02 |
| nvcc / CUDA | 12.9 (V12.9.86), libcudart 12.9.79 |
| host compiler | conda `x86_64-conda-linux-gnu-g++` 14.3.0 (via `NVCC_PREPEND_FLAGS=-ccbin=...`) |
| diskannpy (isolated venv, system py3.10) | **0.7.0** (manylinux wheel) |

Build/search are split across two environments connected only by on-disk files —
this mirrors the real product split (GPU build env vs CPU search env).

## Method

Harness `~/spike3b/spike_build.cu` (throwaway, on VM): generate synthetic L2 data
(N=10000, dim=64, seed=1234), write `data.fbin` + `query.fbin` (100 queries) in
DiskANN `.bin` format, build Vamana on GPU, serialize twice — `mem`
(`sector_aligned=false`) and `disk` (`sector_aligned=true`). Index params:
`metric=L2Expanded, graph_degree(R)=32, visited_size(L)=64`.

Recall side `~/spike3b/recall.py` (diskannpy venv): read the same `.fbin`, compute
brute-force L2 top-10 ground truth with numpy, load the cuVS output via diskannpy,
`batch_search`, score recall@10.

## Findings

### F1 — Build must be device-resident (host overload crashes)

`cuvs::neighbors::vamana::build` with a **host** `mdspan` aborts with
`cudaErrorIllegalAddress` inside `batched_insert_vamana` (cuVS 26.04). Switching to
a **device** matrix (`make_device_matrix` + `update_device`) builds cleanly.

> Contract for any product integration: feed Vamana build device-resident data,
> exactly as the existing CAGRA wrapper already does (`cuvs_wrapper.cu`). The
> header advertises a host overload; it does not work here.

### F2 — Emitted file families

| serialize mode | files | note |
|---|---|---|
| `mem` (sector_aligned=false) | `mem` (1,313,372 B graph), `mem.data` (2,560,008 B) | DiskANN **in-memory** index format |
| `disk` (sector_aligned=true) | `disk_disk.index` (4,100,096 B), `disk.data`, `disk_pq_compressed.bin` (**8 B = empty stub**) | DiskANN **disk/PQFlash** format; **no `disk_pq_pivots.bin`** |

`mem` graph header decodes as `(expected_file_size=1313372, 0, max_degree=32,
start/medoid=7537)` — the canonical DiskANN memory-index header. A diskannpy-built
**reference** index on the same data produced a structurally identical header
`(1313996, 0, 32, 37)`; both report `file_frozen_pts: 0`, `10000 nodes`, ~318k
out-edges (cuVS 318,337 vs ref 318,493). No frozen point is added; the graphs are
equivalent in structure. (Reference diskannpy build also wrote `ann_metadata.bin`
+ `ann_vectors.bin` sidecars that cuVS does not emit, but they are not required for
the memory loader to work.)

### F3 — In-memory round-trip WORKS (the gate) [GO]

`StaticMemoryIndex(index_directory=~/spike3b, index_prefix='mem', distance_metric='l2',
dimensions=64)` loads cuVS's `mem`/`mem.data` **directly** (no clean-dir or rename
needed — the sibling `disk_*` files were ignored) and DiskANN logs confirm a clean
parse:

```
From graph header, expected_file_size: 1313372, _max_observed_degree: 32, _start: 7537
Loading vamana graph .../mem...done. Index has 10000 nodes and 318337 out-edges
```

Recall@10 vs numpy brute-force L2, sweeping search `complexity` (k=10, 100 queries):

| complexity | recall@10 |
|---|---|
| 10 | 0.419 |
| 20 | 0.612 |
| 50 | 0.851 |
| 100 | 0.954 |
| 200 | 0.990 |
| 400 | **0.999** |

Monotonic rise toward ~1.0 = a genuinely navigable, compatible graph (not a partial
parse or coincidence). A control test copying the cuVS graph to the canonical
`ann`/`ann.data` prefix gave the identical 0.954 @complexity=100, confirming the
result is the cuVS graph itself, not loader luck. **cuVS 26.04 Vamana (GPU build)
-> MS DiskANN StaticMemoryIndex (CPU search) is a real, tunable, high-recall
round-trip for L2.**

> Contract: the in-memory loader needs the `<prefix>` graph + `<prefix>.data`
> sibling and the exact `index_prefix`/`dimensions`/`distance_metric`. Stray
> sibling files did NOT break the memory load.

### F4 — Disk/PQFlash, first attempt without codebooks [resolved in 3B-0b]

With `index_params.codebooks` left unset, cuVS emitted `disk_disk.index` +
`disk.data` but only an **empty 8-byte `disk_pq_compressed.bin`** and **no
`disk_pq_pivots.bin`**, so `StaticDiskIndex` could not load. This pointed at the
two-tool dance (DiskANN trains PQ -> cuVS consumes via `deserialize_codebooks` ->
DiskANN searches). **3B-0b below ran that dance to completion** and found the disk
path does NOT round-trip in cuVS 26.04.

## Verdict and implications (3B-0a, in-memory)

- **GO for the in-memory DiskANN path.** cuVS Vamana on GPU + MS DiskANN
  `StaticMemoryIndex` on CPU is proven compatible (L2, recall to 0.999). This is
  RAM-bound (not larger-than-RAM) but already offloads the expensive graph build to
  the GPU while keeping search on CPU.
- The **larger-than-RAM disk/PQFlash** path was then taken up in **3B-0b** (below)
  and came back **NO-GO** for cuVS 26.04 (loads but recall collapses).
- Design contracts already extracted: (1) device-resident build; (2) per-index
  artifact set under a unique `<prefix>` (graph + `.data`); (3) search is pure CPU
  (GPU idle at query time for DiskANN indexes) — pg_cuvs becomes "GPU build + CPU
  search" for this index type, distinct from CAGRA's GPU-both model.
- Next within scope: metric expansion (cosine/IP) on the proven in-memory path,
  before committing to 3B design (build wrapper, algorithm discriminator, artifact
  contract, AM `USING diskann`). The disk/PQ question is answered by 3B-0b.

## Reproduction (on `pg-cuvs-dev`)

```bash
# build env
source /home/ubuntu/miniforge3/etc/profile.d/conda.sh && conda activate cuvs_dev
cd ~/spike3b
nvcc -O3 -std=c++17 -arch=sm_80 -DRAFT_SYSTEM_LITTLE_ENDIAN=1 \
  -I$CONDA_PREFIX/include -I$CONDA_PREFIX/include/rapids \
  spike_build.cu -o spike_build \
  -L$CONDA_PREFIX/lib -lcuvs -lrmm -lcudart -lstdc++ -lpthread -lrt
./spike_build 10000 64 100 ~/spike3b           # device build + dual serialize

# search env (isolated venv, diskannpy 0.7.0)
~/spike3b/dvenv/bin/python recall.py ~/spike3b 10 100   # recall sweep
```

Artifacts live under `~/spike3b/` on the VM (harness `spike_build.cu`,
`recall.py`, `.fbin` data, `mem*`/`disk*` outputs, `ref/` diskannpy reference).
Not committed to the repo (throwaway spike code).

---

# Phase 3B-0b — PQ/disk (PQFlash) compatibility (2026-05-29)

> Status: **complete**. Verdict: **NO-GO** for the disk/PQFlash round-trip in
> cuVS 26.04. The "DiskANN trains PQ -> cuVS builds quantized disk index ->
> DiskANN searches" dance was run end-to-end on `pg-cuvs-dev`: codebook ingestion
> and PQ encoding succeed, but the cuVS-serialized disk index, while it **loads**
> in MS DiskANN `StaticDiskIndex`, returns **collapsed recall** (~0.40 peak vs
> 0.885 native) — its on-disk sector/node layout is not what PQFlash expects.

## Why this sub-spike

3B-0a proved the in-memory path but is RAM-bound. The real capacity goal (3B-2,
larger-than-RAM NVMe) needs a quantized disk index. 3B-0b answers **"who generates
the PQ pivots/codebooks, and does cuVS's quantized disk index actually search in
MS DiskANN?"** — the disk analogue of the 3B-0a gate.

## Environment delta from 3B-0a

| Component | Detail |
|---|---|
| diskannpy reference build | `build_disk_index(..., pq_disk_bytes=32, index_prefix="ref")`, L2 |
| diskannpy OPQ | `defaults.USE_OPQ = false`; `build_disk_index` does **not** expose `use_opq` |
| cuVS API | `vamana::index_params.codebooks` (build-time field); `deserialize_codebooks(prefix, dim)` (no `raft::resources` arg) |

## Method

Same synthetic L2 data as 3B-0a (`data.fbin`, N=10000, dim=64). (1) DiskANN-native
reference disk build (`build_ref.py`) for pivots + baseline; (2) cuVS Vamana build
with `index_params.codebooks = deserialize_codebooks(...)`, serialize
`sector_aligned=true` (`spike_build2.cu`); (3) assemble cuVS graph + cuVS PQ codes +
DiskANN pivots/aux and load in `StaticDiskIndex` (`asm_recall.py`); recall@10 vs the
same numpy brute-force GT. All outputs captured to files on the VM (streamed SSH
output proved unreliable; file + `grep` for specific tokens is the trustworthy path).

## Findings

### G1 — `deserialize_codebooks` requires an OPQ rotation-matrix file

`deserialize_codebooks(prefix, dim)` reads **both** `<prefix>_pq_pivots.bin` **and**
`<prefix>_pq_pivots.bin_rotation_matrix.bin`. MS DiskANN `build_disk_index` defaults
to non-OPQ and **omits the rotation matrix**, so the call aborts:

```
RAFT failure at vamana_codebooks.cuh line=88:
  Cannot open file .../ref_pq_pivots.bin_rotation_matrix.bin
```

> Contract: to feed DiskANN-trained PQ into cuVS you must either build DiskANN with
> OPQ, or inject a rotation matrix. A synthetic **identity** `D x D` matrix written
> in DiskANN `bin` format (`[int32 D][int32 D][float identity]`, 16,392 B for D=64)
> satisfies the parser: `deserialize_codebooks` then returns
> `pq_codebook_size=256, pq_dim=64, enc_table=16384 (=256x64), rot=4096 (=64x64)`.
> Non-OPQ PQ == OPQ with identity rotation, so this bridge is semantically sound.

### G2 — With codebooks set, cuVS DOES emit a real quantized disk index

`spike_build2` (codebooks attached) serializes `sector_aligned=true` to three files:
`cuq_disk.index` (4,100,096 B), `cuq.data` (2,560,008 B), and a **full**
`cuq_pq_compressed.bin` (640,008 B = 10000 x 64 bytes/vec) — no longer the 8-byte
stub of F4. So cuVS genuinely PQ-encodes the dataset with the supplied codebook.

### G3 — DiskANN StaticDiskIndex loads the cuVS disk index but recall COLLAPSES [NO-GO]

Assembling cuVS's `cuq_disk.index` + `cuq_pq_compressed.bin` with DiskANN's matching
64-chunk `ref_pq_pivots.bin` (+ `mem.index.data`/`sample_*`/`metadata` aux), prefix
`cuq`, `StaticDiskIndex` **loads cleanly** ("Loaded PQ centroids ... #chunks: 64")
but searches poorly:

| complexity | cuVS-graph disk (3b) | DiskANN-native ref | in-memory (3B-0a) |
|---|---|---|---|
| 10  | 0.057 | 0.510 | 0.419 |
| 20  | 0.077 | 0.666 | 0.612 |
| 50  | 0.132 | 0.808 | 0.851 |
| 100 | 0.190 | 0.868 | 0.954 |
| 200 | 0.287 | 0.882 | 0.990 |
| 400 | **0.405** | **0.885** | **0.999** |

Recall **rises** with complexity (0.057 -> 0.405) rather than staying at random
(~10/10000), so the graph is partly navigable — but the cuVS disk index packs
**1001 sectors** vs DiskANN's **417 sectors (24 nodes/sector, 164 B/node)**, so
PQFlash reads full-precision rerank vectors from the wrong on-disk offsets and the
top-k is corrupted. 3B-0a established cuVS preserves node-id == data-row ordering
(in-memory recall 0.999), so this is a **disk-layout** incompatibility, not an
ordering bug — and OPQ would not fix it (it changes PQ quality, not sector packing).
cuVS 26.04's only disk knob is `sector_aligned`, so there is no other format to try.

## Verdict and implications for Phase 3B

- **NO-GO** for "cuVS builds the disk index, MS DiskANN PQFlash searches it" in
  cuVS 26.04. Despite the serialize doc-comment's "matches the DiskANN file format"
  claim, the sector-aligned disk index is not searchable by PQFlash (loads, but
  recall collapses). The header claim holds for the **in-memory** format (3B-0a),
  not the disk format.
- The viable cuVS+DiskANN combo remains **in-memory only** (3B-0a). For 3B-1, a
  pg_cuvs DiskANN **in-memory** index (GPU build + CPU `StaticMemoryIndex` search,
  RAM-bound) is the path that actually works today.
- For the larger-than-RAM **3B-2** goal, the disk index must be made
  DiskANN-compatible by one of: (a) re-layout cuVS's graph into DiskANN's PQFlash
  sector format ourselves (own serializer); (b) use MS DiskANN's own disk build for
  the cold tier (GPU/cuVS then only accelerates the in-memory tier); (c) re-test on
  a future cuVS whose `sector_aligned` serialize is PQFlash-correct. Decision
  deferred to 3B design.
- Design contract (PQ ingestion): cuVS needs OPQ-shaped pivots; supply DiskANN OPQ
  output or an identity rotation matrix (G1).

## Reproduction (on `pg-cuvs-dev`, additive to 3B-0a)

```bash
# build env (conda) — reference PQ + cuVS quantized build
source /home/ubuntu/miniforge3/etc/profile.d/conda.sh && conda activate cuvs_dev
cd ~/spike3b
dvenv/bin/python build_ref.py ~/spike3b 32            # DiskANN-native disk + pivots
python - <<'PY'                                       # identity rotation matrix bridge (G1)
import numpy as np; D=64
with open("ref/ref_pq_pivots.bin_rotation_matrix.bin","wb") as f:
    np.array([D,D],dtype=np.int32).tofile(f); np.eye(D,dtype=np.float32).tofile(f)
PY
nvcc -O3 -std=c++17 -arch=sm_80 -DRAFT_SYSTEM_LITTLE_ENDIAN=1 \
  -I$CONDA_PREFIX/include -I$CONDA_PREFIX/include/rapids \
  spike_build2.cu -o spike_build2 \
  -L$CONDA_PREFIX/lib -lcuvs -lrmm -lcudart -lstdc++ -lpthread -lrt
LD_LIBRARY_PATH=$CONDA_PREFIX/lib ./spike_build2 ~/spike3b ~/spike3b/ref/ref

# search env — assemble cuVS graph + DiskANN PQ, score recall (collapses)
dvenv/bin/python asm_recall.py        # RES cpx=... recall@10=... (0.057 -> 0.405)
```

Throwaway harness on the VM (`build_ref.py`, `spike_build2.cu`, `spike_codebook.cu`,
`asm_recall.py`); not committed.
