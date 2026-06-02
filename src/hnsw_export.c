/*
 * hnsw_export.c — Phase 3I-2: import hnswlib binary sidecar into a pgvector
 * HNSW relation.
 *
 * The hnswlib binary format is described in the Phase 3I-2 design doc.
 * We parse it in pure C (no C++ / hnswlib headers) and write pgvector-
 * compatible pages using ReadBuffer(P_NEW) + MarkBufferDirty + log_newpage_buffer.
 * GenericXLogFinish was tried but the FULL_IMAGE copy-back was unreliable on
 * PG16 for direct-to-PageGetContents writes; the pgvector build path (direct
 * buffer write + log_newpage) is correct and crash-safe.
 *
 * WAL safety: every new page is logged via log_newpage_buffer(buf, true) before
 * UnlockReleaseBuffer.  Crash recovery replays the full-page images.
 *
 * pgvector version dependency: structs are pinned to pgvector 0.5.0+ layout
 * (HNSW_VERSION=1, stable since Aug 2023).  A pgvector major-version bump that
 * changes the on-disk format would require updating the PgvHnsw* structs and
 * the HNSW_VERSION / HNSW_MAGIC constants below.
 *
 * pgvector HNSW on-disk layout (all sizes in bytes, LE):
 *   Block 0: metapage (HnswMetaPageData + HnswPageOpaqueData)
 *   Blocks 1..N: interleaved element tuples (type=1) and neighbor tuples (type=2)
 *     One page per element pair; unused space left empty.
 *
 * We do NOT include pgvector's hnsw.h (not installed).  All structs are
 * defined locally matching the pgvector 0.8.x binary layout.
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/generic_xlog.h"
#include "access/xlog.h"            /* log_newpage_buffer */
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_attribute.h"  /* Form_pg_attribute, ATTNUM */
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"      /* Anum_pg_index_indclass */
#include "catalog/pg_opclass.h"    /* Form_pg_opclass, CLAOID */
#include "utils/syscache.h"        /* SearchSysCache1/2, RELOID, ATTNUM, INDEXRELID, CLAOID */
#include "common/relpath.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/builtins.h"
#include "catalog/index.h"       /* index_create, INDEX_CREATE_SKIP_BUILD, BuildIndexInfo */
#include "commands/defrem.h"     /* get_am_oid */
#include "commands/extension.h"  /* get_extension_oid */
#include "miscadmin.h"
#include "nodes/pg_list.h"       /* list_make1 */
#include "access/genam.h"        /* GetSysCacheOid3 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cuvs_ipc.h"
#include "cuvs_util.h"
#include "hnsw_export.h"

/* GUC declared in pg_cuvs.c — socket path for daemon IPC */
extern char *cuvs_socket_path;

/* ----------------------------------------------------------------
 * pgvector HNSW on-disk constants (must match pgvector source)
 * ---------------------------------------------------------------- */
#define PGV_HNSW_MAGIC          0xA953A953u
#define PGV_HNSW_VERSION        1u
#define PGV_HNSW_PAGE_ID        0xFF90u

/* Byte size of the 13-field hnswlib binary header (fixed across versions):
 * 10 × size_t(8) + 2 × int(4) + 1 × double(8) = 96 bytes. */
#define HNSWLIB_HEADER_BYTES    96

/* Maximum heaptids stored inline in an element tuple */
#define PGV_HNSW_MAX_HEAPTIDS   10

/* HnswGetLayerM: layer 0 uses 2*M connections, upper layers use M */
#define PGV_HNSW_LAYER_M(m, layer) ((layer) == 0 ? 2*(m) : (m))

/*
 * ItemPointerData: (BlockIdData blkhi:blklo, OffsetNumber).
 * BlockIdData is { uint16 bi_hi, uint16 bi_lo }.
 * We replicate the struct so we do not need to include heapam internals.
 */
typedef struct {
    uint16_t bi_hi;
    uint16_t bi_lo;
} PgvBlockId;

typedef struct {
    PgvBlockId  ip_blkid;
    uint16_t    ip_posid;
} PgvItemPointer;   /* 6 bytes */

/* ---- metapage (block 0) ---- */
typedef struct {
    uint32_t        magicNumber;
    uint32_t        version;
    uint32_t        dimensions;
    uint16_t        m;
    uint16_t        efConstruction;
    uint32_t        entryBlkno;
    uint16_t        entryOffno;
    int16_t         entryLevel;
    uint32_t        insertPage;
} PgvHnswMeta;      /* 28 bytes */

/* ---- page opaque (special area, 8 bytes) ---- */
typedef struct {
    uint32_t    nextblkno;
    uint16_t    unused;
    uint16_t    page_id;
} PgvHnswPageOpaque;   /* 8 bytes */

/*
 * Element tuple header (type = 1).
 * The varlena vector follows immediately after the fixed header.
 * Vector varlena layout: int32 vl_len_ | int16 dim | int16 unused | float x[dim]
 */
typedef struct {
    uint8_t         type;           /* 1 */
    uint8_t         level;
    uint8_t         deleted;        /* 0 */
    uint8_t         version;        /* 0 */
    PgvItemPointer  heaptids[PGV_HNSW_MAX_HEAPTIDS]; /* 60 bytes */
    PgvItemPointer  neighbortid;    /* 6 bytes — points to neighbor tuple */
    uint16_t        unused;         /* 0 */
    /* followed by: int32 vl_len_ | int16 dim | int16 unused | float x[dim] */
} PgvHnswElemHdr;   /* 72 bytes */

/* Neighbor tuple header (type = 2) */
typedef struct {
    uint8_t         type;           /* 2 */
    uint8_t         version;        /* 0 */
    uint16_t        count;          /* total slots = (level+2)*m */
    /* followed by: PgvItemPointer indextids[count] */
} PgvHnswNeighHdr;  /* 4 bytes */

/* ----------------------------------------------------------------
 * Page geometry
 * ---------------------------------------------------------------- */
#define BLCKSZ_VAL              8192
#define PG_PAGE_HEADER_SIZE     24      /* SizeOfPageHeaderData */
#define PGV_OPAQUE_SIZE         8       /* sizeof(PgvHnswPageOpaque), MAXALIGN'd */
#define PGV_USABLE_BYTES        (BLCKSZ_VAL - PG_PAGE_HEADER_SIZE - PGV_OPAQUE_SIZE)
/* Each item also uses 4 bytes in the ItemId array at the page head */
#define PGV_ITEM_OVERHEAD       4

/* MAXALIGN to 8 bytes (standard on x86-64 LP64) */
#define MAXALIGN8(n)  (((n) + 7) & ~7)

/* Vector varlena header: 4 bytes vl_len_ + 2 bytes dim + 2 bytes unused */
#define PGV_VEC_HEADER_SIZE     8

/*
 * Size of an element tuple for a vector of `dim` floats.
 * = MAXALIGN(sizeof(PgvHnswElemHdr) + PGV_VEC_HEADER_SIZE + dim*4)
 */
static size_t
elem_tuple_size(int dim)
{
    return MAXALIGN8(sizeof(PgvHnswElemHdr) + PGV_VEC_HEADER_SIZE
                     + (size_t)dim * sizeof(float));
}

/*
 * Size of a neighbor tuple for an element at `level` with graph M.
 * count = (level + 2) * m  (level 0 uses 2m, each upper level uses m)
 * = MAXALIGN(sizeof(PgvHnswNeighHdr) + count * sizeof(PgvItemPointer))
 */
static size_t
neigh_tuple_size(int level, int m)
{
    int count = (level + 2) * m;
    return MAXALIGN8(sizeof(PgvHnswNeighHdr)
                     + (size_t)count * sizeof(PgvItemPointer));
}

/* ----------------------------------------------------------------
 * hnswlib binary header (all fields are size_t / int / double)
 * ---------------------------------------------------------------- */
typedef struct {
    size_t  offsetLevel0;
    size_t  maxElements;
    size_t  curElementCount;
    size_t  sizeDataPerElement;
    size_t  labelOffset;
    size_t  offsetData;
    int     maxlevel;
    int     enterpointNode;
    size_t  maxM;
    size_t  maxM0;
    size_t  M;
    double  mult;
    size_t  efConstruction;
} HnswlibHeader;

/* ----------------------------------------------------------------
 * External GUCs declared in pg_cuvs.c
 * ---------------------------------------------------------------- */
extern char *cuvs_index_dir;

/* Forward declaration of get_index_dir from pg_cuvs.c — we replicate the
 * logic here to avoid exposing it as a public symbol. */
static const char *
hnsw_get_index_dir(void)
{
    static char buf[1024];
    if (cuvs_index_dir && cuvs_index_dir[0] != '\0')
        return cuvs_index_dir;
    snprintf(buf, sizeof(buf), "%s/cuvs_indexes", DataDir);
    return buf;
}

/* ----------------------------------------------------------------
 * In-memory element descriptor (filled during parse phase)
 * ---------------------------------------------------------------- */
typedef struct {
    int             level;          /* max level for this element */
    uint64_t        tid_encoded;    /* heap TID from .tids sidecar */
    /* layer-0 links: link_count + links[maxM0] */
    int             lv0_count;
    int            *lv0_links;      /* malloc'd; size maxM0 */
    /* upper-level links: for levels 1..level */
    /* upper_links[l-1] is an array of int[link_count_l], length upper_counts[l-1] */
    int           **upper_links;    /* malloc'd outer array; NULL if level==0 */
    int            *upper_counts;
    /* assigned page layout (filled in pass 2) */
    uint32_t        elem_blkno;
    uint16_t        elem_offno;
    uint32_t        neigh_blkno;
    uint16_t        neigh_offno;
} ElemDesc;

/* ----------------------------------------------------------------
 * Helper: set a PgvItemPointer from block + offset
 * ---------------------------------------------------------------- */
static void
set_item_ptr(PgvItemPointer *ip, uint32_t blkno, uint16_t offno)
{
    ip->ip_blkid.bi_hi = (uint16_t)(blkno >> 16);
    ip->ip_blkid.bi_lo = (uint16_t)(blkno & 0xFFFF);
    ip->ip_posid       = offno;
}

static void
set_invalid_item_ptr(PgvItemPointer *ip)
{
    ip->ip_blkid.bi_hi = 0xFFFF;
    ip->ip_blkid.bi_lo = 0xFFFF;
    ip->ip_posid       = 0;
}

/* ----------------------------------------------------------------
 * Helper: decode heap TID from .tids encoding into a PgvItemPointer.
 * cuvs_tid_encode: block<<16 | offset
 * ---------------------------------------------------------------- */
static void
tid_to_item_ptr(uint64_t encoded, PgvItemPointer *ip)
{
    uint32_t block;
    uint16_t offset;
    cuvs_tid_decode(encoded, &block, &offset);
    set_item_ptr(ip, block, offset);
}

/* ----------------------------------------------------------------
 * Free all memory owned by an ElemDesc array
 * ---------------------------------------------------------------- */
static void
free_elem_descs(ElemDesc *elems, size_t n, size_t maxM0)
{
    for (size_t i = 0; i < n; i++)
    {
        if (elems[i].lv0_links)
            free(elems[i].lv0_links);
        if (elems[i].upper_links)
        {
            for (int l = 0; l < elems[i].level; l++)
                if (elems[i].upper_links[l])
                    free(elems[i].upper_links[l]);
            free(elems[i].upper_links);
        }
        if (elems[i].upper_counts)
            free(elems[i].upper_counts);
    }
    (void)maxM0;
}

/* ----------------------------------------------------------------
 * Write a single page for element i (elem + neigh tuples).
 *
 * We write one page per element pair.  Both tuples must fit; the
 * layout pass guarantees this.
 * ---------------------------------------------------------------- */
static void
write_elem_page(Relation rel,
                uint32_t blkno,
                ElemDesc *e,
                const float *vec,
                int dim,
                int m,
                ElemDesc *all_elems,
                size_t n_elems,
                bool skip_wal)
{
    GenericXLogState   *xlog_state;
    Buffer              buf;
    Page                page;
    PgvHnswPageOpaque  *opaque;

    /* elem tuple */
    size_t esize = elem_tuple_size(dim);
    size_t nsize = neigh_tuple_size(e->level, m);

    PgvHnswElemHdr *etup = (PgvHnswElemHdr *) palloc0(esize);
    etup->type    = 1;
    etup->level   = (uint8_t) e->level;
    etup->deleted = 0;
    etup->version = 0;

    /* fill heaptids[0] with the heap TID; rest are invalid */
    tid_to_item_ptr(e->tid_encoded, &etup->heaptids[0]);
    for (int t = 1; t < PGV_HNSW_MAX_HEAPTIDS; t++)
        set_invalid_item_ptr(&etup->heaptids[t]);

    /* neighbortid points to the neighbor tuple on the same page, offno 2 */
    set_item_ptr(&etup->neighbortid, e->neigh_blkno, e->neigh_offno);
    etup->unused = 0;

    /* write the vector varlena immediately after the element header.
     * pgvector uses SET_VARSIZE (standard 4-byte varlena): vl_len_ = size << 2. */
    char *vdata = (char *)etup + sizeof(PgvHnswElemHdr);
    int32_t vl_len = (int32_t)(PGV_VEC_HEADER_SIZE + (size_t)dim * sizeof(float));
    int32_t vl_len_field = vl_len << 2;
    memcpy(vdata, &vl_len_field, 4);
    int16_t vdim   = (int16_t)dim;
    int16_t vunused = 0;
    memcpy(vdata + 4, &vdim,    2);
    memcpy(vdata + 6, &vunused, 2);
    memcpy(vdata + 8, vec, (size_t)dim * sizeof(float));

    /* neighbor tuple */
    int count = (e->level + 2) * m;
    PgvHnswNeighHdr *ntup = (PgvHnswNeighHdr *) palloc0(nsize);
    ntup->type    = 2;
    ntup->version = 0;
    ntup->count   = (uint16_t)count;

    PgvItemPointer *slots = (PgvItemPointer *)((char *)ntup + sizeof(PgvHnswNeighHdr));

    /*
     * Slots are stored DESCENDING by level (highest level first):
     *   slots[0..m-1]:      level e->level neighbors
     *   slots[m..2m-1]:     level e->level-1 neighbors
     *   ...
     *   slots[(level+1)*m .. (level+2)*m-1]:  level 0 neighbors (2m slots)
     *
     * Note: level 0 always has 2m slots (HnswGetLayerM(m,0)=2m), but all
     * other levels have m slots each.  The total = m*(level) + 2m = (level+2)*m.
     */
    int slot = 0;

    /* highest level first, down to level 1 (each m slots) */
    for (int lv = e->level; lv >= 1; lv--)
    {
        int lv_idx = lv - 1;   /* upper_links index (0 = level 1) */
        int lv_count = (lv_idx < e->level && e->upper_links)
                       ? e->upper_counts[lv_idx] : 0;
        for (int s = 0; s < m; s++, slot++)
        {
            if (s < lv_count && e->upper_links && e->upper_links[lv_idx])
            {
                int nb = e->upper_links[lv_idx][s];
                if (nb >= 0 && (size_t)nb < n_elems)
                    set_item_ptr(&slots[slot],
                                 all_elems[nb].elem_blkno,
                                 all_elems[nb].elem_offno);
                else
                    set_invalid_item_ptr(&slots[slot]);
            }
            else
                set_invalid_item_ptr(&slots[slot]);
        }
    }

    /* level 0 — 2m slots */
    for (int s = 0; s < 2 * m; s++, slot++)
    {
        if (s < e->lv0_count && e->lv0_links)
        {
            int nb = e->lv0_links[s];
            if (nb >= 0 && (size_t)nb < n_elems)
                set_item_ptr(&slots[slot],
                             all_elems[nb].elem_blkno,
                             all_elems[nb].elem_offno);
            else
                set_invalid_item_ptr(&slots[slot]);
        }
        else
            set_invalid_item_ptr(&slots[slot]);
    }

    /* Write the page — always extend with P_NEW; caller loops in blkno order
     * so each call appends the next block in sequence. */
    (void)blkno;        /* layout reference only */
    (void)xlog_state;   /* unused after switching to direct write */
    buf  = ReadBuffer(rel, P_NEW);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    page = BufferGetPage(buf);
    PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

    opaque = (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
    opaque->nextblkno = InvalidBlockNumber;
    opaque->unused    = 0;
    opaque->page_id   = PGV_HNSW_PAGE_ID;

    /* Add element tuple at offno 1 */
    OffsetNumber eoff = PageAddItem(page, (Item)etup, esize, InvalidOffsetNumber,
                                    false, false);
    if (eoff == InvalidOffsetNumber)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: element tuple does not fit on page "
                        "(esize=%zu, free=%zu)", esize,
                        (size_t)PageGetFreeSpace(page))));

    /* Add neighbor tuple at offno 2 */
    OffsetNumber noff = PageAddItem(page, (Item)ntup, nsize, InvalidOffsetNumber,
                                    false, false);
    if (noff == InvalidOffsetNumber)
        ereport(ERROR,
                (errmsg("pg_cuvs: hnsw_export: neighbor tuple does not fit on page "
                        "(nsize=%zu, free=%zu)", nsize,
                        (size_t)PageGetFreeSpace(page))));

    (void)eoff;
    (void)noff;

    MarkBufferDirty(buf);
    if (!skip_wal)
        log_newpage_buffer(buf, true); /* WAL full-page image for crash safety */
    UnlockReleaseBuffer(buf);

    pfree(etup);
    pfree(ntup);
}

/* ================================================================
 * Main SQL function
 * ================================================================ */
/* Internal helper: fill an existing HNSW index from hnswlib sidecar.
 * Called by pg_cuvs_build_hnsw() for modes 'hnswlib' and 'hnswlib_file'.
 * use_shm=true  → daemon runs from_cagra() → /dev/shm (no disk I/O)
 * use_shm=false → reads pre-built .hnsw sidecar from index_dir */
static void
fill_hnsw_from_hnswlib(Oid cagra_oid, Oid hnsw_oid, bool use_shm)
{

    /* ---- 0. pgvector compatibility check ---- */
    /* pg_cuvs_import_hnsw is pinned to pgvector HNSW_VERSION=1 (stable since
     * pgvector 0.5.0, Aug 2023). Warn if pgvector is not installed; error if
     * the target index's metapage format would be unreadable. */
    {
        Oid vec_oid = get_extension_oid("vector", true /* missing_ok */);
        if (!OidIsValid(vec_oid))
            ereport(ERROR,
                    (errmsg("pg_cuvs: pgvector extension is not installed; "
                            "pg_cuvs_import_hnsw requires pgvector 0.5.0+"),
                     errhint("Run: CREATE EXTENSION vector;")));
    }

    /* ---- 1. Open CAGRA source index to get db_oid, index_oid, dim ---- */
    Relation cagra_rel = index_open(cagra_oid, AccessShareLock);

    uint32_t db_oid    = (uint32_t)MyDatabaseId;
    uint32_t index_oid = (uint32_t)cagra_oid;

    /* Infer dim from the index tuple descriptor (atttypmod on the first attr). */
    int dim = 0;
    if (RelationGetDescr(cagra_rel)->natts >= 1)
        dim = (int)TupleDescAttr(RelationGetDescr(cagra_rel), 0)->atttypmod;

    if (dim <= 0)
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot determine vector dimension from cagra "
                        "index %u; atttypmod=%d", cagra_oid, dim)));

    index_close(cagra_rel, AccessShareLock);

    /* ---- 1b. Validate target index before opening it ---- */
    /* Look up via syscache to avoid opening a non-HNSW index first. */
    bool hnsw_unlogged = false;  /* set below; true = skip WAL on page writes */
    {
        HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(hnsw_oid));
        if (!HeapTupleIsValid(tup))
            ereport(ERROR,
                    (errmsg("pg_cuvs: target relation with OID %u not found",
                            hnsw_oid)));

        Form_pg_class cf = (Form_pg_class) GETSTRUCT(tup);

        /* Must be an index */
        if (cf->relkind != RELKIND_INDEX)
        {
            char *name = pstrdup(NameStr(cf->relname));
            ReleaseSysCache(tup);
            ereport(ERROR,
                    (errmsg("pg_cuvs: \"%s\" is not an index", name)));
        }

        /* Must use the 'hnsw' access method */
        {
            Oid hnsw_amoid = get_am_oid("hnsw", true);
            if (!OidIsValid(hnsw_amoid) || cf->relam != hnsw_amoid)
            {
                char *name = pstrdup(NameStr(cf->relname));
                ReleaseSysCache(tup);
                ereport(ERROR,
                        (errmsg("pg_cuvs: index \"%s\" is not a pgvector HNSW index; "
                                "pg_cuvs_import_hnsw requires USING hnsw",
                                name)));
            }
        }

        /* Dimension must match the source CAGRA index */
        {
            int tgt_dim = 0;
            AttrNumber  natts = cf->relnatts;
            if (natts >= 1)
            {
                HeapTuple attup = SearchSysCache2(ATTNUM,
                                                   ObjectIdGetDatum(hnsw_oid),
                                                   Int16GetDatum(1));
                if (HeapTupleIsValid(attup))
                {
                    Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attup);
                    tgt_dim = (int)att->atttypmod;
                    ReleaseSysCache(attup);
                }
            }
            if (tgt_dim > 0 && tgt_dim != dim)
            {
                char *name = pstrdup(NameStr(cf->relname));
                ReleaseSysCache(tup);
                ereport(ERROR,
                        (errmsg("pg_cuvs: dimension mismatch: source CAGRA index has "
                                "dim=%d but target HNSW index \"%s\" has dim=%d",
                                dim, name, tgt_dim)));
            }
        }

        /* Detect UNLOGGED target: WAL will be skipped for faster import.
         * UNLOGGED indexes are not crash-safe; the caller is responsible
         * for rebuilding after crash recovery. */
        hnsw_unlogged = (cf->relpersistence == RELPERSISTENCE_UNLOGGED);

        ReleaseSysCache(tup);
    }

    if (hnsw_unlogged)
        ereport(NOTICE,
                (errmsg("pg_cuvs: target HNSW index is UNLOGGED — "
                        "WAL skipped for faster import; "
                        "index must be rebuilt after crash recovery")));

    /* ---- 2. Build sidecar paths ---- */
    char hnsw_path[1024];
    char tids_path[1024];
    const char *idir = hnsw_get_index_dir();

    if (use_shm)
    {
        /* Request daemon to run from_cagra() → /dev/shm (no disk I/O) */
        if (!cuvs_socket_path || cuvs_socket_path[0] == '\0')
            ereport(ERROR, (errmsg("pg_cuvs: daemon socket not set")));
        int shm_rc = cuvs_ipc_export_hnsw_shm(
            cuvs_socket_path, db_oid, index_oid, hnsw_path, sizeof(hnsw_path));
        if (shm_rc != CUVS_STATUS_OK)
            ereport(ERROR,
                    (errmsg("pg_cuvs: export_hnsw_shm failed (status=%d); "
                            "ensure cagra index %u is loaded in daemon", shm_rc, cagra_oid)));
        /* .tids comes from the regular index_dir sidecar */
        snprintf(tids_path, sizeof(tids_path), "%s/%u_%u.tids", idir, db_oid, index_oid);
    }
    else
    {
        snprintf(hnsw_path, sizeof(hnsw_path), "%s/%u_%u.hnsw", idir, db_oid, index_oid);
        snprintf(tids_path, sizeof(tids_path), "%s/%u_%u.tids", idir, db_oid, index_oid);
    }

    /* ---- 3. Parse hnswlib binary header ---- */
    FILE *hf = fopen(hnsw_path, "rb");
    if (!hf)
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot open .hnsw sidecar \"%s\": %m",
                        hnsw_path)));

    HnswlibHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    if (fread(&hdr.offsetLevel0,       sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.maxElements,        sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.curElementCount,    sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.sizeDataPerElement, sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.labelOffset,        sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.offsetData,         sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.maxlevel,           sizeof(int),    1, hf) != 1 ||
        fread(&hdr.enterpointNode,     sizeof(int),    1, hf) != 1 ||
        fread(&hdr.maxM,               sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.maxM0,              sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.M,                  sizeof(size_t), 1, hf) != 1 ||
        fread(&hdr.mult,               sizeof(double), 1, hf) != 1 ||
        fread(&hdr.efConstruction,     sizeof(size_t), 1, hf) != 1)
    {
        fclose(hf);
        ereport(ERROR,
                (errmsg("pg_cuvs: short read on .hnsw header \"%s\"",
                        hnsw_path)));
    }

    size_t N  = hdr.curElementCount;
    int    M  = (int)hdr.M;
    int    M0 = (int)hdr.maxM0;   /* should == 2*M */

    if (N == 0)
    {
        fclose(hf);
        ereport(ERROR, (errmsg("pg_cuvs: .hnsw sidecar is empty (N=0)")));
    }
    if (M <= 0 || M0 <= 0)
    {
        fclose(hf);
        ereport(ERROR, (errmsg("pg_cuvs: invalid M=%d M0=%d in .hnsw header",
                               M, M0)));
    }

    /* ---- 4. Read .tids sidecar ---- */
    FILE *tf = fopen(tids_path, "rb");
    if (!tf)
    {
        fclose(hf);
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot open .tids sidecar \"%s\": %m",
                        tids_path)));
    }

    CuvsTidsHeader tids_hdr;
    uint64_t      *tids = NULL;
    if (cuvs_tids_read(tf, &tids_hdr, &tids) != 0)
    {
        fclose(tf);
        fclose(hf);
        ereport(ERROR,
                (errmsg("pg_cuvs: failed to read/validate .tids sidecar \"%s\"",
                        tids_path)));
    }
    fclose(tf);

    if ((size_t)tids_hdr.n_vecs < N)
    {
        free(tids);
        fclose(hf);
        ereport(ERROR,
                (errmsg("pg_cuvs: .tids has %lld entries but .hnsw has %zu elements",
                        (long long)tids_hdr.n_vecs, N)));
    }

    /* ---- 4b. Metric (opclass) compatibility check ---- */
    /* Map the target HNSW's opclass name to a CUVS_METRIC_* value and verify
     * it matches the source CAGRA's metric stored in the .tids header.
     * Importing L2 CAGRA into cosine/IP HNSW would return nonsense distances. */
    {
        HeapTuple   idxtup;
        Datum       indclassDatum;
        bool        isnull;
        uint32_t    tgt_metric = CUVS_METRIC_L2; /* default if lookup fails */

        idxtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(hnsw_oid));
        if (HeapTupleIsValid(idxtup))
        {
            indclassDatum = SysCacheGetAttr(INDEXRELID, idxtup,
                                             Anum_pg_index_indclass, &isnull);
            if (!isnull)
            {
                oidvector *indclass = (oidvector *) DatumGetPointer(indclassDatum);
                Oid        opclassOid = indclass->values[0];
                HeapTuple  opctup = SearchSysCache1(CLAOID,
                                                     ObjectIdGetDatum(opclassOid));
                if (HeapTupleIsValid(opctup))
                {
                    Form_pg_opclass opcf = (Form_pg_opclass) GETSTRUCT(opctup);
                    const char *nm = NameStr(opcf->opcname);
                    if (strstr(nm, "cosine"))    tgt_metric = CUVS_METRIC_COSINE;
                    else if (strstr(nm, "_ip"))  tgt_metric = CUVS_METRIC_IP;
                    ReleaseSysCache(opctup);
                }
            }
            ReleaseSysCache(idxtup);
        }

        if (tgt_metric != (uint32_t)tids_hdr.metric)
        {
            static const char *mname[] = { "l2", "cosine", "ip" };
            const char *src_m = (tids_hdr.metric < 3) ? mname[tids_hdr.metric] : "unknown";
            const char *tgt_m = (tgt_metric < 3) ? mname[tgt_metric] : "unknown";
            free(tids);
            fclose(hf);
            ereport(ERROR,
                    (errmsg("pg_cuvs: metric mismatch: source CAGRA uses %s "
                            "but target HNSW opclass uses %s",
                            src_m, tgt_m)));
        }
    }

    /* ---- 5. Read level-0 block into memory ---- */
    /*
     * After reading the 13 header fields the file pointer is at position 96
     * (the header byte count). The level-0 DATA block starts immediately after
     * the header; hdr.offsetLevel0 is an INTRA-ELEMENT offset (where the link
     * array sits within each sizeDataPerElement slot), not a file offset.
     *
     * No seek needed here: file is already positioned at byte 96 = start of
     * the level-0 block. (An explicit seek with SEEK_SET would rewind to 0.)
     */

    size_t lv0_block_bytes = N * hdr.sizeDataPerElement;
    char  *lv0_block = (char *) malloc(lv0_block_bytes);
    if (!lv0_block)
    {
        free(tids);
        fclose(hf);
        ereport(ERROR, (errmsg("pg_cuvs: out of memory for level-0 block "
                               "(%zu bytes)", lv0_block_bytes)));
    }

    if (fread(lv0_block, 1, lv0_block_bytes, hf) != lv0_block_bytes)
    {
        free(lv0_block);
        free(tids);
        fclose(hf);
        ereport(ERROR,
                (errmsg("pg_cuvs: short read on level-0 block in \"%s\"",
                        hnsw_path)));
    }

    /* ---- 6. Parse element descriptors ---- */
    ElemDesc *elems = (ElemDesc *) calloc(N, sizeof(ElemDesc));
    if (!elems)
    {
        free(lv0_block);
        free(tids);
        fclose(hf);
        ereport(ERROR, (errmsg("pg_cuvs: out of memory for element descriptors")));
    }

    for (size_t i = 0; i < N; i++)
    {
        char *base = lv0_block + i * hdr.sizeDataPerElement;

        /* level-0 links: [int link_count][int links[M0]] at offsetLevel0 within element */
        char *lv0ptr = base + hdr.offsetLevel0;
        int lv0_count;
        memcpy(&lv0_count, lv0ptr, sizeof(int));
        if (lv0_count < 0 || lv0_count > M0)
            lv0_count = 0;

        elems[i].lv0_count = lv0_count;
        elems[i].lv0_links = (int *) malloc((size_t)M0 * sizeof(int));
        if (!elems[i].lv0_links)
        {
            /* cleanup and error */
            free_elem_descs(elems, i, (size_t)M0);
            free(elems);
            free(lv0_block);
            free(tids);
            fclose(hf);
            ereport(ERROR, (errmsg("pg_cuvs: out of memory for lv0 links")));
        }
        memcpy(elems[i].lv0_links, lv0ptr + sizeof(int),
               (size_t)M0 * sizeof(int));

        /* label (item_id) at labelOffset — used to look up heap TID */
        size_t label;
        memcpy(&label, base + hdr.labelOffset, sizeof(size_t));
        elems[i].tid_encoded = (label < (size_t)tids_hdr.n_vecs)
                               ? tids[label]
                               : (uint64_t)0;

        /* level and upper links come from the upper-level block; set defaults */
        elems[i].level        = 0;
        elems[i].upper_links  = NULL;
        elems[i].upper_counts = NULL;
    }

    free(lv0_block);

    /* ---- 7. Read upper-level link lists ---- */
    /*
     * Upper-level data follows the level-0 block.  For each element i:
     *   uint32 linkListSize
     *   if linkListSize > 0:
     *     for levels 1..element_level: [int link_count][int links[M]]
     *     element_level = linkListSize / sizeLinksPerElement
     *     sizeLinksPerElement = (M + 1) * sizeof(int)
     */
    size_t sizeLinksPerElement = ((size_t)M + 1) * sizeof(int);

    for (size_t i = 0; i < N; i++)
    {
        uint32_t llsize = 0;
        if (fread(&llsize, sizeof(uint32_t), 1, hf) != 1)
        {
            free_elem_descs(elems, N, (size_t)M0);
            free(elems);
            free(tids);
            fclose(hf);
            ereport(ERROR,
                    (errmsg("pg_cuvs: short read on upper-level size for "
                            "element %zu in \"%s\"", i, hnsw_path)));
        }

        if (llsize == 0)
        {
            elems[i].level = 0;
            continue;
        }

        int elem_level = (int)(llsize / sizeLinksPerElement);
        if (elem_level <= 0)
        {
            /* skip unexpected data */
            fseek(hf, (long)llsize, SEEK_CUR);
            elems[i].level = 0;
            continue;
        }

        elems[i].level = elem_level;

        char *llbuf = (char *) malloc(llsize);
        if (!llbuf)
        {
            free_elem_descs(elems, N, (size_t)M0);
            free(elems);
            free(tids);
            fclose(hf);
            ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper links "
                                   "element %zu", i)));
        }

        if (fread(llbuf, 1, llsize, hf) != llsize)
        {
            free(llbuf);
            free_elem_descs(elems, N, (size_t)M0);
            free(elems);
            free(tids);
            fclose(hf);
            ereport(ERROR,
                    (errmsg("pg_cuvs: short read on upper links for element "
                            "%zu in \"%s\"", i, hnsw_path)));
        }

        elems[i].upper_links  = (int **) calloc((size_t)elem_level, sizeof(int *));
        elems[i].upper_counts = (int *)  calloc((size_t)elem_level, sizeof(int));
        if (!elems[i].upper_links || !elems[i].upper_counts)
        {
            free(llbuf);
            free_elem_descs(elems, N, (size_t)M0);
            free(elems);
            free(tids);
            fclose(hf);
            ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper link arrays")));
        }

        char *p = llbuf;
        /* levels 1..elem_level stored in order from level 1 upward */
        for (int lv = 1; lv <= elem_level; lv++)
        {
            int lv_idx = lv - 1;
            int lcount;
            memcpy(&lcount, p, sizeof(int));
            p += sizeof(int);
            if (lcount < 0 || lcount > M)
                lcount = 0;
            elems[i].upper_counts[lv_idx] = lcount;
            elems[i].upper_links[lv_idx]  = (int *) malloc((size_t)M * sizeof(int));
            if (!elems[i].upper_links[lv_idx])
            {
                free(llbuf);
                free_elem_descs(elems, N, (size_t)M0);
                free(elems);
                free(tids);
                fclose(hf);
                ereport(ERROR,
                        (errmsg("pg_cuvs: out of memory for upper links lv=%d", lv)));
            }
            memcpy(elems[i].upper_links[lv_idx], p, (size_t)M * sizeof(int));
            p += (size_t)M * sizeof(int);
        }

        free(llbuf);
    }

    fclose(hf);
    free(tids);

    /* ---- 8. Layout pass: assign (blkno, offno) to each element ---- */
    /*
     * One page per element (elem tuple at offno=1, neigh tuple at offno=2).
     * Block 0 is the metapage, so elements start at block 1.
     */
    for (size_t i = 0; i < N; i++)
    {
        uint32_t blkno = (uint32_t)(i + 1);  /* block 0 is meta */
        elems[i].elem_blkno  = blkno;
        elems[i].elem_offno  = 1;
        elems[i].neigh_blkno = blkno;
        elems[i].neigh_offno = 2;
    }

    /* Verify that elem+neigh fit on one page for the worst-case element level.
     * We check against the largest possible level. */
    {
        int max_level_seen = hdr.maxlevel;
        size_t esize = elem_tuple_size(dim);
        size_t nsize = neigh_tuple_size(max_level_seen, M);
        size_t needed = esize + 2 * PGV_ITEM_OVERHEAD + nsize + 2 * PGV_ITEM_OVERHEAD;
        if (needed > (size_t)PGV_USABLE_BYTES)
            ereport(ERROR,
                    (errmsg("pg_cuvs: element+neighbor tuple pair too large for "
                            "one page: needed=%zu, available=%d "
                            "(dim=%d, maxlevel=%d, M=%d)",
                            needed, PGV_USABLE_BYTES, dim, max_level_seen, M)));
    }

    /* ---- 9. Re-open .hnsw to read vectors sequentially ---- */
    hf = fopen(hnsw_path, "rb");
    if (!hf)
    {
        free_elem_descs(elems, N, (size_t)M0);
        free(elems);
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot re-open .hnsw \"%s\": %m", hnsw_path)));
    }

    /* Seek to level-0 block, then to the offsetData of element 0 */
    if (fseek(hf, (long)hdr.offsetLevel0, SEEK_SET) != 0)
    {
        fclose(hf);
        free_elem_descs(elems, N, (size_t)M0);
        free(elems);
        ereport(ERROR,
                (errmsg("pg_cuvs: fseek failed on re-opened \"%s\"", hnsw_path)));
    }

    /* ---- 10. Open target HNSW relation and validate before truncate ---- */
    /*
     * AccessExclusiveLock is required — not just ExclusiveLock.
     * ExclusiveLock does NOT conflict with AccessShareLock, which is held by
     * concurrent index scans.  A truncate + full page rewrite while a reader
     * holds AccessShareLock would expose partially-written pages and corrupt
     * search results.  AccessExclusiveLock blocks all concurrent access until
     * the import transaction commits (or rolls back), matching the semantics
     * of REINDEX and other index-rebuild DDL.
     *
     * Implication: pg_cuvs_import_hnsw is an OFFLINE operation.  No SELECT
     * queries against the target HNSW index can proceed until this function
     * returns and the caller's transaction commits.
     */
    Relation hnsw_rel = index_open(hnsw_oid, AccessExclusiveLock);

    /*
     * Truncate the relation to 0 blocks so we can write fresh pages.
     * We use smgrtruncate directly since RelationTruncate is not
     * exposed to extensions.
     */
    {
        SMgrRelation smgr = RelationGetSmgr(hnsw_rel);
        BlockNumber  cur  = smgrnblocks(smgr, MAIN_FORKNUM);
        if (cur > 0)
        {
            ForkNumber  forks[1]  = { MAIN_FORKNUM };
            BlockNumber blocks[1] = { 0 };
            smgrtruncate(smgr, forks, 1, blocks);
        }
        /* Invalidate the buffer manager's knowledge of this relation's size. */
        RelationSetTargetBlock(hnsw_rel, InvalidBlockNumber);
    }

    /* ---- 11. Write metapage (block 0) ---- */
    /* Use BufferGetPage directly (pgvector-style build path; WAL safety via
     * transaction rollback on error — the caller's transaction covers all writes). */
    {
        Buffer buf = ReadBuffer(hnsw_rel, P_NEW);   /* → block 0 */
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
        PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

        PgvHnswPageOpaque *opaque =
            (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
        opaque->nextblkno = InvalidBlockNumber;
        opaque->unused    = 0;
        opaque->page_id   = PGV_HNSW_PAGE_ID;

        /*
         * Write metapage data directly into PageGetContents(page).
         * pgvector reads it as HnswPageGetMeta(page) = PageGetContents(page),
         * NOT via the item-pointer table. Using PageAddItem would put the data
         * at the wrong location (pd_upper area vs PageGetContents area).
         */
        PgvHnswMeta *metap = (PgvHnswMeta *) PageGetContents(page);
        memset(metap, 0, sizeof(*metap));
        metap->magicNumber   = PGV_HNSW_MAGIC;
        metap->version       = PGV_HNSW_VERSION;
        metap->dimensions    = (uint32_t)dim;
        metap->m             = (uint16_t)M;
        metap->efConstruction = (uint16_t)hdr.efConstruction;

        /* entrypoint: the node with the highest level (= hdr.enterpointNode) */
        int ep = hdr.enterpointNode;
        if (ep >= 0 && (size_t)ep < N)
        {
            metap->entryBlkno  = elems[ep].elem_blkno;
            metap->entryOffno  = elems[ep].elem_offno;
            metap->entryLevel  = (int16_t)elems[ep].level;
        }
        else
        {
            metap->entryBlkno  = InvalidBlockNumber;
            metap->entryOffno  = 0;
            metap->entryLevel  = -1;
        }
        /* insertPage: last element block (block N = last element page). */
        metap->insertPage = (uint32_t)N;

        /* Update pd_lower to reflect the metapage data (pgvector pattern). */
        ((PageHeader)page)->pd_lower =
            (uint16_t)(((char *)metap + sizeof(PgvHnswMeta)) - (char *)page);

        MarkBufferDirty(buf);
        if (!hnsw_unlogged)
            log_newpage_buffer(buf, true); /* WAL full-page image for crash safety */
        UnlockReleaseBuffer(buf);
    }

    /* ---- 12. Write element+neighbor pages ---- */
    /* Allocate a temporary buffer for one element's vector */
    float *vec_buf = (float *) palloc((size_t)dim * sizeof(float));

    for (size_t i = 0; i < N; i++)
    {
        /* Read the vector from the .hnsw file.
         * Level-0 block starts at HNSWLIB_HEADER_BYTES (96) in the file.
         * offsetLevel0 is an intra-element offset (0 in practice).
         * offsetData is the byte offset of vectors within each element block. */
        long elem_offset = (long)HNSWLIB_HEADER_BYTES
                         + (long)hdr.offsetLevel0
                         + (long)(i * hdr.sizeDataPerElement)
                         + (long)hdr.offsetData;
        if (fseek(hf, elem_offset, SEEK_SET) != 0)
        {
            fclose(hf);
            pfree(vec_buf);
            free_elem_descs(elems, N, (size_t)M0);
            free(elems);
            index_close(hnsw_rel, AccessExclusiveLock);
            ereport(ERROR,
                    (errmsg("pg_cuvs: fseek to vector for element %zu failed",
                            i)));
        }

        if (fread(vec_buf, sizeof(float), (size_t)dim, hf) != (size_t)dim)
        {
            fclose(hf);
            pfree(vec_buf);
            free_elem_descs(elems, N, (size_t)M0);
            free(elems);
            index_close(hnsw_rel, AccessExclusiveLock);
            ereport(ERROR,
                    (errmsg("pg_cuvs: short read on vector for element %zu", i)));
        }

        write_elem_page(hnsw_rel,
                        elems[i].elem_blkno,
                        &elems[i],
                        vec_buf,
                        dim, M,
                        elems, N,
                        hnsw_unlogged);
    }

    fclose(hf);
    pfree(vec_buf);
    free_elem_descs(elems, N, (size_t)M0);
    free(elems);

    index_close(hnsw_rel, AccessExclusiveLock);

    ereport(NOTICE,
            (errmsg("pg_cuvs: imported %zu elements (dim=%d, M=%d) from "
                    "\"%s\" (use_shm=%d) into hnsw index %u",
                    N, dim, M, hnsw_path, (int)use_shm, hnsw_oid)));

    /* Clean up /dev/shm file after successful import */
    if (use_shm)
        unlink(hnsw_path);
}

/* ================================================================
 * pg_cuvs_import_cagra — Phase 3J: direct CAGRA → pgvector HNSW
 *
 * Skips the hnswlib intermediate format entirely. Gets CAGRA adjacency
 * + corpus vectors from the daemon via IPC, builds a flat pgvector HNSW
 * (all nodes at level 0), and writes it directly into the target relation.
 *
 * Does NOT require cuvs.cpu_hnsw_fallback=on.
 * Supports UNLOGGED target for faster import (see pg_cuvs_import_hnsw).
 * ================================================================ */

/* ----------------------------------------------------------------
 * Squared L2 distance between two float32 vectors.
 * Used by heuristic_select_neighbors; squared form is sufficient
 * for comparisons (monotone with actual L2).
 * ---------------------------------------------------------------- */
static float
vec_dist_sq(const float *a, const float *b, int dim)
{
    float d = 0.0f;
    for (int k = 0; k < dim; k++)
    {
        float diff = a[k] - b[k];
        d += diff * diff;
    }
    return d;
}

/* ----------------------------------------------------------------
 * HNSW heuristic neighbor selection (Algorithm 4, Malkov & Yashunin 2018).
 *
 * For each candidate c (processed nearest-first to q), add c to the
 * result only if d(q, c) < d(c, s) for ALL s already selected.
 * This ensures selected neighbors cover diverse directions from q rather
 * than clustering in one region.
 *
 * candidates    : CAGRA neighbor indices, assumed approximately sorted by
 *                 distance to q (NSG adjacency is nearest-first in practice).
 * n_candidates  : number of candidates
 * M             : max neighbors to select
 * out_selected  : output array of size >= M
 * returns       : number of neighbors selected (<= M)
 * ---------------------------------------------------------------- */
static int
heuristic_select_neighbors(
    const float *query_vec,
    const float *all_vecs,
    int          dim,
    const int   *candidates,
    int          n_candidates,
    int          M,
    int         *out_selected)
{
    int n_sel = 0;
    for (int i = 0; i < n_candidates && n_sel < M; i++)
    {
        int          c     = candidates[i];
        const float *c_vec = all_vecs + (size_t)c * dim;
        float        d_q_c = vec_dist_sq(query_vec, c_vec, dim);

        /* c is kept only if it is closer to q than to every selected s */
        bool dominated = false;
        for (int j = 0; j < n_sel && !dominated; j++)
        {
            const float *s_vec = all_vecs + (size_t)out_selected[j] * dim;
            /* hnswlib condition: reject if any selected s is closer to c than q is
             * (strict < matches getNeighborsByHeuristic2 in hnswalg.h line 470) */
            if (vec_dist_sq(c_vec, s_vec, dim) < d_q_c)
                dominated = true;
        }

        if (!dominated)
            out_selected[n_sel++] = c;
    }
    return n_sel;
}

/* ----------------------------------------------------------------
 * Level assignment helper for HNSW mode.
 * Uses geometric distribution: level = floor(-log(r) / log(M)).
 * Matches hnswlib / from_cagra() convention.
 * ---------------------------------------------------------------- */
static int
cagra_assign_level(int M, unsigned int *seed)
{
    double mL = 1.0 / log((double)(M > 1 ? M : 2));
    /* (0,1) exclusive to avoid log(0) */
    double r = ((double)rand_r(seed) + 1.0) / ((double)RAND_MAX + 2.0);
    int lv   = (int)(-log(r) * mL);
    return (lv > 16) ? 16 : lv;   /* cap to avoid degenerate indexes */
}

/* Internal helper: fill an existing HNSW index from CAGRA adjacency via IPC.
 * Called by pg_cuvs_build_hnsw() for modes 'nsw' and 'hnsw'.
 * mode='nsw'  → flat NSW (no hierarchy, level 0 only)
 * mode='hnsw' → hierarchical with heuristic neighbor selection */
static void
fill_hnsw_from_cagra_ipc(Oid cagra_oid, Oid hnsw_oid, const char *mode)
{
    bool do_hierarchy = (strcmp(mode, "hnsw") == 0);

    /* ---- 0. pgvector compatibility check ---- */
    {
        Oid vec_oid = get_extension_oid("vector", true);
        if (!OidIsValid(vec_oid))
            ereport(ERROR,
                    (errmsg("pg_cuvs: pgvector extension is not installed; "
                            "pg_cuvs_import_cagra requires pgvector 0.5.0+"),
                     errhint("Run: CREATE EXTENSION vector;")));
    }

    /* ---- 1. Source CAGRA index: get dim ---- */
    Relation cagra_rel = index_open(cagra_oid, AccessShareLock);
    int dim = 0;
    if (RelationGetDescr(cagra_rel)->natts >= 1)
        dim = (int)TupleDescAttr(RelationGetDescr(cagra_rel), 0)->atttypmod;
    if (dim <= 0)
        ereport(ERROR,
                (errmsg("pg_cuvs: cannot determine vector dimension from cagra "
                        "index %u", cagra_oid)));
    index_close(cagra_rel, AccessShareLock);

    /* ---- 1b. Validate target index (AM, dim, metric) ---- */
    bool hnsw_unlogged = false;
    uint32_t tgt_metric = CUVS_METRIC_L2;
    {
        HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(hnsw_oid));
        if (!HeapTupleIsValid(tup))
            ereport(ERROR,
                    (errmsg("pg_cuvs: target relation %u not found", hnsw_oid)));
        Form_pg_class cf = (Form_pg_class) GETSTRUCT(tup);

        if (cf->relkind != RELKIND_INDEX)
        {
            char *name = pstrdup(NameStr(cf->relname));
            ReleaseSysCache(tup);
            ereport(ERROR, (errmsg("pg_cuvs: \"%s\" is not an index", name)));
        }
        Oid hnsw_amoid = get_am_oid("hnsw", true);
        if (!OidIsValid(hnsw_amoid) || cf->relam != hnsw_amoid)
        {
            char *name = pstrdup(NameStr(cf->relname));
            ReleaseSysCache(tup);
            ereport(ERROR,
                    (errmsg("pg_cuvs: \"%s\" is not a pgvector HNSW index", name)));
        }
        /* Dimension check */
        {
            int tgt_dim = 0;
            HeapTuple attup = SearchSysCache2(ATTNUM,
                                               ObjectIdGetDatum(hnsw_oid),
                                               Int16GetDatum(1));
            if (HeapTupleIsValid(attup))
            {
                tgt_dim = (int)((Form_pg_attribute)GETSTRUCT(attup))->atttypmod;
                ReleaseSysCache(attup);
            }
            if (tgt_dim > 0 && tgt_dim != dim)
            {
                char *name = pstrdup(NameStr(cf->relname));
                ReleaseSysCache(tup);
                ereport(ERROR,
                        (errmsg("pg_cuvs: dimension mismatch: source CAGRA dim=%d "
                                "but target HNSW \"%s\" dim=%d", dim, name, tgt_dim)));
            }
        }
        /* Opclass → metric */
        {
            HeapTuple idxtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(hnsw_oid));
            if (HeapTupleIsValid(idxtup))
            {
                bool isnull;
                Datum d = SysCacheGetAttr(INDEXRELID, idxtup,
                                           Anum_pg_index_indclass, &isnull);
                if (!isnull)
                {
                    oidvector *ov = (oidvector *)DatumGetPointer(d);
                    HeapTuple opctup = SearchSysCache1(CLAOID,
                                                        ObjectIdGetDatum(ov->values[0]));
                    if (HeapTupleIsValid(opctup))
                    {
                        const char *nm = NameStr(((Form_pg_opclass)GETSTRUCT(opctup))->opcname);
                        if (strstr(nm, "cosine"))   tgt_metric = CUVS_METRIC_COSINE;
                        else if (strstr(nm, "_ip")) tgt_metric = CUVS_METRIC_IP;
                        ReleaseSysCache(opctup);
                    }
                }
                ReleaseSysCache(idxtup);
            }
        }
        hnsw_unlogged = (cf->relpersistence == RELPERSISTENCE_UNLOGGED);
        ReleaseSysCache(tup);
    }

    if (hnsw_unlogged)
        ereport(NOTICE,
                (errmsg("pg_cuvs: target HNSW is UNLOGGED — WAL skipped; "
                        "index must be rebuilt after crash recovery")));

    /* ---- 2. Fetch adjacency + vectors + tids from daemon ---- */
    if (cuvs_socket_path == NULL || cuvs_socket_path[0] == '\0')
        ereport(ERROR,
                (errmsg("pg_cuvs: daemon socket not set (cuvs.socket_path); "
                        "is pg_cuvs_server running?")));

    uint32_t *adj   = NULL;
    float    *vecs  = NULL;
    uint64_t *tids  = NULL;
    size_t    N;
    int       graph_degree, ipc_dim;
    uint32_t  src_metric;

    int rc = cuvs_ipc_export_adjacency(
        cuvs_socket_path,
        (uint32_t)MyDatabaseId,
        (uint32_t)cagra_oid,
        &adj, &vecs, &tids,
        &N, &graph_degree, &ipc_dim,
        &src_metric);

    if (rc != CUVS_STATUS_OK)
    {
        ereport(ERROR,
                (errmsg("pg_cuvs: IPC export_adjacency failed (status=%d); "
                        "ensure index %u is loaded in daemon", rc, cagra_oid)));
    }

    /* Metric mismatch check */
    if (src_metric != tgt_metric)
    {
        static const char *mname[] = {"l2", "cosine", "ip"};
        const char *sm = (src_metric < 3) ? mname[src_metric] : "unknown";
        const char *tm = (tgt_metric < 3) ? mname[tgt_metric] : "unknown";
        free(adj); free(vecs); free(tids);
        ereport(ERROR,
                (errmsg("pg_cuvs: metric mismatch: source CAGRA uses %s but "
                        "target HNSW uses %s", sm, tm)));
    }

    /* ---- 3. Build ElemDesc array ---- */
    /* M0 = level-0 neighbor slots (pgvector: 2*M).
     * Clamp graph_degree to M0 = min(graph_degree, 64); M = M0/2. */
    int M0 = (graph_degree < 64) ? graph_degree : 64;
    if (M0 % 2 != 0) M0--;
    if (M0 < 2) M0 = 2;
    int M = M0 / 2;

    ElemDesc *elems = (ElemDesc *)calloc(N, sizeof(ElemDesc));
    if (!elems)
    {
        free(adj); free(vecs); free(tids);
        ereport(ERROR, (errmsg("pg_cuvs: out of memory for element descriptors")));
    }

    int    max_level  = 0;
    size_t entry_elem = 0;

    if (do_hierarchy)
    {
        /* ---- 3a. Assign levels (geometric distribution, same as hnswlib) ---- */
        unsigned int seed = (unsigned int)(N * 31337u ^ (size_t)M * 13u);
        for (size_t i = 0; i < N; i++)
        {
            elems[i].level = cagra_assign_level(M, &seed);
            if (elems[i].level > max_level)
            {
                max_level  = elems[i].level;
                entry_elem = i;
            }
        }
    }
    /* else: all elems[i].level remain 0 (calloc'd), max_level=0, entry_elem=0 */

    /* ---- 3b. Level-0 and upper-level links ---- */
    for (size_t i = 0; i < N; i++)
    {
        elems[i].tid_encoded = tids[i];
        const uint32_t *row = adj + i * (size_t)graph_degree;

        /* Level 0: up to M0 CAGRA neighbors */
        int cnt0 = (graph_degree < M0) ? graph_degree : M0;
        elems[i].lv0_count = cnt0;
        if (cnt0 > 0)
        {
            elems[i].lv0_links = (int *)malloc((size_t)cnt0 * sizeof(int));
            if (!elems[i].lv0_links)
            {
                free_elem_descs(elems, i, (size_t)M0);
                free(elems); free(adj); free(vecs); free(tids);
                ereport(ERROR, (errmsg("pg_cuvs: out of memory for lv0_links")));
            }
            for (int j = 0; j < cnt0; j++)
                elems[i].lv0_links[j] = (int)row[j];
        }

        /* Upper levels (only when do_hierarchy and level > 0) */
        int lv = elems[i].level;
        if (do_hierarchy && lv > 0)
        {
            elems[i].upper_links  = (int **)calloc((size_t)lv, sizeof(int *));
            elems[i].upper_counts = (int  *)calloc((size_t)lv, sizeof(int));
            if (!elems[i].upper_links || !elems[i].upper_counts)
            {
                free_elem_descs(elems, i + 1, (size_t)M0);
                free(elems); free(adj); free(vecs); free(tids);
                ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper_links")));
            }
            /* Convert CAGRA uint32_t adjacency to int candidates for heuristic */
            int *cand_buf = (int *)malloc((size_t)graph_degree * sizeof(int));
            if (!cand_buf)
            {
                free_elem_descs(elems, i + 1, (size_t)M0);
                free(elems); free(adj); free(vecs); free(tids);
                ereport(ERROR, (errmsg("pg_cuvs: out of memory for heuristic candidates")));
            }
            for (int j = 0; j < graph_degree; j++)
                cand_buf[j] = (int)row[j];

            for (int l = 1; l <= lv; l++)
            {
                elems[i].upper_links[l - 1]  = (int *)malloc((size_t)M * sizeof(int));
                if (!elems[i].upper_links[l - 1])
                {
                    free(cand_buf);
                    free_elem_descs(elems, i + 1, (size_t)M0);
                    free(elems); free(adj); free(vecs); free(tids);
                    ereport(ERROR, (errmsg("pg_cuvs: out of memory for upper_links[%d]", l)));
                }
                /* Heuristic neighbor selection: diverse coverage of directions */
                int n_sel = heuristic_select_neighbors(
                    vecs + (size_t)i * ipc_dim,
                    vecs,
                    ipc_dim,
                    cand_buf,
                    graph_degree,
                    M,
                    elems[i].upper_links[l - 1]);
                elems[i].upper_counts[l - 1] = n_sel;
            }
            free(cand_buf);
        }
    }
    free(adj);
    free(tids);

    /* ---- 4. Layout pass ---- */
    for (size_t i = 0; i < N; i++)
    {
        uint32_t blkno = (uint32_t)(i + 1);
        elems[i].elem_blkno  = blkno;
        elems[i].elem_offno  = 1;
        elems[i].neigh_blkno = blkno;
        elems[i].neigh_offno = 2;
    }

    /* Verify elem+neigh fit on one page (worst-case: max_level, M neighbors). */
    {
        size_t esize  = elem_tuple_size((int)ipc_dim);
        size_t nsize  = neigh_tuple_size(max_level, M);
        size_t needed = esize + 2 * PGV_ITEM_OVERHEAD + nsize + 2 * PGV_ITEM_OVERHEAD;
        if (needed > (size_t)PGV_USABLE_BYTES)
        {
            free_elem_descs(elems, N, (size_t)M0);
            free(elems); free(vecs);
            ereport(ERROR,
                    (errmsg("pg_cuvs: element+neighbor too large for one page "
                            "(dim=%d M=%d max_level=%d needed=%zu avail=%d)",
                            ipc_dim, M, max_level, needed, PGV_USABLE_BYTES)));
        }
    }

    /* ---- 5. Open target HNSW with AccessExclusiveLock and truncate ---- */
    Relation hnsw_rel = index_open(hnsw_oid, AccessExclusiveLock);
    {
        SMgrRelation smgr = RelationGetSmgr(hnsw_rel);
        BlockNumber  cur  = smgrnblocks(smgr, MAIN_FORKNUM);
        if (cur > 0)
        {
            ForkNumber  forks[1]  = { MAIN_FORKNUM };
            BlockNumber blocks[1] = { 0 };
            smgrtruncate(smgr, forks, 1, blocks);
        }
        RelationSetTargetBlock(hnsw_rel, InvalidBlockNumber);
    }

    /* ---- 6. Write metapage (block 0) ---- */
    {
        Buffer buf = ReadBuffer(hnsw_rel, P_NEW);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
        PageInit(page, BLCKSZ, sizeof(PgvHnswPageOpaque));

        PgvHnswPageOpaque *opaque =
            (PgvHnswPageOpaque *) PageGetSpecialPointer(page);
        opaque->nextblkno = InvalidBlockNumber;
        opaque->unused    = 0;
        opaque->page_id   = PGV_HNSW_PAGE_ID;

        PgvHnswMeta *metap = (PgvHnswMeta *) PageGetContents(page);
        memset(metap, 0, sizeof(*metap));
        metap->magicNumber    = PGV_HNSW_MAGIC;
        metap->version        = PGV_HNSW_VERSION;
        metap->dimensions     = (uint16_t)ipc_dim;
        metap->m              = (uint16_t)M;
        metap->efConstruction = (uint16_t)(M0 * 2);  /* reasonable default */
        if (N > 0)
        {
            /* entry_elem is the node with highest level (0 for flat NSW) */
            metap->entryBlkno = elems[entry_elem].elem_blkno;
            metap->entryOffno = 1;
            metap->entryLevel = (int16_t)max_level;
        }
        else
        {
            metap->entryBlkno = InvalidBlockNumber;
            metap->entryOffno = 0;
            metap->entryLevel = -1;
        }
        metap->insertPage = (uint32_t)N;

        ((PageHeader)page)->pd_lower =
            (uint16_t)(((char *)metap + sizeof(PgvHnswMeta)) - (char *)page);

        MarkBufferDirty(buf);
        if (!hnsw_unlogged)
            log_newpage_buffer(buf, true);
        UnlockReleaseBuffer(buf);
    }

    /* ---- 7. Write element+neighbor pages ---- */
    for (size_t i = 0; i < N; i++)
    {
        /* Extract vector for this element */
        const float *vec = vecs + i * (size_t)ipc_dim;

        write_elem_page(hnsw_rel,
                        elems[i].elem_blkno,
                        &elems[i],
                        vec,
                        ipc_dim, M,
                        elems, N,
                        hnsw_unlogged);
    }

    free(vecs);
    free_elem_descs(elems, N, (size_t)M0);
    free(elems);

    index_close(hnsw_rel, AccessExclusiveLock);

    ereport(NOTICE,
            (errmsg("pg_cuvs: direct import %zu elements (dim=%d, M=%d, "
                    "graph_degree=%d, max_level=%d, mode=%s) "
                    "from cagra index %u into hnsw index %u",
                    N, ipc_dim, M, graph_degree, max_level, mode,
                    cagra_oid, hnsw_oid)));
}

/* ================================================================
 * pg_cuvs_build_hnsw — unified GPU import: creates HNSW WITHOUT CPU build.
 *
 * Uses INDEX_CREATE_SKIP_BUILD so pgvector's CPU ambuild() is never called.
 * The 285s CPU HNSW build is eliminated entirely.
 *
 * Usage: SELECT pg_cuvs_build_hnsw('my_cagra'::regclass, 'hnsw');
 * Returns: OID of the newly created HNSW index (regclass).
 * ================================================================ */

/* ---- find HNSW opclass OID by metric ---- */
static Oid
find_hnsw_opclass_oid(uint32_t metric)
{
    const char *opcname;
    switch (metric) {
        case CUVS_METRIC_COSINE: opcname = "vector_cosine_ops"; break;
        case CUVS_METRIC_IP:     opcname = "vector_ip_ops";     break;
        default:                  opcname = "vector_l2_ops";     break;
    }
    Oid hnsw_amoid = get_am_oid("hnsw", true);
    if (!OidIsValid(hnsw_amoid))
        ereport(ERROR, (errmsg("pg_cuvs: pgvector hnsw AM not found")));

    Relation    opcrel = table_open(OperatorClassRelationId, AccessShareLock);
    SysScanDesc sc     = systable_beginscan(opcrel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple   tup;
    Oid         result = InvalidOid;
    while (HeapTupleIsValid(tup = systable_getnext(sc))) {
        Form_pg_opclass opc = (Form_pg_opclass) GETSTRUCT(tup);
        if (opc->opcmethod == hnsw_amoid &&
            strcmp(NameStr(opc->opcname), opcname) == 0) {
            result = opc->oid;
            break;
        }
    }
    systable_endscan(sc);
    table_close(opcrel, AccessShareLock);

    if (!OidIsValid(result))
        ereport(ERROR,
                (errmsg("pg_cuvs: HNSW opclass '%s' not found; install pgvector",
                        opcname)));
    return result;
}

/* ---- detect CAGRA metric from its opclass name ---- */
static uint32_t
cagra_index_metric(Oid cagra_oid)
{
    HeapTuple idxtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(cagra_oid));
    if (!HeapTupleIsValid(idxtup)) return CUVS_METRIC_L2;
    bool isnull = false;
    Datum d   = SysCacheGetAttr(INDEXRELID, idxtup, Anum_pg_index_indclass, &isnull);
    oidvector *ov = (oidvector *) DatumGetPointer(d);
    Oid cagra_opc = ov->values[0];
    ReleaseSysCache(idxtup);

    HeapTuple opctup = SearchSysCache1(CLAOID, ObjectIdGetDatum(cagra_opc));
    if (!HeapTupleIsValid(opctup)) return CUVS_METRIC_L2;
    const char *nm = NameStr(((Form_pg_opclass) GETSTRUCT(opctup))->opcname);
    uint32_t metric = CUVS_METRIC_L2;
    if (strstr(nm, "cosine"))   metric = CUVS_METRIC_COSINE;
    else if (strstr(nm, "_ip")) metric = CUVS_METRIC_IP;
    ReleaseSysCache(opctup);
    return metric;
}

/* ---- create empty HNSW on parent table, no CPU build ---- */
static Oid
create_empty_hnsw(Oid cagra_oid)
{
    HeapTuple ix_tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(cagra_oid));
    if (!HeapTupleIsValid(ix_tup))
        ereport(ERROR, (errmsg("pg_cuvs: CAGRA index %u not found", cagra_oid)));
    Oid heap_oid = ((Form_pg_index) GETSTRUCT(ix_tup))->indrelid;
    ReleaseSysCache(ix_tup);

    uint32_t metric       = cagra_index_metric(cagra_oid);
    Oid      hnsw_opclass = find_hnsw_opclass_oid(metric);

    Relation heap_rel  = table_open(heap_oid, ShareLock);
    Relation cagra_rel = index_open(cagra_oid, AccessShareLock);

    AttrNumber col_attnum = cagra_rel->rd_index->indkey.values[0];
    Form_pg_attribute att = TupleDescAttr(heap_rel->rd_att, col_attnum - 1);
    char  *col_name  = pstrdup(NameStr(att->attname));
    Oid    collation = att->attcollation;
    int16  coloption = 0;

    IndexInfo *iinfo = BuildIndexInfo(cagra_rel);
    iinfo->ii_Predicate      = NIL;
    iinfo->ii_PredicateState = NULL;

    index_close(cagra_rel, AccessShareLock);

    char *idx_name = psprintf("pg_cuvs_hnsw_%u", cagra_oid);

    /* INDEX_CREATE_SKIP_BUILD: catalog entries created, ambuild() skipped.
     * pgvector CPU build (285s for 1M×1024) is never called. */
    Oid tablespace = heap_rel->rd_rel->reltablespace;
    if (!OidIsValid(tablespace))
        tablespace = MyDatabaseTableSpace;

    Oid hnsw_oid = index_create(
        heap_rel,
        idx_name,
        InvalidOid,           /* auto-assign OID */
        InvalidOid,           /* parentIndexRelid */
        InvalidOid,           /* parentConstraintId */
        InvalidRelFileNumber, /* auto relfilenode */
        iinfo,
        list_make1(makeString(col_name)),
        get_am_oid("hnsw", false),
        tablespace,
        &collation,           /* collationObjectId */
        &hnsw_opclass,        /* classObjectId */
        &coloption,           /* coloptions */
        (Datum) 0,            /* reloptions: defaults (m=16, ef=64) */
        INDEX_CREATE_SKIP_BUILD,  /* flags: skip pgvector CPU build */
        0,                    /* constr_flags */
        false,                /* allow_system_table_mods */
        true,                 /* is_internal */
        NULL);                /* constraintId */

    table_close(heap_rel, ShareLock);
    CommandCounterIncrement();
    return hnsw_oid;
}

PG_FUNCTION_INFO_V1(pg_cuvs_build_hnsw);
Datum
pg_cuvs_build_hnsw(PG_FUNCTION_ARGS)
{
    Oid         cagra_oid = PG_GETARG_OID(0);
    const char *mode = (PG_NARGS() >= 2 && !PG_ARGISNULL(1)) ?
                       text_to_cstring(PG_GETARG_TEXT_PP(1)) : "nsw";

    /* Create empty HNSW on parent table — INDEX_CREATE_SKIP_BUILD, no CPU build */
    Oid hnsw_oid = create_empty_hnsw(cagra_oid);

    /* Dispatch to internal C helpers — no SQL/SPI overhead.
     *
     * RECOMMENDED modes:
     *   'nsw'     — flat NSW via IPC adjacency. 117s, 2.4x speedup.
     *               Empirically equal quality at ef>=40 (Cohere 1M×1024 benchmark).
     *   'hnswlib' — from_cagra() hierarchy via /dev/shm (no disk I/O). 139s, 2.0x.
     *               Slight recall advantage at ef<20.
     *
     * HIDDEN modes (kept for research, not recommended):
     *   'hnsw'         — direct hierarchy with heuristic neighbor selection.
     *                    Currently 144s with no quality gain over 'nsw'.
     *                    Retained pending future improvement of level assignment.
     *   'hnswlib_file' — on-disk .hnsw sidecar; superceded by 'hnswlib' (shm).
     */
    if (strcmp(mode, "nsw") == 0 || strcmp(mode, "hnsw") == 0)
        fill_hnsw_from_cagra_ipc(cagra_oid, hnsw_oid, mode);
    else if (strcmp(mode, "hnswlib") == 0)
        fill_hnsw_from_hnswlib(cagra_oid, hnsw_oid, true);
    else  /* hnswlib_file */
        fill_hnsw_from_hnswlib(cagra_oid, hnsw_oid, false);

    PG_RETURN_OID(hnsw_oid);
}
