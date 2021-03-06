/*
 * tcache.c
 *
 * Implementation of T-tree cache
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/barrier.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/pg_crc.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "pg_strom.h"

#define TCACHE_HASH_SIZE	2048
typedef struct {
	dlist_node	chain;
	pid_t		pid;
	Oid			datoid;
	Oid			reloid;
	Latch	   *latch;
} tcache_columnizer;

typedef struct {
	slock_t		lock;
	dlist_head	lru_list;		/* LRU list of tc_head */
	dlist_head	pending_list;	/* list of tc_head pending for columnization */
	dlist_head	slot[TCACHE_HASH_SIZE];

	/* properties of columnizers */
	dlist_head	inactive_list;	/* list of inactive columnizers */
	tcache_columnizer columnizers[FLEXIBLE_ARRAY_MEMBER];
} tcache_common;

/*
 * static variables
 */
static shmem_startup_hook_type shmem_startup_hook_next;
static object_access_hook_type object_access_hook_next;
static heap_page_prune_hook_type heap_page_prune_hook_next;
static tcache_common  *tc_common = NULL;
static int	num_columnizers;

/*
 * static declarations
 */
static tcache_column_store *tcache_create_column_store(tcache_head *tc_head);
static tcache_column_store *tcache_duplicate_column_store(tcache_head *tc_head,
												  tcache_column_store *tcs_old,
												  bool duplicate_toastbuf);
static tcache_column_store *tcache_get_column_store(tcache_column_store *tcs);
static void tcache_put_column_store(tcache_column_store *tcs);


static tcache_toastbuf *tcache_create_toast_buffer(Size required);
static tcache_toastbuf *tcache_duplicate_toast_buffer(tcache_toastbuf *tbuf,
													  Size required);
static tcache_toastbuf *tcache_get_toast_buffer(tcache_toastbuf *tbuf);
static void tcache_put_toast_buffer(tcache_toastbuf *tbuf);

static tcache_node *tcache_find_next_node(tcache_head *tc_head,
										  BlockNumber blkno);
static tcache_column_store *tcache_find_next_column_store(tcache_head *tc_head,
														  BlockNumber blkno);
//static tcache_node *tcache_find_prev_node(tcache_head *tc_head,
//										  BlockNumber blkno_cur);
static tcache_column_store *tcache_find_prev_column_store(tcache_head *tc_head,
														  BlockNumber blkno);

static void tcache_copy_cs_varlena(tcache_column_store *tcs_dst, int base_dst,
								   tcache_column_store *tcs_src, int base_src,
								   int attidx, int nitems);
static void tcache_rebalance_tree(tcache_head *tc_head,
								  tcache_node *tc_node,
								  tcache_node **p_upper);

static void pgstrom_wakeup_columnizer(bool wakeup_all);

/*
 * Misc utility functions
 */
static inline bool
TCacheHeadLockedByMe(tcache_head *tc_head, bool be_exclusive)
{
	if (!LWLockHeldByMe(&tc_head->lwlock))
		return false;
	if (be_exclusive)
	{
		char	exclusive;

		SpinLockAcquire(&tc_head->lwlock.mutex);
		exclusive = tc_head->lwlock.exclusive;
		SpinLockRelease(&tc_head->lwlock.mutex);

		if (exclusive == 0)
			return false;
	}
	return true;
}

/*
 * NOTE: we usually put NULLs on 'prev' and 'next' of dlist_node to
 * mark this node is not linked.
 */
#define dnode_is_linked(dnode)		(!(dnode)->prev || !(dnode)->next)

static inline int
tcache_hash_index(Oid datoid, Oid reloid)
{
	pg_crc32	crc;

	INIT_CRC32(crc);
	COMP_CRC32(crc, &datoid, sizeof(Oid));
	COMP_CRC32(crc, &reloid, sizeof(Oid));
	FIN_CRC32(crc);

	return crc % TCACHE_HASH_SIZE;
}

static inline void
memswap(void *x, void *y, Size len)
{
	union {
		cl_uchar	v_uchar;
		cl_ushort	v_ushort;
		cl_uint		v_uint;
		cl_ulong	v_ulong;
		char		v_misc[32];	/* our usage is up to 32bytes right now */
	} temp;

	switch (len)
	{
		case sizeof(cl_uchar):
			temp.v_uchar = *((cl_uchar *) x);
			*((cl_uchar *) x) = *((cl_uchar *) y);
			*((cl_uchar *) y) = temp.v_uchar;
			break;
		case sizeof(cl_ushort):
			temp.v_ushort = *((cl_ushort *) x);
			*((cl_ushort *) x) = *((cl_ushort *) y);
			*((cl_ushort *) y) = temp.v_ushort;
			break;
		case sizeof(cl_uint):
			temp.v_uint = *((cl_uint *) x);
			*((cl_uint *) x) = *((cl_uint *) y);
			*((cl_uint *) y) = temp.v_uint;
			break;
		case sizeof(cl_ulong):
			temp.v_ulong = *((cl_ulong *) x);
			*((cl_ulong *) x) = *((cl_ulong *) y);
			*((cl_ulong *) y) = temp.v_ulong;
			break;
		default:
			Assert(len < sizeof(temp.v_misc));
			memcpy(temp.v_misc, x, len);
			memcpy(x, y, len);
			memcpy(y, temp.v_misc, len);
			break;
	}
}

static inline void
bitswap(uint8 *bitmap, int x, int y)
{
	bool	temp;

	temp = (bitmap[x / BITS_PER_BYTE] & (1 << (x % BITS_PER_BYTE))) != 0;

	if ((bitmap[y / BITS_PER_BYTE] &  (1 << (y % BITS_PER_BYTE))) != 0)
		bitmap[x / BITS_PER_BYTE] |=  (1 << (x % BITS_PER_BYTE));
	else
		bitmap[x / BITS_PER_BYTE] &= ~(1 << (x % BITS_PER_BYTE));

	if (temp)
		bitmap[y / BITS_PER_BYTE] |=  (1 << (y % BITS_PER_BYTE));
	else
		bitmap[y / BITS_PER_BYTE] &= ~(1 << (y % BITS_PER_BYTE));
}

/* almost same memcpy but use fast path if small data type */
static inline void
memcopy(void *dest, void *source, Size len)
{
	switch (len)
	{
		case sizeof(cl_uchar):
			*((cl_uchar *) dest) = *((cl_uchar *) source);
			break;
		case sizeof(cl_ushort):
			*((cl_ushort *) dest) = *((cl_ushort *) source);
			break;
		case sizeof(cl_uint):
			*((cl_uint *) dest) = *((cl_uint *) source);
			break;
		case sizeof(cl_ulong):
			*((cl_ulong *) dest) = *((cl_ulong *) source);
			break;
		case sizeof(ItemPointerData):
			*((ItemPointerData *) dest) = *((ItemPointerData *) source);
			break;
		default:
			memcpy(dest, source, len);
			break;
	}
}

static inline void
bitmapcopy(uint8 *dstmap, int dindex, uint8 *srcmap, int sindex, int nbits)
{
	int		width = sizeof(Datum) * BITS_PER_BYTE;
	uint8  *temp;
	Datum  *dst;
	Datum  *src;
	int		dmod;
	int		smod;
	int		i, j;

	/* adjust alignment (destination) */
	temp = dstmap + dindex / BITS_PER_BYTE;
	dst = (Datum *)TYPEALIGN_DOWN(sizeof(Datum), temp);
	dmod = ((uintptr_t)temp -
			(uintptr_t)dst) * BITS_PER_BYTE + dindex % BITS_PER_BYTE;
	Assert(dmod < width);

	/* adjust alignment (source) */
	temp = srcmap + sindex / BITS_PER_BYTE;
	src = (Datum *)TYPEALIGN_DOWN(sizeof(Datum), temp);
	smod = ((uintptr_t)temp -
			(uintptr_t)src) * BITS_PER_BYTE + sindex % BITS_PER_BYTE;
	Assert(smod < width);

	/* ok, copy the bitmap */
	for (i=0, j=0; j < nbits; i++, j += (i==0 ? width - dmod : width))
	{
		Datum	mask1 = (1UL << dmod) - 1;	/* first dmod bits are 1 */
		Datum	mask2 = (j + width < nbits ? 0 : ~((1UL << (nbits - j)) - 1));
		Datum	bitmap = 0;

		/* mask1 set 1 on the first lower 'dmod' bits */
		if (i==0)
			bitmap |= dst[i] & mask1;
		else
			bitmap |= (src[i-1] >> (width - dmod)) & mask1;

		if (smod > dmod)
			bitmap |= (src[i] >> (smod - dmod)) & ~mask1;
		else
			bitmap |= (src[i] << (dmod - smod)) & ~mask1;

		dst[i] = (bitmap & ~mask2) | (dst[i] & mask2);
	}
}








/*
 * 
 *
 */
static tcache_column_store *
tcache_create_column_store(tcache_head *tc_head)
{
	Form_pg_attribute attr;
	tcache_column_store *tcs;
	Size	length;
	Size	offset;
	int		i, j;

	/* estimate length of column store */
	length = MAXALIGN(offsetof(tcache_column_store, cdata[tc_head->ncols]));
	length += MAXALIGN(sizeof(ItemPointerData) * NUM_ROWS_PER_COLSTORE);
	length += MAXALIGN(sizeof(HeapTupleHeaderData) * NUM_ROWS_PER_COLSTORE);

	for (i=0; i < tc_head->ncols; i++)
	{
		j = tc_head->i_cached[i];

		Assert(j >= 0 && j < tc_head->tupdesc->natts);
		attr = tc_head->tupdesc->attrs[j];
		if (!attr->attnotnull)
			length += MAXALIGN(NUM_ROWS_PER_COLSTORE / BITS_PER_BYTE);
		length += MAXALIGN((attr->attlen > 0
							? attr->attlen
							: sizeof(cl_uint)) * NUM_ROWS_PER_COLSTORE);
	}

	/* OK, allocate it */
	tcs = pgstrom_shmem_alloc(length);
	if (!tcs)
		elog(ERROR, "out of shared memory");
	memset(tcs, 0, sizeof(tcache_column_store));

	tcs->stag = StromTag_TCacheColumnStore;
	SpinLockInit(&tcs->refcnt_lock);
	tcs->refcnt = 1;
	tcs->ncols = tc_head->ncols;

	offset = MAXALIGN(offsetof(tcache_column_store,
							   cdata[tcs->ncols]));
	/* array of item-pointers */
	tcs->ctids = (ItemPointerData *)((char *)tcs + offset);
	offset += MAXALIGN(sizeof(ItemPointerData) *
					   NUM_ROWS_PER_COLSTORE);
	/* array of other system columns */
	tcs->theads = (HeapTupleHeaderData *)((char *)tcs + offset);
	offset += MAXALIGN(sizeof(HeapTupleHeaderData) *
					   NUM_ROWS_PER_COLSTORE);
	/* array of user defined columns */
	for (i=0; i < tc_head->ncols; i++)
	{
		j = tc_head->i_cached[i];

		Assert(j >= 0 && j < tc_head->tupdesc->natts);
		attr = tc_head->tupdesc->attrs[j];
		if (attr->attnotnull)
			tcs->cdata[i].isnull = NULL;
		else
		{
			tcs->cdata[i].isnull = (uint8 *)((char *)tcs + offset);
			offset += MAXALIGN(NUM_ROWS_PER_COLSTORE / BITS_PER_BYTE);
		}
		tcs->cdata[i].values = ((char *)tcs + offset);
		offset += MAXALIGN((attr->attlen > 0
							? attr->attlen
							: sizeof(cl_uint)) * NUM_ROWS_PER_COLSTORE);
		tcs->cdata[i].toast = NULL;	/* to be set later on demand */
	}
	Assert(offset == length);

	return tcs;
}

static tcache_column_store *
tcache_duplicate_column_store(tcache_head *tc_head,
							  tcache_column_store *tcs_old,
							  bool duplicate_toastbuf)
{
	tcache_column_store *tcs_new = tcache_create_column_store(tc_head);
	int		nrows = tcs_old->nrows;
	int		i, j;

	PG_TRY();
	{
		memcpy(tcs_new->ctids,
			   tcs_old->ctids,
			   sizeof(ItemPointerData) * nrows);
		memcpy(tcs_new->theads,
			   tcs_old->theads,
			   sizeof(HeapTupleHeaderData) * nrows);
		for (i=0; i < tcs_old->ncols; i++)
		{
			Form_pg_attribute	attr;

			j = tc_head->i_cached[i];
			attr = tc_head->tupdesc->attrs[j];

			if (!attr->attnotnull)
			{
				Assert(tcs_new->cdata[i].isnull != NULL);
				memcpy(tcs_new->cdata[i].isnull,
					   tcs_old->cdata[i].isnull,
					   (nrows + BITS_PER_BYTE - 1) / BITS_PER_BYTE);
			}

			if (attr->attlen > 0)
			{
				memcpy(tcs_new->cdata[i].values,
					   tcs_old->cdata[i].values,
					   attr->attlen * nrows);
			}
			else if (!duplicate_toastbuf)
			{
				memcpy(tcs_new->cdata[i].values,
					   tcs_old->cdata[i].values,
					   sizeof(cl_uint) * nrows);
				tcs_new->cdata[i].toast
					= tcache_get_toast_buffer(tcs_old->cdata[i].toast);
			}
			else
			{
				Size	tbuf_length = tcs_old->cdata[i].toast->tbuf_length;

				tcs_new->cdata[i].toast
					= tcache_duplicate_toast_buffer(tcs_old->cdata[i].toast,
													tbuf_length);
			}
		}
	}
	PG_CATCH();
	{
		tcache_put_column_store(tcs_new);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return tcs_new;
}

static tcache_column_store *
tcache_get_column_store(tcache_column_store *tcs)
{
	SpinLockAcquire(&tcs->refcnt_lock);
	Assert(tcs->refcnt > 0);
	tcs->refcnt++;
	SpinLockRelease(&tcs->refcnt_lock);

	return tcs;
}

static void
tcache_put_column_store(tcache_column_store *tcs)
{
	bool	do_release = false;
	int		i;

	SpinLockAcquire(&tcs->refcnt_lock);
	Assert(tcs->refcnt > 0);
	if (--tcs->refcnt == 0)
		do_release = true;
	SpinLockRelease(&tcs->refcnt_lock);

	if (do_release)
	{
		for (i=0; i < tcs->ncols; i++)
		{
			if (tcs->cdata[i].toast)
				tcache_put_toast_buffer(tcs->cdata[i].toast);
		}
		pgstrom_shmem_free(tcs);
	}
}

/*
 * create, duplicate, get and put of toast_buffer
 */
static tcache_toastbuf *
tcache_create_toast_buffer(Size required)
{
	tcache_toastbuf *tbuf;
	Size		allocated;

	required = Max(required, TCACHE_TOASTBUF_INITSIZE);

	tbuf = pgstrom_shmem_alloc_alap(required, &allocated);
	if (!tbuf)
		elog(ERROR, "out of shared memory");

	SpinLockInit(&tbuf->refcnt_lock);
	tbuf->refcnt = 1;
	tbuf->tbuf_length = allocated;
	tbuf->tbuf_usage = offsetof(tcache_toastbuf, data[0]);
	tbuf->tbuf_junk = 0;

	return tbuf;
}

static tcache_toastbuf *
tcache_duplicate_toast_buffer(tcache_toastbuf *tbuf_old, Size required)
{
	tcache_toastbuf *tbuf_new;

	Assert(required >= tbuf_old->tbuf_usage);

	tbuf_new = tcache_create_toast_buffer(required);
	memcpy(tbuf_new->data,
		   tbuf_old->data,
		   tbuf_old->tbuf_usage - offsetof(tcache_toastbuf, data[0]));
	tbuf_new->tbuf_usage = tbuf_old->tbuf_usage;
	tbuf_new->tbuf_junk = tbuf_old->tbuf_junk;

	return tbuf_new;
}

static tcache_toastbuf *
tcache_get_toast_buffer(tcache_toastbuf *tbuf)
{
	SpinLockAcquire(&tbuf->refcnt_lock);
	Assert(tbuf->refcnt > 0);
	tbuf->refcnt++;
	SpinLockRelease(&tbuf->refcnt_lock);

	return tbuf;
}

static void
tcache_put_toast_buffer(tcache_toastbuf *tbuf)
{
	bool	do_release = false;

	SpinLockAcquire(&tbuf->refcnt_lock);
	Assert(tbuf->refcnt > 0);
	if (--tbuf->refcnt == 0)
		do_release = true;
	SpinLockRelease(&tbuf->refcnt_lock);

	if (do_release)
		pgstrom_shmem_free(tbuf);
}






/*
 * tcache_create_row_store
 *
 *
 *
 *
 */
tcache_row_store *
tcache_create_row_store(TupleDesc tupdesc, int ncols, AttrNumber *i_cached)
{
	tcache_row_store   *trs;
	int			i, j;

	trs = pgstrom_shmem_alloc(ROWSTORE_DEFAULT_SIZE);
	if (!trs)
		elog(ERROR, "out of shared memory");

	/*
	 * We put header portion of kern_column_store next to the kern_row_store
	 * as source of copy for in-kernel column store. It has offset of column
	 * array, but contents shall be set up by kernel prior to evaluation of
	 * qualifier expression.
	 */
	trs->stag = StromTag_TCacheRowStore;
	SpinLockInit(&trs->refcnt_lock);
	trs->refcnt = 1;
	memset(&trs->chain, 0, sizeof(dlist_node));
	trs->usage
		= STROMALIGN_DOWN(ROWSTORE_DEFAULT_SIZE -
						  STROMALIGN(offsetof(kern_column_store,
											  colmeta[ncols])) -
						  offsetof(tcache_row_store, kern));
	trs->blkno_max = 0;
	trs->blkno_min = MaxBlockNumber;
	trs->kern.length = trs->usage;
	trs->kern.ncols = tupdesc->natts;
	trs->kern.nrows = 0;
	trs->kcs_head = (kern_column_store *)
		((char *)&trs->kern + trs->kern.length);

	/* construct colmeta structure */
	for (i=0, j=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		kern_colmeta	colmeta;

		memset(&colmeta, 0, sizeof(kern_colmeta));
		if (i == i_cached[j])
			colmeta.flags |= KERN_COLMETA_ATTREFERENCED;
		if (attr->attnotnull)
			colmeta.flags |= KERN_COLMETA_ATTNOTNULL;
		if (attr->attalign == 'c')
			colmeta.attalign = sizeof(cl_char);
		else if (attr->attalign == 's')
			colmeta.attalign = sizeof(cl_short);
		else if (attr->attalign == 'i')
			colmeta.attalign = sizeof(cl_int);
		else
		{
			Assert(attr->attalign == 'd');
			colmeta.attalign = sizeof(cl_long);
		}
		colmeta.attlen = attr->attlen;
		colmeta.cs_ofs = -1;	/* to be set later */

		if (i == i_cached[j])
		{
			memcpy(&trs->kcs_head->colmeta[j],
				   &colmeta,
				   sizeof(kern_colmeta));
			j++;
		}
		memcpy(&trs->kern.colmeta[i],
			   &colmeta,
			   sizeof(kern_colmeta));
	}
	return trs;
}

tcache_row_store *
tcache_get_row_store(tcache_row_store *trs)
{
	SpinLockAcquire(&trs->refcnt_lock);
	Assert(trs->refcnt > 0);
	trs->refcnt++;
	SpinLockRelease(&trs->refcnt_lock);

	return trs;
}

void
tcache_put_row_store(tcache_row_store *trs)
{
	bool	do_release = false;

	SpinLockAcquire(&trs->refcnt_lock);
	Assert(trs->refcnt > 0);
	if (--trs->refcnt == 0)
		do_release = true;
	SpinLockRelease(&trs->refcnt_lock);

	if (do_release)
		pgstrom_shmem_free(trs);
}






/*
 * tcache_alloc_tcnode
 *
 * allocate a tcache_node according to the supplied tcache_head
 */
static tcache_node *
tcache_alloc_tcnode(tcache_head *tc_head)
{
	dlist_node	   *dnode;
	tcache_node	   *tc_node = NULL;

	SpinLockAcquire(&tc_head->lock);
	PG_TRY();
	{
		if (dlist_is_empty(&tc_head->free_list))
		{
			dlist_node *block;
			int			i;

			block = pgstrom_shmem_alloc(SHMEM_BLOCKSZ - sizeof(cl_uint));
			if (!block)
				elog(ERROR, "out of shared memory");
			dlist_push_tail(&tc_head->block_list, block);

			tc_node = (tcache_node *)(block + 1);
			for (i=0; i < TCACHE_NODE_PER_BLOCK_BARE; i++)
				dlist_push_tail(&tc_head->free_list, &tc_node[i].chain);
		}
		dnode = dlist_pop_head_node(&tc_head->free_list);
		tc_node = dlist_container(tcache_node, chain, dnode);
		memset(&tc_node, 0, sizeof(tcache_node));

		SpinLockInit(&tc_node->lock);
		tc_node->tcs = tcache_create_column_store(tc_head);
		if (!tc_node->tcs)
			elog(ERROR, "out of shared memory");
	}
	PG_CATCH();
	{
		if (tc_node)
			dlist_push_tail(&tc_head->free_list, &tc_node->chain);
		SpinLockRelease(&tc_head->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SpinLockRelease(&tc_head->lock);

	return tc_node;
}

/*
 * tcache_free_node
 *
 * release a tcache_node and detach column-store (not release it immediately,
 * because someone may copy the data)
 */
static void
tcache_free_node_nolock(tcache_head *tc_head, tcache_node *tc_node)
{
	SpinLockAcquire(&tc_node->lock);
	if (tc_node->tcs)
		tcache_put_column_store(tc_node->tcs);
	tc_node->tcs = NULL;
	SpinLockRelease(&tc_node->lock);
	dlist_push_head(&tc_head->free_list, &tc_node->chain);
}

static void
tcache_free_node_recurse(tcache_head *tc_head, tcache_node *tc_node)
{
	/* NOTE: caller must be responsible to hold tc_head->lock */

	if (tc_node->right)
		tcache_free_node_recurse(tc_head, tc_node->right);
	if (tc_node->left)
		tcache_free_node_recurse(tc_head, tc_node->left);
	tcache_free_node_nolock(tc_head, tc_node);
}

#if 0
static void
tcache_free_node(tcache_head *tc_head, tcache_node *tc_node)


{
	SpinLockAcquire(&tc_head->lock);
	tcache_free_node_nolock(tc_head, tc_node);
	SpinLockRelease(&tc_head->lock);
}
#endif

/*
 * tcache_find_next_record
 *
 * It finds a record with the least item-pointer greater than supplied
 * ctid, within a particular tcache_column_store.
 * If found, it return an index value [0 ... tcs->nrows - 1]. Elsewhere,
 * it returns a negative value.
 * Note that it may take linear time if tcache_column_store is not sorted.
 */
static int
tcache_find_next_record(tcache_column_store *tcs, ItemPointer ctid)
{
	BlockNumber	blkno_cur = ItemPointerGetBlockNumber(ctid);
	int			index = -1;

	if (tcs->nrows == 0)
		return -1;	/* no records are cached */
	if (blkno_cur > tcs->blkno_max)
		return -1;	/* ctid points higher block, so no candidate is here */

	if (tcs->is_sorted)
	{
		int		i_min = 0;
		int		i_max = tcs->nrows;

		while (i_min < i_max)
		{
			int	i_mid = (i_min + i_max) / 2;

			if (ItemPointerCompare(&tcs->ctids[i_mid], ctid) >= 0)
				i_max = i_mid;
			else
				i_min = i_mid + 1;
		}
		Assert(i_min == i_max);
		if (i_min >= 0 && i_min < tcs->nrows)
			index = i_min;
	}
	else
	{
		ItemPointerData	ip_cur;
		int		i;

		ItemPointerSet(&ip_cur, MaxBlockNumber, MaxOffsetNumber);
		for (i=0; i < tcs->nrows; i++)
		{
			if (ItemPointerCompare(&tcs->ctids[i], ctid) >= 0 &&
				ItemPointerCompare(&tcs->ctids[i], &ip_cur) < 0)
			{
				ItemPointerCopy(&tcs->ctids[i], &ip_cur);
				index = i;
			}
		}
	}
	return index;
}

/*
 * tcache_find_next_node
 * tcache_find_next_column_store
 *
 * It tried to find least column-store that can contain any records larger
 * than the supplied 'ctid'. Usually, this routine is aplied to forward scan.
 */
static void *
tcache_find_next_internal(tcache_node *tc_node, BlockNumber blkno_cur,
						  bool column_store)
{
	tcache_column_store *tcs = NULL;
	void	   *temp;

	SpinLockAcquire(&tc_node->lock);
	if (tc_node->tcs->nrows == 0)
	{
		SpinLockRelease(&tc_node->lock);
		return NULL;
	}

	if (blkno_cur > tc_node->tcs->blkno_max)
	{
		/*
		 * if current blockno is larger then or equal to the 'blkno_max',
		 * it is obvious that this node is not a one to be fetched on the
		 * next. So, we try to walk on the next right branch.
		 */
		SpinLockRelease(&tc_node->lock);

		if (!tc_node->right)
			return NULL;
		return tcache_find_next_internal(tc_node->right, blkno_cur,
										 column_store);
	}
	else if (!tc_node->left || blkno_cur >= tc_node->tcs->blkno_min)
	{
		/*
		 * Unlike above, this case is obvious that this chunk has records
		 * larger than required item-pointer.
		 */
		if (column_store)
			tcs = tcache_get_column_store(tc_node->tcs);
		SpinLockRelease(&tc_node->lock);

		return !tcs ? (void *)tc_node : (void *)tcs;
	}
	SpinLockRelease(&tc_node->lock);

	/*
	 * Even if ctid is less than ip_min and left-node is here, we need
	 * to pay attention on the case when ctid is larger than ip_max of
	 * left node tree. In this case, this tc_node shall still be a node
	 * to be fetched.
	 */
	if ((temp = tcache_find_next_internal(tc_node->left, blkno_cur,
										  column_store)) != NULL)
		return temp;

	/* if no left node is suitable, this node should be fetched */
	if (column_store)
	{
		SpinLockAcquire(&tc_node->lock);
		tcs = tcache_get_column_store(tc_node->tcs);
		SpinLockRelease(&tc_node->lock);
	}
	return !tcs ? (void *)tc_node : (void *)tcs;
}

static tcache_node *
tcache_find_next_node(tcache_head *tc_head, BlockNumber blkno)
{
	Assert(TCacheHeadLockedByMe(tc_head, false));
	if (!tc_head->tcs_root)
		return NULL;
	return tcache_find_next_internal(tc_head->tcs_root, blkno, false);
}

static tcache_column_store *
tcache_find_next_column_store(tcache_head *tc_head, BlockNumber blkno)
{
	Assert(TCacheHeadLockedByMe(tc_head, false));
	if (!tc_head->tcs_root)
		return NULL;
	return tcache_find_next_internal(tc_head->tcs_root, blkno, true);
}

/*
 * tcache_find_prev_node
 * tcache_find_prev_column_store
 *
 *
 *
 */
static void *
tcache_find_prev_internal(tcache_node *tc_node, BlockNumber blkno_cur,
						  bool column_store)
{
	tcache_column_store *tcs = NULL;
	void	   *temp;

	SpinLockAcquire(&tc_node->lock);
	if (tc_node->tcs->nrows == 0)
	{
		SpinLockRelease(&tc_node->lock);
		return NULL;
	}

	if (blkno_cur < tc_node->tcs->blkno_min)
	{
		/*
		 * it is obvious that this chunk cannot be a candidate to be
		 * fetched as previous one.
		 */
		SpinLockRelease(&tc_node->lock);

		if (!tc_node->left)
			return NULL;
		return tcache_find_prev_internal(tc_node->left, blkno_cur,
										 column_store);
	}
	else if (!tc_node->right || blkno_cur <= tc_node->tcs->blkno_max)
	{
		/*
		 * If ctid is less than ip_max but greater than or equal to ip_min,
		 * or tc_node has no left node, this node shall be fetched on the
		 * next.
		 */
		if (column_store)
			tcs = tcache_get_column_store(tc_node->tcs);
		SpinLockRelease(&tc_node->lock);

		return !tcs ? (void *)tc_node : (void *)tcs;
	}
	SpinLockRelease(&tc_node->lock);

	/*
	 * Even if ctid is less than ip_min and left-node is here, we need
	 * to pay attention on the case when ctid is larger than ip_max of
	 * left node tree. In this case, this tc_node shall still be a node
	 * to be fetched.
	 */
	if ((temp = tcache_find_prev_internal(tc_node->right, blkno_cur,
										  column_store)) != NULL)
		return temp;

	/* if no left node is suitable, this node should be fetched */
	if (column_store)
	{
		SpinLockAcquire(&tc_node->lock);
		tcs = tcache_get_column_store(tc_node->tcs);
		SpinLockRelease(&tc_node->lock);
	}
	return !tcs ? (void *)tc_node : (void *)tcs;
}

#if 0
static tcache_node *
tcache_find_prev_node(tcache_head *tc_head, BlockNumber blkno)
{
	Assert(TCacheHeadLockedByMe(tc_head, false));
	if (!tc_head->tcs_root)
		return NULL;
	return tcache_find_prev_internal(tc_head->tcs_root, blkno, false);
}
#endif

static tcache_column_store *
tcache_find_prev_column_store(tcache_head *tc_head, BlockNumber blkno)
{
	Assert(TCacheHeadLockedByMe(tc_head, false));
	if (!tc_head->tcs_root)
		return NULL;
	return tcache_find_prev_internal(tc_head->tcs_root, blkno, true);
}

/*
 * tcache_sort_tcnode
 *
 * It sorts contents of the column-store of a particular tcache_node
 * according to the item-pointers.
 */
static void
tcache_sort_tcnode_internal(tcache_head *tc_head, tcache_node *tc_node,
							tcache_column_store *tcs, int left, int right)
{
	int		li = left;
	int		ri = right;
	ItemPointer pivot = &tcs->ctids[(li + ri) / 2];

	if (left >= right)
		return;

	while (li < ri)
	{
		while (ItemPointerCompare(&tcs->ctids[li], pivot) < 0)
			li++;
		while (ItemPointerCompare(&tcs->ctids[ri], pivot) > 0)
			ri--;
		/*
		 * Swap values
		 */
		if (li < ri)
		{
			Form_pg_attribute attr;
			int		attlen;
			int		i, j;

			memswap(&tcs->ctids[li], &tcs->ctids[ri],
					sizeof(ItemPointerData));
			memswap(&tcs->theads[li], &tcs->theads[ri],
					sizeof(HeapTupleHeaderData));

			for (i=0; i < tc_head->ncols; i++)
			{
				j = tc_head->i_cached[i];
				attr = tc_head->tupdesc->attrs[j];

				attlen = (attr->attlen > 0
						  ? attr->attlen
						  : sizeof(cl_uint));
				/* isnull flags */
				if (!attr->attnotnull)
				{
					Assert(tcs->cdata[i].isnull != NULL);
					bitswap(tcs->cdata[i].isnull, li, ri);
				}
				memswap(tcs->cdata[i].values + attlen * li,
						tcs->cdata[i].values + attlen * ri,
						attlen);
			}
			li++;
			ri--;
		}
	}
	tcache_sort_tcnode_internal(tc_head, tc_node, tcs, left, li - 1);
	tcache_sort_tcnode_internal(tc_head, tc_node, tcs, ri + 1, right);
}

static void
tcache_sort_tcnode(tcache_head *tc_head, tcache_node *tc_node, bool is_inplace)
{
	tcache_column_store *tcs_new;

	if (is_inplace)
		tcs_new = tc_node->tcs;
	else
	{
		/*
		 * even if duplication mode, sort does not move varlena data on
		 * the toast buffer. So, we just reuse existing toast buffer,
		 */
		tcs_new = tcache_duplicate_column_store(tc_head, tc_node->tcs, false);
		tcache_put_column_store(tc_node->tcs);
		tc_node->tcs = tcs_new;
	}
	tcache_sort_tcnode_internal(tc_head, tc_node, tcs_new,
								0, tcs_new->nrows - 1);
	tcs_new->is_sorted = true;
}

/*
 * tcache_copy_cs_varlena
 *
 *
 *
 */
static void
tcache_copy_cs_varlena(tcache_column_store *tcs_dst, int base_dst,
					   tcache_column_store *tcs_src, int base_src,
					   int attidx, int nitems)
{
	tcache_toastbuf *tbuf_src = tcs_src->cdata[attidx].toast;
	tcache_toastbuf *tbuf_dst = tcs_dst->cdata[attidx].toast;
	cl_uint	   *src_ofs;
	cl_uint	   *dst_ofs;
	cl_uint		vpos;
	cl_uint		vsize;
	char	   *vptr;
	int			i;

	src_ofs = (cl_uint *)(tcs_src->cdata[attidx].values);
	for (i=0; i < nitems; i++)
	{
		if (src_ofs[base_src + i] == 0)
			vpos = 0;
		else
		{
			vptr = (char *)tbuf_src + src_ofs[base_src + i];
			vsize = VARSIZE(vptr);
			if (tbuf_dst->tbuf_usage + MAXALIGN(vsize) < tbuf_dst->tbuf_length)
			{
				tcs_dst->cdata[attidx].toast
					= tcache_duplicate_toast_buffer(tbuf_dst,
													2 * tbuf_dst->tbuf_length);
				tbuf_dst = tcs_dst->cdata[attidx].toast;
			}
			memcpy((char *)tbuf_dst + tbuf_dst->tbuf_usage,
				   vptr,
				   vsize);
			vpos = tbuf_dst->tbuf_usage;
			tbuf_dst->tbuf_usage += MAXALIGN(vsize);
		}
		dst_ofs = (cl_uint *)(tcs_dst->cdata[attidx].values);
		dst_ofs[base_dst + i] = vpos;
	}
}




/*
 * tcache_compaction_tcnode
 *
 *
 * NOTE: caller must hold exclusive lwlock on tc_head.
 */
static void
tcache_compaction_tcnode(tcache_head *tc_head, tcache_node *tc_node)
{
	tcache_column_store	*tcs_new;
	tcache_column_store *tcs_old = tc_node->tcs;

	Assert(TCacheHeadLockedByMe(tc_head, true));

	tcs_new = tcache_create_column_store(tc_head);
	PG_TRY();
	{
		Size	required;
		int		i, j, k;

		/* assign a toast buffer first */
		for (i=0; i < tcs_old->ncols; i++)
		{
			if (!tcs_old->cdata[i].toast)
				continue;

			required = tcs_old->cdata[i].toast->tbuf_length;
			tcs_new->cdata[i].toast = tcache_create_toast_buffer(required);
		}

		/*
		 * It ensures ip_max/ip_min shall be updated during the loop
		 * below, not a bug that miscopies min <-> max.
		 */
		tcs_new->blkno_min = tcs_old->blkno_max;
		tcs_new->blkno_max = tcs_old->blkno_min;

		/* OK, let's make it compacted */
		for (i=0, j=0; i < tcs_old->nrows; i++)
		{
			TransactionId	xmax;
			BlockNumber		blkno_cur;

			/*
			 * Once a record on the column-store is vacuumed, it will have
			 * FrozenTransactionId less than FirstNormalTransactionId.
			 * Nobody will never see the record, so we can skip it.
			 */
			xmax = HeapTupleHeaderGetRawXmax(&tcs_old->theads[i]);
			if (xmax < FirstNormalTransactionId)
				continue;

			/*
			 * Data copy
			 */
			memcopy(&tcs_new->ctids[j],
					&tcs_old->ctids[i],
					sizeof(ItemPointerData));
			memcopy(&tcs_new->theads[j],
					&tcs_old->theads[i],
					sizeof(HeapTupleHeaderData));
			blkno_cur = ItemPointerGetBlockNumber(&tcs_new->ctids[j]);
			if (blkno_cur > tcs_new->blkno_max)
				tcs_new->blkno_max = blkno_cur;
			if (blkno_cur < tcs_new->blkno_min)
				tcs_new->blkno_min = blkno_cur;

			for (k=0; k < tcs_old->ncols; k++)
			{
				int		l = tc_head->i_cached[k];
				int		attlen = tc_head->tupdesc->attrs[l]->attlen;

				/* nullmap */
				if (tcs_old->cdata[k].isnull)
					bitmapcopy(tcs_new->cdata[k].isnull, j,
							   tcs_old->cdata[k].isnull, i, 1);
				/* values */
				if (attlen > 0)
					memcopy(tcs_new->cdata[k].values + attlen * j,
							tcs_old->cdata[k].values + attlen * i,
							attlen);
				else
				{
					tcache_copy_cs_varlena(tcs_new, j,
										   tcs_old, i, k, 1);
				}
			}
			j++;
		}
		tcs_new->nrows = j;
		tcs_new->njunks = 0;
		tcs_new->is_sorted = tcs_old->is_sorted;

		Assert(tcs_old->nrows - tcs_old->njunks == tcs_new->nrows);

		/* ok, replace it */
		tc_node->tcs = tcs_new;
		tcache_put_column_store(tcs_old);

		/*
		 * TODO: how to handle the case when nrows == 0 ?
		 */
	}
	PG_CATCH();
	{
		tcache_put_column_store(tcs_new);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * tcache_try_merge_tcnode
 *
 *
 *
 */
static bool
do_try_merge_tcnode(tcache_head *tc_head,
					tcache_node *tc_parent,
					tcache_node *tc_child)	/* <- to be removed */
{
	if (tc_parent->tcs->nrows < NUM_ROWS_PER_COLSTORE / 2 &&
		tc_child->tcs->nrows  < NUM_ROWS_PER_COLSTORE / 2 &&
		(tc_parent->tcs->nrows +
		 tc_child->tcs->nrows)  < ((2 * NUM_ROWS_PER_COLSTORE) / 3))
	{
		tcache_column_store *tcs_src = tc_child->tcs;
		tcache_column_store *tcs_dst = tc_parent->tcs;
		int		base = tcs_dst->nrows;
		int		nmoved = tcs_src->nrows;
		int		i, j;

		memcpy(tcs_dst->ctids + base,
			   tcs_src->ctids,
			   sizeof(ItemPointerData) * nmoved);
		memcpy(tcs_dst->theads + base,
			   tcs_src->theads,
			   sizeof(HeapTupleHeaderData) * nmoved);
		for (i=0; i < tc_head->ncols; i++)
		{
			Form_pg_attribute	attr;

			j = tc_head->i_cached[i];
			attr = tc_head->tupdesc->attrs[j];

			/* move nullmap */
			if (!attr->attnotnull)
				bitmapcopy(tcs_dst->cdata[i].isnull, base,
						   tcs_src->cdata[i].isnull, 0,
						   nmoved);

			if (attr->attlen > 0)
			{
				memcpy(tcs_dst->cdata[i].values + attr->attlen * base,
					   tcs_src->cdata[i].values,
					   attr->attlen * nmoved);
			}
			else
			{
				tcache_copy_cs_varlena(tcs_dst, base,
									   tcs_src, 0,
									   i, nmoved);
			}
		}
		tcs_dst->nrows	+= tcs_src->nrows;
		tcs_dst->njunks	+= tcs_src->njunks;
		/* XXX - caller should set is_sorted */
		tcs_dst->blkno_max = Max(tcs_dst->blkno_max, tcs_src->blkno_max);
		tcs_dst->blkno_min = Max(tcs_dst->blkno_min, tcs_src->blkno_min);

		return true;
	}
	return false;
}

static bool
tcache_try_merge_left_recurse(tcache_head *tc_head,
							  tcache_node *tc_node,
							  tcache_node *target)
{
	if (!tc_node->left)
		return true;	/* first left-open node; that is merginable */
	else if (tcache_try_merge_left_recurse(tc_head, tc_node->left, target))
	{
		if (do_try_merge_tcnode(tc_head, target, tc_node->left))
		{
			tcache_node	*child = tc_node->left;

			Assert(!child->left);
			tc_node->left = child->right;
			tc_node->l_depth = child->r_depth + 1;
			tc_node->tcs->is_sorted = false;
			tcache_free_node_nolock(tc_head, child);
		}
	}
	return false;
}

static bool
tcache_try_merge_right_recurse(tcache_head *tc_head,
							   tcache_node *tc_node,
							   tcache_node *target)
{
	if (!tc_node->right)
		return true;	/* first right-open node; that is merginable */
	else if (tcache_try_merge_left_recurse(tc_head, tc_node->right, target))
	{
		if (do_try_merge_tcnode(tc_head, target, tc_node->right))
		{
			tcache_node	*child = tc_node->right;

			Assert(!child->right);
			tc_node->right = child->left;
			tc_node->r_depth = child->l_depth + 1;
			if (tc_node->tcs->is_sorted)
				tc_node->tcs->is_sorted = child->tcs->is_sorted;
			tcache_free_node_nolock(tc_head, child);
		}
	}
	return false;
}

static void
tcache_try_merge_recurse(tcache_head *tc_head,
						 tcache_node *tc_node,
						 tcache_node **p_upper,
						 tcache_node *l_candidate,
						 tcache_node *r_candidate,
						 tcache_node *target)
{
	if (tc_node->tcs->blkno_min > target->tcs->blkno_max)
	{
		/*
		 * NOTE: target's block-number is less than this node, so
		 * we go down the left branch. This node may be marginable
		 * if target target is right-open node, so we inform this
		 * node can be a merge candidate.
		 */
		Assert(tc_node->left != NULL);
		l_candidate = tc_node;	/* Last node that goes down left branch */
		tcache_try_merge_recurse(tc_head, tc_node->left, &tc_node->left,
								 l_candidate, r_candidate, target);
		if (!tc_node->left)
			tc_node->l_depth = 0;
		else
		{
			tc_node->l_depth = Max(tc_node->left->l_depth,
								   tc_node->left->r_depth) + 1;
			tcache_rebalance_tree(tc_head, tc_node->left, &tc_node->left);
		}
	}
	else if (tc_node->tcs->blkno_max < target->tcs->blkno_min)
	{
		/*
		 * NOTE: target's block-number is greater than this node,
		 * so we go down the right branch. This node may be marginable
		 * if target target is left-open node, so we inform this node
		 * can be a merge candidate.
		 */
		Assert(tc_node->right != NULL);
		r_candidate = tc_node;	/* Last node that goes down right branch */
		tcache_try_merge_recurse(tc_head, tc_node->right, &tc_node->right,
								 l_candidate, r_candidate, target);
		if (!tc_node->right)
			tc_node->r_depth = 0;
		else
		{
			tc_node->r_depth = Max(tc_node->right->l_depth,
								   tc_node->right->r_depth) + 1;
			tcache_rebalance_tree(tc_head, tc_node->right, &tc_node->right);
		}
	}
	else
	{
		Assert(tc_node == target);
		/* try to merge with the least greater node */
		if (tc_node->right)
			tcache_try_merge_left_recurse(tc_head, tc_node->right, target);
		/* try to merge with the greatest less node */
		if (tc_node->left)
			tcache_try_merge_right_recurse(tc_head, tc_node->left, target);

		if (!tc_node->right && l_candidate &&
			do_try_merge_tcnode(tc_head, l_candidate, tc_node))
		{
			/*
			 * try to merge with the last upper node that goes down left-
			 * branch, if target is right-open node.
			 */
			*p_upper = tc_node->left;
			tcache_free_node_nolock(tc_head, tc_node);			
		}
		else if (!tc_node->left && r_candidate &&
				 do_try_merge_tcnode(tc_head, r_candidate, tc_node))
		{
			/*
			 * try to merge with the last upper node that goes down right-
			 * branch, if target is left-open node.
			 */
			*p_upper = tc_node->right;
			tcache_free_node_nolock(tc_head, tc_node);
		}
	}
}

static void
tcache_try_merge_tcnode(tcache_head *tc_head, tcache_node *tc_node)
{
	Assert(TCacheHeadLockedByMe(tc_head, true));

	/*
	 * NOTE: no need to walk on the tree if target contains obviously
	 * large enough number of records not to be merginable
	 */
	if (tc_node->tcs->nrows < NUM_ROWS_PER_COLSTORE / 2)
	{
		tcache_try_merge_recurse(tc_head,
								 tc_head->tcs_root,
								 &tc_head->tcs_root,
								 NULL,
								 NULL,
								 tc_node);
		tcache_rebalance_tree(tc_head, tc_head->tcs_root, &tc_head->tcs_root);
	}
}

/*
 * tcache_split_tcnode
 *
 * It creates a new tcache_node and move the largest one block of the records;
 * including varlena datum.
 *
 * NOTE: caller must hold exclusive lwlock on tc_head.
 */
static void
tcache_split_tcnode(tcache_head *tc_head, tcache_node *tc_node_old)
{
	tcache_node *tc_node_new;
    tcache_column_store *tcs_new;
    tcache_column_store *tcs_old = tc_node_old->tcs;

	Assert(TCacheHeadLockedByMe(tc_head, true));

	tc_node_new = tcache_alloc_tcnode(tc_head);
	tcs_new = tc_node_new->tcs;
	PG_TRY();
	{
		Form_pg_attribute attr;
		int		nremain;
		int		nmoved;
		int		i, j;

		/* assign toast buffers first */
		for (i=0; i < tcs_old->ncols; i++)
		{
			Size	required;

			if (!tcs_old->cdata[i].toast)
				continue;

			required = tcs_old->cdata[i].toast->tbuf_length;
			tcs_new->cdata[i].toast = tcache_create_toast_buffer(required);
		}

		/*
		 * We have to sort this column-store first, if not yet.
		 * We assume this routine is called under the exclusive lock,
		 * so in-place sorting is safe.
		 */
		if (!tcs_old->is_sorted)
			tcache_sort_tcnode(tc_head, tc_node_old, true);

		/*
		 * Find number of records to be moved into the new one.
		 * Usually, a column-store being filled caches contents of
		 * multiple heap-pages. So, block-number of ip_min and ip_max
		 * should be different.
		 */
		Assert(tcs_old->blkno_min != tcs_old->blkno_max);

		for (nremain = tcs_old->nrows; nremain > 0; nremain--)
		{
			BlockNumber	blkno
				= ItemPointerGetBlockNumber(&tcs_old->ctids[nremain - 1]);

			if (blkno != tcs_old->blkno_max)
				break;
		}
		nmoved = tcs_old->nrows - nremain;
		Assert(nremain > 0 && nmoved > 0);

		/*
		 * copy item-pointers; also update ip_min/ip_max
		 */
		memcpy(tcs_new->ctids,
			   tcs_old->ctids + nremain,
			   sizeof(ItemPointerData) * nmoved);

		/*
		 * copy system columns
		 */
		memcpy(tcs_new->theads,
			   tcs_old->theads + nremain,
			   sizeof(HeapTupleHeaderData) * nmoved);

		/*
		 * copy regular columns
		 */
		for (i=0; i < tcs_old->ncols; i++)
		{
			j = tc_head->i_cached[i];
			attr = tc_head->tupdesc->attrs[j];

			/* nullmap */
			if (!attr->attnotnull)
			{
				bitmapcopy(tcs_new->cdata[i].isnull, 0,
						   tcs_old->cdata[i].isnull, nremain,
						   nmoved);
			}

			/* regular columns */
			if (attr->attlen > 0)
			{
				memcpy(tcs_new->cdata[i].values,
					   tcs_old->cdata[i].values + attr->attlen * nremain,
					   attr->attlen * nmoved);
			}
			else
			{
				tcache_copy_cs_varlena(tcs_new, 0,
									   tcs_old, nremain,
									   i, nmoved);
			}
		}
		tcs_new->nrows = nmoved;
		tcs_new->njunks = 0;
		tcs_new->is_sorted = true;
		tcs_new->blkno_min
			= ItemPointerGetBlockNumber(&tcs_new->ctids[0]);
		tcs_new->blkno_max
			= ItemPointerGetBlockNumber(&tcs_new->ctids[nmoved - 1]);
		Assert(tcs_new->blkno_min == tcs_new->blkno_max);

		/*
		 * OK, tc_node_new is ready to chain as larger half of
		 * this column-store.
		 */
		tc_node_new->right = tc_node_old->right;
		tc_node_new->r_depth = tc_node_old->r_depth;
		tc_node_old->right = tc_node_new;
		tc_node_old->r_depth = tc_node_new->r_depth + 1;

		tcs_old->nrows = nremain;
		tcs_old->blkno_max
			= ItemPointerGetBlockNumber(&tcs_old->ctids[nremain - 1]);
	}
	PG_CATCH();
	{
		tcache_free_node_nolock(tc_head, tc_node_new);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * At last, we try to remove garbages in the tc_node_old.
	 * tcache_compaction_tcnode() may cause an error, but larger half is
	 * already moved to the tc_node_new. In this case, the tree is still
	 * valid, even if tc_node_old has ideal format.
	 *
	 * XXX - it is an option to have compaction only toast-buffer,
	 * because tcache_compaction_tcnode kicks compaction on values-array
	 * also, not only toast-buffers. Usually, it may be expensive.
	 */
	tcache_compaction_tcnode(tc_head, tc_node_old);
}

/*
 * tcache_rebalance_tree
 *
 * It rebalances the t-tree structure if supplied 'tc_node' was not
 * a balanced tree.
 */
#define TCACHE_NODE_DEPTH(tc_node) \
	(!(tc_node) ? 0 : Max((tc_node)->l_depth, (tc_node)->r_depth))

static void
tcache_rebalance_tree(tcache_head *tc_head, tcache_node *tc_node,
					  tcache_node **p_upper)
{
	Assert(TCacheHeadLockedByMe(tc_head, true));

	if (tc_node->l_depth + 1 < tc_node->r_depth)
	{
		/* anticlockwise rotation */
		tcache_node *r_node = tc_node->right;

		tc_node->right = r_node->left;
		r_node->left = tc_node;

		tc_node->r_depth = TCACHE_NODE_DEPTH(tc_node->right);
		r_node->l_depth = TCACHE_NODE_DEPTH(r_node);

		*p_upper = r_node;
	}
	else if (tc_node->l_depth > tc_node->r_depth + 1)
	{
		/* clockwise rotation */
		tcache_node	*l_node = tc_node->left;

		tc_node->left = l_node->right;
		l_node->right = tc_node;

		tc_node->l_depth = TCACHE_NODE_DEPTH(tc_node->left);
		l_node->r_depth = TCACHE_NODE_DEPTH(l_node);

		*p_upper = l_node;
	}
}


/*
 * tcache_insert_tuple_row
 *
 *
 *
 *
 *
 */
bool
tcache_row_store_insert_tuple(tcache_row_store *trs, HeapTuple tuple)
{
	bool		result = false;
	cl_uint	   *tupoffset;
	Size		usage_head;
	Size		usage_tail;
	Size		required;

	required = MAXALIGN(sizeof(HeapTupleData)) + MAXALIGN(tuple->t_len);
	tupoffset = kern_rowstore_get_offset(&trs->kern);
	usage_head = ((uintptr_t)&tupoffset[trs->kern.nrows + 1] -
				  (uintptr_t)&trs->kern);
	usage_tail = trs->usage - required;
	if (usage_head < usage_tail)
	{
		rs_tuple   *rs_tup = (rs_tuple *)((char *)&trs->kern + usage_tail);

		memcpy(&rs_tup->htup, tuple, sizeof(HeapTupleData));
		rs_tup->htup.t_data = &rs_tup->data;
		memcpy(&rs_tup->data, tuple->t_data, tuple->t_len);

		tupoffset[trs->kern.nrows++] = usage_tail;
		trs->usage = usage_tail;
		if (trs->blkno_max < ItemPointerGetBlockNumber(&tuple->t_self))
			trs->blkno_max = ItemPointerGetBlockNumber(&tuple->t_self);
		if (trs->blkno_min > ItemPointerGetBlockNumber(&tuple->t_self))
			trs->blkno_min = ItemPointerGetBlockNumber(&tuple->t_self);
		result = true;
	}

	return result;
}

static void
tcache_insert_tuple_row(tcache_head *tc_head, HeapTuple tuple)
{
	tcache_row_store *trs = NULL;

	/* shared lwlock is sufficient to insert */
	Assert(TCacheHeadLockedByMe(tc_head, false));
	SpinLockAcquire(&tc_head->lock);
	PG_TRY();
	{
	retry:
		if (tc_head->trs_curr)
			trs = tcache_get_row_store(tc_head->trs_curr);
		else
		{
			tc_head->trs_curr = tcache_create_row_store(tc_head->tupdesc,
														tc_head->ncols,
														tc_head->i_cached);
			trs = tcache_get_row_store(tc_head->trs_curr);
		}

		if (!tcache_row_store_insert_tuple(trs, tuple))
		{
			/*
			 * No more space to put tuples any more. So, move this trs into
			 * columnizer pending list (if nobody do it), them retry again.
			 */
			dlist_push_head(&tc_head->trs_list, &trs->chain);
			tcache_put_row_store(trs);
			tc_head->trs_curr = trs = NULL;
			pgstrom_wakeup_columnizer(false);
			goto retry;
		}
	}
	PG_CATCH();
	{
		SpinLockRelease(&tc_head->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	SpinLockRelease(&tc_head->lock);
}

/*
 *
 *
 *
 *
 */
static bool
tcache_update_tuple_hints_rowstore(tcache_row_store *trs, HeapTuple tuple)
{
	int			index;

	if (ItemPointerGetBlockNumber(&tuple->t_self) > trs->blkno_max ||
		ItemPointerGetBlockNumber(&tuple->t_self) < trs->blkno_min)
		return false;

	for (index=0; index < trs->kern.nrows; index++)
	{
		rs_tuple   *rs_tup = kern_rowstore_get_tuple(&trs->kern, index);

		if (!rs_tup)
			continue;
		if (ItemPointerEquals(&rs_tup->htup.t_self, &tuple->t_self))
		{
			memcpy(&rs_tup->data, tuple->t_data, sizeof(HeapTupleHeaderData));
			return true;
		}
	}
	return false;
}

static void
tcache_update_tuple_hints(tcache_head *tc_head, HeapTuple tuple)
{
	BlockNumber		blkno = ItemPointerGetBlockNumber(&tuple->t_self);
	tcache_node	   *tc_node;
	bool			hit_on_tcs = false;

	Assert(TCacheHeadLockedByMe(tc_head, false));

	tc_node = tcache_find_next_node(tc_head, blkno);
	if (tc_node)
	{
		tcache_column_store *tcs;
		int		index;

		SpinLockAcquire(&tc_node->lock);
		tcs = tc_node->tcs;
		index = tcache_find_next_record(tcs, &tuple->t_self);
		if (index >= 0)
		{
			HeapTupleHeader	htup;

			Assert(index < tcs->nrows);
			htup = &tcs->theads[index];
			Assert(HeapTupleHeaderGetRawXmax(htup) < FirstNormalTransactionId);
			memcpy(htup, tuple->t_data, sizeof(HeapTupleHeaderData));
			hit_on_tcs = true;
		}
		SpinLockRelease(&tc_node->lock);
	}

	/* if no entries in column-store, try to walk on row-store */
	if (!hit_on_tcs)
	{
		dlist_iter		iter;

		SpinLockAcquire(&tc_head->lock);
		if (tc_head->trs_curr &&
			tcache_update_tuple_hints_rowstore(tc_head->trs_curr, tuple))
			goto out;

		dlist_foreach(iter, &tc_head->trs_list)
		{
			tcache_row_store   *trs
				= dlist_container(tcache_row_store, chain, iter.cur);

			if (tcache_update_tuple_hints_rowstore(trs, tuple))
				break;
		}
	out:		
		SpinLockRelease(&tc_head->lock);
	}
}

/*
 * tcache_insert_tuple
 *
 *
 *
 *
 *
 */
static void
do_insert_tuple(tcache_head *tc_head, tcache_node *tc_node, HeapTuple tuple)
{
	tcache_column_store *tcs = tc_node->tcs;
	TupleDesc	tupdesc = tc_head->tupdesc;
	Datum	   *values = alloca(sizeof(Datum) * tupdesc->natts);
	bool	   *isnull = alloca(sizeof(bool) * tupdesc->natts);
	int			i, j;

	Assert(TCacheHeadLockedByMe(tc_head, true));
	Assert(tcs->nrows < NUM_ROWS_PER_COLSTORE);
	Assert(tcs->nrows == 0 ||
		   (ItemPointerGetBlockNumber(&tuple->t_self) >= tcs->blkno_min &&
			ItemPointerGetBlockNumber(&tuple->t_self) <= tcs->blkno_max));

	heap_deform_tuple(tuple, tupdesc, values, isnull);

	/* copy system columns */
	tcs->ctids[tcs->nrows] = tuple->t_self;
	memcopy(&tcs->theads[tcs->nrows], tuple->t_data,
			sizeof(HeapTupleHeaderData));

	for (i=0; i < tcs->ncols; i++)
	{
		Form_pg_attribute	attr;

		j = tc_head->i_cached[i];
		Assert(j >= 0 && j < tupdesc->natts);
		attr = tupdesc->attrs[j];

		if (attr->attlen > 0)
		{
			/* fixed-length variable is simple to put */
			memcopy(tcs->cdata[i].values + attr->attlen * tcs->nrows,
					&values[j],
					attr->attlen);
		}
		else
		{
			/*
			 * varlena datum shall be copied into toast-buffer once,
			 * and its offset (from the head of toast-buffer) shall be
			 * put on the values array.
			 */
			tcache_toastbuf	*tbuf = tcs->cdata[i].toast;
			Size		vsize = VARSIZE_ANY(values[j]);

			if (tbuf->tbuf_usage + MAXALIGN(vsize) < tbuf->tbuf_length)
			{
				tcache_toastbuf	*tbuf_new;

				/*
				 * Needs to expand toast-buffer if no more room exist
				 * to store new varlenas. Usually, twice amount of
				 * toast buffer is best choice for buddy allocator.
				 */
				tbuf_new = tcache_create_toast_buffer(2 * tbuf->tbuf_length);
				memcpy(tbuf_new->data,
					   tbuf->data,
					   tbuf->tbuf_usage - offsetof(tcache_toastbuf, data[0]));
				tbuf_new->tbuf_usage = tbuf->tbuf_usage;
				tbuf_new->tbuf_junk = tbuf->tbuf_junk;

				/* replace older buffer by new (larger) one */
				tcache_put_toast_buffer(tbuf);
				tcs->cdata[i].toast = tbuf = tbuf_new;
			}
			Assert(tbuf->tbuf_usage + MAXALIGN(vsize) < tbuf->tbuf_length);

			((cl_uint *)tcs->cdata[i].values)[tcs->nrows] = tbuf->tbuf_usage;
			memcpy((char *)tbuf + tbuf->tbuf_usage,
				   DatumGetPointer(values[j]),
				   vsize);
			tbuf->tbuf_usage += MAXALIGN(vsize);
		}
	}

	/*
	 * update ip_max and ip_min, if needed
	 */
	if (tcs->nrows == 0)
	{
		tcs->blkno_min = ItemPointerGetBlockNumber(&tuple->t_self);
		tcs->blkno_max = ItemPointerGetBlockNumber(&tuple->t_self);
		tcs->is_sorted = true;	/* it is obviously sorted! */
	}
	else if (tcs->is_sorted)
	{
		if (ItemPointerCompare(&tuple->t_self, &tcs->ctids[tcs->nrows-1]) > 0)
			tcs->blkno_max = ItemPointerGetBlockNumber(&tuple->t_self);
		else
		{
			/*
			 * Oh... the new record placed on 'nrows' does not have
			 * largest item-pointer. It breaks the assumption; this
			 * column-store is sorted by item-pointer.
			 * It may take sorting again in the future.
			 */
			tcs->is_sorted = false;
			if (ItemPointerGetBlockNumber(&tuple->t_self) < tcs->blkno_min)
				tcs->blkno_min = ItemPointerGetBlockNumber(&tuple->t_self);
		}
	}
	else
	{
		if (ItemPointerGetBlockNumber(&tuple->t_self) > tcs->blkno_max)
			tcs->blkno_max = ItemPointerGetBlockNumber(&tuple->t_self);
		if (ItemPointerGetBlockNumber(&tuple->t_self) < tcs->blkno_min)
			tcs->blkno_min = ItemPointerGetBlockNumber(&tuple->t_self);
	}
	/* all valid, so increment nrows */
	pg_memory_barrier();
	tcs->nrows++;
}

static void
tcache_insert_tuple(tcache_head *tc_head,
					tcache_node *tc_node,
					HeapTuple tuple)
{
	tcache_column_store *tcs = tc_node->tcs;
	BlockNumber		blkno_cur = ItemPointerGetBlockNumber(&tuple->t_self);

	Assert(TCacheHeadLockedByMe(tc_head, true));

	if (tcs->nrows == 0)
	{
		do_insert_tuple(tc_head, tc_node, tuple);
		/* no rebalance is needed obviously */
		return;
	}

retry:
	if (blkno_cur < tcs->blkno_min)
	{
		if (!tc_node->left && tcs->nrows < NUM_ROWS_PER_COLSTORE)
			do_insert_tuple(tc_head, tc_node, tuple);
		else
		{
			if (!tc_node->left)
			{
				tc_node->left = tcache_alloc_tcnode(tc_head);
				tc_node->l_depth = 1;
			}
			do_insert_tuple(tc_head, tc_node->left, tuple);
			tc_node->l_depth = TCACHE_NODE_DEPTH(tc_node->left);
			tcache_rebalance_tree(tc_head, tc_node->left, &tc_node->left);
		}
	}
	else if (blkno_cur > tcs->blkno_max)
	{
		if (!tc_node->right && tcs->nrows < NUM_ROWS_PER_COLSTORE)
			do_insert_tuple(tc_head, tc_node, tuple);
		else
		{
			if (!tc_node->right)
			{
				tc_node->right = tcache_alloc_tcnode(tc_head);
				tc_node->r_depth = 1;
			}
			do_insert_tuple(tc_head, tc_node->right, tuple);
			tc_node->r_depth = TCACHE_NODE_DEPTH(tc_node->right);
			tcache_rebalance_tree(tc_head, tc_node->right, &tc_node->right);
		}
	}
	else
	{
		if (tcs->nrows < NUM_ROWS_PER_COLSTORE)
			do_insert_tuple(tc_head, tc_node, tuple);
		else
		{
			/*
			 * No more room to store new records, so we split this chunk
			 * into two portions; the largest one block shall be pushed
			 * out into a new node.
			 */
			tcache_split_tcnode(tc_head, tc_node);
			goto retry;
		}
	}
}

/*
 * tcache_build_main
 *
 * main routine to construct columnar cache. It fully scans the heap
 * and insert the record into in-memory cache structure.
 */
static void
tcache_build_main(tcache_head *tc_head, HeapScanDesc heapscan)
{
	HeapTuple	tuple;

	Assert(TCacheHeadLockedByMe(tc_head, true));

	while (true)
	{
		tuple = heap_getnext(heapscan, ForwardScanDirection);
		if (!HeapTupleIsValid(tuple))
			break;

		tcache_insert_tuple(tc_head, tc_head->tcs_root, tuple);
		tcache_rebalance_tree(tc_head,
							  tc_head->tcs_root,
							  &tc_head->tcs_root);
	}
}





tcache_scandesc *
tcache_begin_scan(Relation rel, Bitmapset *required)
{
	tcache_scandesc	   *tc_scan;
	tcache_head		   *tc_head;
	bool				has_wrlock = false;

	tc_scan = palloc0(sizeof(tcache_scandesc));
	tc_scan->rel = rel;
	tc_head = tcache_get_tchead(RelationGetRelid(rel), required, true);
	if (!tc_head)
		elog(ERROR, "out of shared memory");
	pgstrom_track_object(&tc_head->stag);
	tc_scan->tc_head = tc_head;

	LWLockAcquire(&tc_head->lwlock, LW_SHARED);
retry:
	SpinLockAcquire(&tc_head->lock);
	if (tc_head->state == TCACHE_STATE_NOT_BUILT)
	{
		if (!has_wrlock)
		{
			SpinLockRelease(&tc_head->lock);
			LWLockRelease(&tc_head->lwlock);
			LWLockAcquire(&tc_head->lwlock, LW_EXCLUSIVE);
			has_wrlock = true;
			goto retry;
		}
		tc_head->state = TCACHE_STATE_NOW_BUILD;
		SpinLockRelease(&tc_head->lock);
		tc_scan->heapscan = heap_beginscan(rel, SnapshotAny, 0, NULL);
	}
	else if (has_wrlock)
	{
		SpinLockRelease(&tc_head->lock);
		LWLockRelease(&tc_head->lwlock);
		LWLockAcquire(&tc_head->lwlock, LW_SHARED);
		has_wrlock = false;
		goto retry;
	}
	else
	{
		Assert(tc_head->state == TCACHE_STATE_READY);
		SpinLockRelease(&tc_head->lock);
	}
	return tc_scan;
}

StromTag *
tcache_scan_next(tcache_scandesc *tc_scan)
{
	tcache_head		   *tc_head = tc_scan->tc_head;
	tcache_row_store   *trs_prev;
	tcache_row_store   *trs_curr;
	dlist_node		   *dnode;

	/*
	 * NOTE: In case when tcache_head is not build yet, tc_scan will have
	 * a valid 'heapscan'. Even though it is a bit ugly design, we try to
	 * load contents of the heap once, then move to 
	 */
	if (tc_scan->heapscan)
	{
		tcache_build_main(tc_head, tc_scan->heapscan);
		heap_endscan(tc_scan->heapscan);
		tc_scan->heapscan = NULL;
	}
	/* at least, we have to hold shared-lwlock on tc_head here */
	Assert(TCacheHeadLockedByMe(tc_head, false));

	if (!tc_scan->trs_curr)
	{
		tcache_column_store *tcs_prev = tc_scan->tcs_curr;
		BlockNumber		blkno = (tcs_prev ? tcs_prev->blkno_max + 1 : 0);

		tc_scan->tcs_curr
			= tcache_find_next_column_store(tc_head, blkno);
		if (tcs_prev)
			tcache_put_column_store(tcs_prev);
		if (tc_scan->tcs_curr)
			return &tc_scan->tcs_curr->stag;
	}
	/* no column-store entries, we also walks on row-stores */
	trs_prev = tc_scan->trs_curr;

	SpinLockAcquire(&tc_head->lock);
	if (!trs_prev)
	{
		if (!dlist_is_empty(&tc_head->trs_list))
		{
			dnode = dlist_head_node(&tc_head->trs_list);
			trs_curr = dlist_container(tcache_row_store, chain, dnode);
		}
		else
			trs_curr = tc_head->trs_curr;

		if (trs_curr)
			tc_scan->trs_curr = tcache_get_row_store(trs_curr);
	}
	else if (dnode_is_linked(&trs_prev->chain) &&
			 dlist_has_next(&tc_head->trs_list, &trs_prev->chain))
	{
		dnode = dlist_next_node(&tc_head->trs_list, &trs_prev->chain);
		trs_curr = dlist_container(tcache_row_store, chain, dnode);
		tc_scan->trs_curr = tcache_get_row_store(trs_curr);
		tcache_put_row_store(trs_prev);
	}
	else
	{
		if (tc_head->trs_curr)
			tc_scan->trs_curr = tcache_get_row_store(tc_head->trs_curr);
		else
			tc_scan->trs_curr = NULL;
		tcache_put_row_store(trs_prev);
	}
	SpinLockRelease(&tc_head->lock);

	if (tc_scan->trs_curr)
		return &tc_scan->trs_curr->stag;
	return NULL;
}

StromTag *
tcache_scan_prev(tcache_scandesc *tc_scan)
{
	tcache_head		   *tc_head = tc_scan->tc_head;
	tcache_column_store *tcs_prev;
	BlockNumber		blkno;
	dlist_node	   *dnode;

	/*
	 * NOTE: In case when tcache_head is not build yet, tc_scan will have
	 * a valid 'heapscan'. Even though it is a bit ugly design, we try to
	 * load contents of the heap once, then move to 
	 */
	if (tc_scan->heapscan)
	{
		tcache_build_main(tc_head, tc_scan->heapscan);
		heap_endscan(tc_scan->heapscan);
		tc_scan->heapscan = NULL;
	}
	/* at least, we have to hold shared-lwlock on tc_head here */
	Assert(TCacheHeadLockedByMe(tc_head, false));

	if (!tc_scan->tcs_curr)
	{
		tcache_row_store *trs_prev = tc_scan->trs_curr;
		tcache_row_store *trs_curr;

		SpinLockAcquire(&tc_head->lock);
		if (!trs_prev)
		{
			if (tc_head->trs_curr)
			{
				trs_curr = tcache_get_row_store(tc_head->trs_curr);
				Assert(!dnode_is_linked(&trs_curr->chain));
			}
			else if (!dlist_is_empty(&tc_head->trs_list))
			{
				dnode = dlist_tail_node(&tc_head->trs_list);
				trs_curr = dlist_container(tcache_row_store, chain, dnode);
				trs_curr = tcache_get_row_store(trs_curr);
			}
			else
				trs_curr = NULL;
		}
		else if (!dnode_is_linked(&trs_prev->chain))
		{
			if (!dlist_is_empty(&tc_head->trs_list))
			{
				dnode = dlist_tail_node(&tc_head->trs_list);
				trs_curr = dlist_container(tcache_row_store, chain, dnode);
				trs_curr = tcache_get_row_store(trs_curr);
			}
			else
				trs_curr = NULL;
			tcache_put_row_store(trs_prev);
		}
		else
		{
			if (!dlist_has_prev(&tc_head->trs_list, &trs_prev->chain))
			{
				dnode = dlist_prev_node(&tc_head->trs_list,
										&trs_prev->chain);
				trs_curr = dlist_container(tcache_row_store, chain, dnode);
				trs_curr = tcache_get_row_store(trs_curr);
			}
			else
				trs_curr = NULL;
			tcache_put_row_store(trs_prev);
		}
		tc_scan->trs_curr = trs_curr;
		SpinLockRelease(&tc_head->lock);

		/* if we have a row-store, return it */
		if (tc_scan->trs_curr)
			return &tc_scan->trs_curr->stag;
	}
	/* if we have no row-store, we also walks on column-stores */
	tcs_prev = tc_scan->tcs_curr;

	/* it's obvious we have no more column-store in this direction */
	if (tcs_prev->blkno_min == 0)
	{
		tc_scan->tcs_curr = NULL;
		tcache_put_column_store(tcs_prev);
		return NULL;
	}
	Assert(tcs_prev->blkno_min > 0);
	blkno = (tcs_prev ? tcs_prev->blkno_min - 1 : MaxBlockNumber);
	tc_scan->tcs_curr
		= tcache_find_prev_column_store(tc_head, blkno);
	if (tcs_prev)
		tcache_put_column_store(tcs_prev);
	if (tc_scan->tcs_curr)
		return &tc_scan->tcs_curr->stag;
	return NULL;
}

void
tcache_end_scan(tcache_scandesc *tc_scan)
{
	tcache_head	   *tc_head = tc_scan->tc_head;

	/*
	 * If scan is already reached end of the relation, tc_scan->scan shall
	 * be already closed. If not, it implies scan is aborted in the middle.
	 */
	SpinLockAcquire(&tc_common->lock);
	if (tc_scan->heapscan)
	{
		/*
		 * tc_scan->heapscan should be already closed, if tcache is
		 * successfully constructed. Elsewhere, it is under construction.
		 */
		tcache_free_node_recurse(tc_head, tc_head->tcs_root);
		SpinLockRelease(&tc_common->lock);

		heap_endscan(tc_scan->heapscan);
	}
	else if (tc_head->state == TCACHE_STATE_NOW_BUILD)
	{
		/* OK, cache was successfully built */
		tc_head->state = TCACHE_STATE_READY;
		SpinLockRelease(&tc_common->lock);
	}
	else
	{
		Assert(tc_head->state == TCACHE_STATE_READY);
		SpinLockRelease(&tc_common->lock);
	}
	if (tc_scan->tcs_curr)
		tcache_put_column_store(tc_scan->tcs_curr);
	if (tc_scan->trs_curr)
		tcache_put_row_store(tc_scan->trs_curr);

	tcache_put_tchead(tc_head);
	pfree(tc_scan);
}

void
tcache_rescan(tcache_scandesc *tc_scan)
{
	tcache_head	   *tc_head = tc_scan->tc_head;

	if (tc_scan->tcs_curr)
		tcache_put_column_store(tc_scan->tcs_curr);
	tc_scan->tcs_curr = NULL;
	if (tc_scan->trs_curr)
		tcache_put_row_store(tc_scan->trs_curr);
	tc_scan->trs_curr = NULL;

	SpinLockAcquire(&tc_head->lock);
	if (tc_head->state == TCACHE_STATE_NOW_BUILD)
	{
		/* XXX - how to handle half constructed cache? */
		tcache_free_node_recurse(tc_head, tc_head->tcs_root);
	}
	SpinLockRelease(&tc_head->lock);

	if (tc_scan->heapscan)
		heap_rescan(tc_scan->heapscan, NULL);
}























/*
 * tcache_create_tchead
 *
 * It constructs an empty tcache_head that is capable to cache required
 * attributes. Usually, this routine is called by tcache_get_tchead with
 * on-demand creation. Caller has to acquire tc_common->lock on invocation.
 */
static tcache_head *
tcache_create_tchead(Oid reloid, Bitmapset *required,
					 tcache_head *tcache_old)
{
	tcache_head	   *tc_head;
	HeapTuple		reltup;
	HeapTuple		atttup;
	Form_pg_class	relform;
	TupleDesc		tupdesc;
	Size			length;
	Size			allocated;
	Bitmapset	   *tempset;
	int				i, j, k;

	/* calculation of the length */
	reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", reloid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	length = (MAXALIGN(offsetof(tcache_head, data[0])) +
			  MAXALIGN(sizeof(*tupdesc)) +
			  MAXALIGN(sizeof(Form_pg_attribute) * relform->relnatts) +
			  MAXALIGN(sizeof(FormData_pg_attribute)) * relform->relnatts +
			  MAXALIGN(sizeof(AttrNumber) * relform->relnatts));

	/* allocation of a shared memory block (larger than length) */
	tc_head = pgstrom_shmem_alloc_alap(length, &allocated);
	if (!tc_head)
		elog(ERROR, "out of shared memory");

	PG_TRY();
	{
		Size	offset = MAXALIGN(offsetof(tcache_head, data[0]));

		memset(tc_head, 0, sizeof(tcache_head));

		tc_head->stag = StromTag_TCacheHead;
		tc_head->refcnt = 1;

		LWLockInitialize(&tc_head->lwlock, 0);

		SpinLockInit(&tc_head->lock);
		tc_head->state = TCACHE_STATE_NOT_BUILT;
		dlist_init(&tc_head->free_list);
		dlist_init(&tc_head->block_list);
		dlist_init(&tc_head->pending_list);
		dlist_init(&tc_head->trs_list);
		tc_head->datoid = MyDatabaseId;
		tc_head->reloid = reloid;

		tempset = bms_copy(required);
		if (tcache_old)
		{
			for (i=0; i < tcache_old->ncols; i++)
			{
				j = (tcache_old->i_cached[i] + 1
					 - FirstLowInvalidHeapAttributeNumber);
				tempset = bms_add_member(tempset, j);
			}
		}
		tc_head->ncols = bms_num_members(tempset);
		tc_head->i_cached = (AttrNumber *)((char *)tc_head + offset);
		offset += MAXALIGN(sizeof(AttrNumber) * relform->relnatts);

		tupdesc = (TupleDesc)((char *)tc_head + offset);
		memset(tupdesc, 0, sizeof(*tupdesc));
		offset += MAXALIGN(sizeof(*tupdesc));

		tupdesc->natts = relform->relnatts;
		tupdesc->attrs = (Form_pg_attribute *)((char *)tc_head + offset);
		offset += MAXALIGN(sizeof(Form_pg_attribute) * relform->relnatts);
		tupdesc->tdtypeid = relform->reltype;
		tupdesc->tdtypmod = -1;
		tupdesc->tdhasoid = relform->relhasoids;
		tupdesc->tdrefcount = -1;

		for (i=0, j=0; i < tupdesc->natts; i++)
		{
			atttup = SearchSysCache2(ATTNUM,
									 ObjectIdGetDatum(reloid),
									 Int16GetDatum(i+1));
			if (!HeapTupleIsValid(atttup))
				elog(ERROR, "cache lookup failed for attr %d of relation %u",
					 i+1, reloid);
			tupdesc->attrs[i] = (Form_pg_attribute)((char *)tc_head + offset);
			offset += MAXALIGN(sizeof(FormData_pg_attribute));
			memcpy(tupdesc->attrs[i], GETSTRUCT(atttup),
				   sizeof(FormData_pg_attribute));

			k = ((Form_pg_attribute) GETSTRUCT(atttup))->attnum
				- FirstLowInvalidHeapAttributeNumber;
			if (bms_is_member(k, tempset))
				tc_head->i_cached[j++] = i;

			ReleaseSysCache(atttup);
		}
		Assert(offset <= length);
		Assert(tc_head->ncols == j);
		tc_head->tupdesc = tupdesc;
		bms_free(tempset);

		/* remaining area shall be used to tcache_node */
		while (offset + sizeof(tcache_node) < allocated)
		{
			tcache_node *tc_node
				= (tcache_node *)((char *)tc_head + offset);

			dlist_push_tail(&tc_head->free_list, &tc_node->chain);
			offset += MAXALIGN(sizeof(tcache_node));
		}

		/* also, allocate first empty tcache node as root */
		tc_head->tcs_root = tcache_alloc_tcnode(tc_head);
	}
	PG_CATCH();
	{
		pgstrom_shmem_free(tc_head);
		PG_RE_THROW();
	}
	PG_END_TRY();
	ReleaseSysCache(reltup);

	return tc_head;
}

static void
tcache_put_tchead_nolock(tcache_head *tc_head)
{
	/*
	 * TODO: needs to check tc_head->state.
	 * If TC_STATE_NOW_BUILD, we have to release it and revert the status
	 *
	 * Also, it has to be done prior to release locking.
	 */


	if (--tc_head->refcnt == 0)
	{
		dlist_mutable_iter iter;

		Assert(!dnode_is_linked(&tc_head->chain));
		Assert(!dnode_is_linked(&tc_head->lru_chain));

		/* release tcache_node root recursively */
		tcache_free_node_recurse(tc_head, tc_head->tcs_root);

		/* release blocks allocated for tcache_node */
		dlist_foreach_modify(iter, &tc_head->block_list)
		{
#ifdef USE_ASSERT_CHECKING
			int		i;
			tcache_node	*tc_node = (tcache_node *)(iter.cur + 1);

			/*
			 * all the blocks should be already released
			 * (to be linked at tc_head->free_list)
			 */
			for (i=0; i < TCACHE_NODE_PER_BLOCK_BARE; i++)
				Assert(dnode_is_linked(&tc_node[i].chain));
#endif
			pgstrom_shmem_free(iter.cur);
		}
		/* TODO: also check tc_nodes behind of the tc_head */

		/* also, all the row-store should be released */
		Assert(dlist_is_empty(&tc_head->trs_list));
		
		pgstrom_shmem_free(tc_head);
	}
}

void
tcache_put_tchead(tcache_head *tc_head)
{
	SpinLockAcquire(&tc_common->lock);
	tcache_put_tchead_nolock(tc_head);
	SpinLockRelease(&tc_common->lock);
}

/*
 * tcache_unlink_tchead(_nolock)
 *
 * It unlinks tcache_head from the global hash table and decrements
 * reference counter of supplied object. This routine has to be
 * called within same critical section that looked up this object
 * on the hash table.
 */
static void
tcache_unlink_tchead_nolock(tcache_head *tc_head)
{
	Assert(tc_head->chain.prev != NULL && tc_head->chain.next != NULL);
	dlist_delete(&tc_head->chain);
	dlist_delete(&tc_head->lru_chain);
	memset(&tc_head->chain, 0, sizeof(dlist_node));
	memset(&tc_head->lru_chain, 0, sizeof(dlist_node));

	tcache_put_tchead_nolock(tc_head);
}

static void
tcache_unlink_tchead(tcache_head *tc_head)
{
	/*
	 * NOTE: we need to check whether tc_head is still linked to the global
	 * hash table actually, or not. If concurrent task already unlinked,
	 * nothing to do anymore.
	 */
	SpinLockAcquire(&tc_common->lock);
	if (!tc_head->chain.prev && !tc_head->chain.next)
		tcache_unlink_tchead_nolock(tc_head);
	else
		tcache_put_tchead_nolock(tc_head);
	SpinLockRelease(&tc_common->lock);
}

tcache_head *
tcache_get_tchead(Oid reloid, Bitmapset *required,
				  bool create_on_demand)
{
	dlist_iter		iter;
	tcache_head	   *tc_head = NULL;
	tcache_head	   *tc_old = NULL;
	int				hindex = tcache_hash_index(MyDatabaseId, reloid);

	SpinLockAcquire(&tc_common->lock);
	PG_TRY();
	{
		dlist_foreach(iter, &tc_common->slot[hindex])
		{
			tcache_head	   *temp
				= dlist_container(tcache_head, chain, iter.cur);

			if (temp->datoid == MyDatabaseId &&
				temp->reloid == reloid)
			{
				Bitmapset  *tempset = bms_copy(required);
				int			i, j = 0;
				int			k, l;

				while ((i = bms_first_member(tempset)) >= 0 &&
					   j < temp->tupdesc->natts)
				{
					i += FirstLowInvalidHeapAttributeNumber;

					/* all the system attributes are cached in the default */
					if (i < 0)
						continue;

					/*
					 * whole row reference is equivalent to references to
					 * all the valid (none dropped) columns.
					 * also, row reference shall apear prior to all the
					 * regular columns because of its attribute number
					 */
					if (i == InvalidAttrNumber)
					{
						for (k=0; k < temp->tupdesc->natts; k++)
						{
							if (temp->tupdesc->attrs[k]->attisdropped)
								continue;

							l = k - FirstLowInvalidHeapAttributeNumber;
							tempset = bms_add_member(tempset, l);
						}
						continue;
					}

					/*
					 * Is this regular column cached?
					 */
					while (j < temp->ncols)
					{
						k = temp->i_cached[j];
						if (temp->tupdesc->attrs[k]->attnum != i)
							break;
						j++;
					}
				}
				bms_free(tempset);

				if (j < temp->ncols)
				{
					/*
					 * Perfect! Cache of the target relation exists and all
					 * the required columns are cached.
					 */
					temp->refcnt++;
					dlist_move_head(&tc_common->lru_list, &temp->lru_chain);
					tc_head = temp;
				}
				else
				{
					/*
					 * Elsewhere, cache exists towards the required relation
					 * but all the required columns are not cached in-memory.
					 */
					tc_old = temp;
				}
				break;
			}
		}

		if (!tc_head && create_on_demand)
		{
			tc_head = tcache_create_tchead(reloid, required, tc_old);
			if (tc_head)
			{
				/* add this tcache_head to the hash table */
				dlist_push_head(&tc_common->slot[hindex], &tc_head->chain);
				dlist_push_head(&tc_common->lru_list, &tc_head->lru_chain);

				/* also, old tcache_head is unlinked */
				if (tc_old)
				{
					dlist_delete(&tc_old->chain);
					dlist_delete(&tc_old->lru_chain);
					memset(&tc_old->chain, 0, sizeof(dlist_node));
					memset(&tc_old->lru_chain, 0, sizeof(dlist_node));
					tcache_put_tchead_nolock(tc_old);
				}
			}
		}
	}
	PG_CATCH();
	{
		SpinLockRelease(&tc_common->lock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return tc_head;
}








/*
 * pgstrom_tcache_synchronizer
 *
 *
 *
 *
 */
Datum
pgstrom_tcache_synchronizer(PG_FUNCTION_ARGS)
{
	TriggerData    *trigdata;
	HeapTuple		result = NULL;
	tcache_head	   *tc_head;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "%s: not fired by trigger manager", __FUNCTION__);

	trigdata = (TriggerData *) fcinfo->context;
	tc_head = tcache_get_tchead(RelationGetRelid(trigdata->tg_relation),
								NULL, false);
	if (!tc_head)
		return PointerGetDatum(trigdata->tg_newtuple);

	PG_TRY();
	{
		TriggerEvent	tg_event = trigdata->tg_event;

		/*
		 * TODO: it may make sense if we can add this tuple into column-
		 * store directly, in case when column-store has at least one
		 * slot to store the new tuple.
		 */
		LWLockAcquire(&tc_head->lwlock, LW_SHARED);

		if (TRIGGER_FIRED_AFTER(tg_event) &&
			TRIGGER_FIRED_FOR_ROW(tg_event) &&
			TRIGGER_FIRED_BY_INSERT(tg_event))
		{
			/* after insert for each row */
			tcache_insert_tuple_row(tc_head, trigdata->tg_trigtuple);
			result = trigdata->tg_trigtuple;
		}
		else if (TRIGGER_FIRED_AFTER(tg_event) &&
				 TRIGGER_FIRED_FOR_ROW(tg_event) &&
				 TRIGGER_FIRED_BY_UPDATE(tg_event))
		{
			/* after update for each row */
			tcache_update_tuple_hints(tc_head, trigdata->tg_trigtuple);
			tcache_insert_tuple_row(tc_head, trigdata->tg_newtuple);
			result = trigdata->tg_newtuple;
		}
		else if (TRIGGER_FIRED_AFTER(tg_event) &&
				 TRIGGER_FIRED_FOR_ROW(tg_event) &&
				 TRIGGER_FIRED_BY_DELETE(tg_event))
        {
			/* after delete for each row */
			tcache_update_tuple_hints(tc_head, trigdata->tg_trigtuple);
			result = trigdata->tg_trigtuple;
		}
		else if (TRIGGER_FIRED_AFTER(tg_event) &&
				 TRIGGER_FIRED_FOR_STATEMENT(tg_event) &&
				 TRIGGER_FIRED_BY_TRUNCATE(tg_event))
		{
			/* after truncate for statement */
			tcache_unlink_tchead(tc_head);
		}
		else
			elog(ERROR, "%s: fired on unexpected context (%08x)",
				 trigdata->tg_trigger->tgname, tg_event);
	}
	PG_CATCH();
	{
		LWLockRelease(&tc_head->lwlock);
		tcache_put_tchead(tc_head);
		PG_RE_THROW();
	}
	PG_END_TRY();
	LWLockRelease(&tc_head->lwlock);
	tcache_put_tchead(tc_head);

	PG_RETURN_POINTER(result);
}
PG_FUNCTION_INFO_V1(pgstrom_tcache_synchronizer);

/*
 *
 *
 *
 *
 *
 *
 */
static void
pgstrom_assign_synchronizer(Oid reloid)
{
	Relation	class_rel;
	Relation	tgrel;
	ScanKeyData	skey;
	SysScanDesc	sscan;
	Form_pg_class class_form;
	Datum		values[Natts_pg_trigger];
	bool		isnull[Natts_pg_trigger];
	HeapTuple	tuple;
	HeapTuple	tgtup;
	Oid			funcoid;
	Oid			tgoid;
	ObjectAddress myself;
	ObjectAddress referenced;
	const char *funcname = "pgstrom_tcache_synchronizer";
	const char *tgname_s = "pgstrom_tcache_sync_stmt";
	const char *tgname_r = "pgstrom_tcache_sync_row";

	/*
	 * Fetch a relation tuple (probably) needs to be updated
	 *
	 * TODO: add description the reason why to use inplace_update
	 *
	 *
	 */
	class_rel = heap_open(RelationRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(reloid));

	sscan = systable_beginscan(class_rel, ClassOidIndexId, true,
							   SnapshotSelf, 1, &skey);
	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "catalog lookup failed for relation %u", reloid);
	class_form = (Form_pg_class) GETSTRUCT(tuple);

	/* only regular (none-toast) relation has synchronizer */
	if (class_form->relkind != RELKIND_RELATION)
		goto skip_make_trigger;
	/* we don't support synchronizer on system tables */
	if (class_form->relnamespace == PG_CATALOG_NAMESPACE)
		goto skip_make_trigger;

	/* OK, this relation should have tcache synchronizer */


	/* Lookup synchronizer function */
	funcoid = GetSysCacheOid3(PROCNAMEARGSNSP,
							  PointerGetDatum(funcname),
							  PointerGetDatum(buildoidvector(NULL, 0)),
							  ObjectIdGetDatum(PG_PUBLIC_NAMESPACE));
	if (!OidIsValid(funcoid))
		elog(ERROR, "cache lookup failed for trigger function: %s", funcname);

	/*
	 * OK, let's construct trigger definitions
	 */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * construct a tuple of statement level synchronizer
	 */
	memset(isnull, 0, sizeof(isnull));
	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(reloid);
	values[Anum_pg_trigger_tgname - 1]
		= DirectFunctionCall1(namein, CStringGetDatum(tgname_s));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_trigger_tgtype - 1]
		= Int16GetDatum(TRIGGER_TYPE_TRUNCATE);
	values[Anum_pg_trigger_tgenabled - 1]
		= CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);
	values[Anum_pg_trigger_tgisinternal - 1] = BoolGetDatum(true);
	values[Anum_pg_trigger_tgconstrrelid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_trigger_tgconstrindid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_trigger_tgconstraint - 1] = ObjectIdGetDatum(InvalidOid);
	/*
	 * XXX - deferrable trigger may make sense for cache invalidation
	 * because transaction might be aborted later, In this case, it is
	 * waste of time to re-construct columnar-cache again.
	 */
	values[Anum_pg_trigger_tgdeferrable - 1] = BoolGetDatum(false);
	values[Anum_pg_trigger_tginitdeferred - 1] = BoolGetDatum(false);

	values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
	values[Anum_pg_trigger_tgargs - 1]
		= DirectFunctionCall1(byteain, CStringGetDatum(""));
	values[Anum_pg_trigger_tgattr - 1]
		= PointerGetDatum(buildint2vector(NULL, 0));
	isnull[Anum_pg_trigger_tgqual - 1] = true;

	tgtup = heap_form_tuple(tgrel->rd_att, values, isnull);
	tgoid = simple_heap_insert(tgrel, tgtup);
	CatalogUpdateIndexes(tgrel, tgtup);

	/* record dependency on the statement-level trigger */
	myself.classId = TriggerRelationId;
	myself.objectId = tgoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	referenced.classId = RelationRelationId;
	referenced.objectId = reloid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	heap_freetuple(tgtup);

	/*
	 * also, a tuple for row-level synchronizer
	 */
	values[Anum_pg_trigger_tgname - 1]
		= DirectFunctionCall1(namein, CStringGetDatum(tgname_r));
	values[Anum_pg_trigger_tgtype - 1]
		= Int16GetDatum(TRIGGER_TYPE_ROW |
						TRIGGER_TYPE_INSERT |
						TRIGGER_TYPE_DELETE |
						TRIGGER_TYPE_UPDATE);
	tgtup = heap_form_tuple(tgrel->rd_att, values, isnull);
	tgoid = simple_heap_insert(tgrel, tgtup);
	CatalogUpdateIndexes(tgrel, tgtup);

	/* record dependency on the row-level trigger */
	myself.classId = TriggerRelationId;
	myself.objectId = tgoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	referenced.classId = RelationRelationId;
	referenced.objectId = reloid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	heap_freetuple(tgtup);

	heap_close(tgrel, NoLock);

	/*
	 * We also need to put a flag of 'relhastriggers'. This is new relation
	 * uncommitted, so it is obvious that nobody touched this catalog.
	 * So, we can apply heap_inplace_update(), instead of the regular
	 * operations.
	 */
	if (!class_form->relhastriggers)
	{
		class_form->relhastriggers = true;
		heap_inplace_update(class_rel, tuple);
		CatalogUpdateIndexes(class_rel, tuple);
	}
skip_make_trigger:
	systable_endscan(sscan);
	heap_close(class_rel, NoLock);
}

/*
 * pgstrom_relation_has_synchronizer
 *
 * A table that can have columnar-cache also needs to have trigger to
 * synchronize the in-memory cache and heap. It returns true, if supplied
 * relation has triggers that invokes pgstrom_tcache_synchronizer on
 * appropriate context.
 */
bool
pgstrom_relation_has_synchronizer(Relation rel)
{
	int		i, numtriggers;
	bool	has_on_insert_synchronizer = false;
	bool	has_on_update_synchronizer = false;
	bool	has_on_delete_synchronizer = false;
	bool	has_on_truncate_synchronizer = false;

	if (!rel->trigdesc)
		return false;

	numtriggers = rel->trigdesc->numtriggers;
	for (i=0; i < numtriggers; i++)
	{
		Trigger	   *trig = rel->trigdesc->triggers + i;
		HeapTuple	tup;

		if (!trig->tgenabled)
			continue;

		tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(trig->tgfoid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for function %u", trig->tgfoid);

		if (((Form_pg_proc) GETSTRUCT(tup))->prolang == ClanguageId)
		{
			Datum		value;
			bool		isnull;
			char	   *prosrc;
			char	   *probin;

			value = SysCacheGetAttr(PROCOID, tup,
									Anum_pg_proc_prosrc, &isnull);
			if (isnull)
				elog(ERROR, "null prosrc for C function %u", trig->tgoid);
			prosrc = TextDatumGetCString(value);

			value = SysCacheGetAttr(PROCOID, tup,
									Anum_pg_proc_probin, &isnull);
			if (isnull)
				elog(ERROR, "null probin for C function %u", trig->tgoid);
			probin = TextDatumGetCString(value);

			if (strcmp(prosrc, "pgstrom_tcache_synchronizer") == 0 &&
				strcmp(probin, "$libdir/cache_scan") == 0)
			{
				int16       tgtype = trig->tgtype;

				if (TRIGGER_TYPE_MATCHES(tgtype,
										 TRIGGER_TYPE_ROW,
										 TRIGGER_TYPE_AFTER,
										 TRIGGER_TYPE_INSERT))
					has_on_insert_synchronizer = true;
				if (TRIGGER_TYPE_MATCHES(tgtype,
										 TRIGGER_TYPE_ROW,
										 TRIGGER_TYPE_AFTER,
										 TRIGGER_TYPE_UPDATE))
					has_on_update_synchronizer = true;
				if (TRIGGER_TYPE_MATCHES(tgtype,
										 TRIGGER_TYPE_ROW,
										 TRIGGER_TYPE_AFTER,
										 TRIGGER_TYPE_DELETE))
					has_on_delete_synchronizer = true;
				if (TRIGGER_TYPE_MATCHES(tgtype,
										 TRIGGER_TYPE_STATEMENT,
										 TRIGGER_TYPE_AFTER,
										 TRIGGER_TYPE_TRUNCATE))
					has_on_truncate_synchronizer = true;
			}
			pfree(prosrc);
			pfree(probin);
		}
		ReleaseSysCache(tup);
	}

	if (has_on_insert_synchronizer &&
		has_on_update_synchronizer &&
		has_on_delete_synchronizer &&
		has_on_truncate_synchronizer)
		return true;
	return false;
}

/*
 * tcache_vacuum_heappage_column
 *
 * callback function for each heap-page. Caller should already acquires
 * shared-lock on the tcache_head, so it is prohibited to modify tree-
 * structure. All we can do is mark particular records as junk.
 */
static void
tcache_vacuum_column_store(tcache_head *tc_head, Buffer buffer)
{
	BlockNumber		blknum = BufferGetBlockNumber(buffer);
	OffsetNumber	offnum = InvalidOffsetNumber;
	Page			page = BufferGetPage(buffer);
	ItemPointerData	ctid;
	ItemId			itemid;
	tcache_node	   *tc_node;
	tcache_column_store *tcs;
	int				index;

	tc_node = tcache_find_next_node(tc_head, blknum);
	if (!tc_node)
		return;

	SpinLockAcquire(&tc_node->lock);
	if (!tc_node->tcs->is_sorted)
		tcache_sort_tcnode(tc_head, tc_node, false);
	tcs = tcache_get_column_store(tc_node->tcs);
	Assert(tcs->is_sorted);

	ItemPointerSet(&ctid, blknum, FirstOffsetNumber);
	index = tcache_find_next_record(tcs, &ctid);
	if (index < 0)
	{
		tcache_put_column_store(tcs);
		SpinLockRelease(&tc_node->lock);
		return;
	}

	while (index < tcs->nrows &&
		   ItemPointerGetBlockNumber(&tcs->ctids[index]) == blknum)
	{
		offnum = ItemPointerGetOffsetNumber(&tcs->ctids[index]);
		itemid = PageGetItemId(page, offnum);

		if (!ItemIdIsNormal(itemid))
		{
			/* find an actual item-pointer, if redirected */
			while (ItemIdIsRedirected(itemid))
				itemid = PageGetItemId(page, ItemIdGetRedirect(itemid));

			if (ItemIdIsNormal(itemid))
			{
				/* needs to update item-pointer */
				ItemPointerSetOffsetNumber(&tcs->ctids[index],
										   ItemIdGetOffset(itemid));
				/*
				 * if this offset update breaks pre-sorted array,
				 * we have to set is_sorted = false;
				 */
				if (tcs->is_sorted &&
					((index > 0 &&
					  ItemPointerCompare(&tcs->ctids[index - 1],
										 &tcs->ctids[index]) > 0) ||
					 (index < tcs->nrows &&
					  ItemPointerCompare(&tcs->ctids[index + 1],
										 &tcs->ctids[index]) < 0)))
					tcs->is_sorted = false;
			}
			else
			{
				/* remove this record from the column-store */
				HeapTupleHeaderSetXmax(&tcs->theads[index],
									   FrozenTransactionId);
			}
		}
		index++;
	}
}

/*
 * tcache_vacuum_row_store
 *
 *
 *
 */
static void
do_vacuum_row_store(tcache_row_store *trs, Buffer buffer)
{
	BlockNumber		blknum = BufferGetBlockNumber(buffer);
	OffsetNumber	offnum;
	ItemId			itemid;
	Page			page;
	cl_uint			index;

	if (blknum < trs->blkno_min || trs->blkno_max > blknum)
		return;

	page = BufferGetPage(buffer);
	for (index=0; index < trs->kern.nrows; index++)
	{
		rs_tuple   *rs_tup
			= kern_rowstore_get_tuple(&trs->kern, index);

		if (!rs_tup ||
			ItemPointerGetBlockNumber(&rs_tup->htup.t_self) != blknum)
			continue;

		offnum = ItemPointerGetOffsetNumber(&rs_tup->htup.t_self);
		itemid = PageGetItemId(page, offnum);

		if (!ItemIdIsNormal(itemid))
		{
			/* find an actual item-pointer, if redirected */
			while (ItemIdIsRedirected(itemid))
				itemid = PageGetItemId(page, ItemIdGetRedirect(itemid));

			if (ItemIdIsNormal(itemid))
			{
				/* needs to update item-pointer */
				ItemPointerSetOffsetNumber(&rs_tup->htup.t_self,
										   ItemIdGetOffset(itemid));
			}
			else
			{
				/* remove this record from the column store */
				cl_uint	   *tupoffset
					= kern_rowstore_get_offset(&trs->kern);

				tupoffset[index] = 0;
			}
		}
	}
}

static void
tcache_vacuum_row_store(tcache_head *tc_head, Buffer buffer)
{
	dlist_iter	iter;

	SpinLockAcquire(&tc_head->lock);
	if (tc_head->trs_curr)
		do_vacuum_row_store(tc_head->trs_curr, buffer);
	dlist_foreach(iter, &tc_head->trs_list)
	{
		tcache_row_store *trs
			= dlist_container(tcache_row_store, chain, iter.cur);
		do_vacuum_row_store(trs, buffer);
	}
	SpinLockRelease(&tc_head->lock);
}

/*
 * tcache_on_page_prune
 *
 * 
 *
 *
 */
static void
tcache_on_page_prune(Relation relation,
					 Buffer buffer,
					 int ndeleted,
					 TransactionId OldestXmin,
					 TransactionId latestRemovedXid)
{
	tcache_head	   *tc_head;

	if (heap_page_prune_hook_next)
		heap_page_prune_hook_next(relation, buffer, ndeleted,
								  OldestXmin, latestRemovedXid);

	tc_head = tcache_get_tchead(RelationGetRelid(relation), NULL, false);
	if (tc_head)
	{
		bool		has_lwlock = TCacheHeadLockedByMe(tc_head, false);

		/*
		 * At least, we need to acquire shared-lwlock on the tcache_head,
		 * but no need for exclusive-lwlock because vacuum page never
		 * create or drop tcache_nodes. Per node level spinlock is
		 * sufficient to do.
		 * Note that, vacuumed records are marked as junk, then columnizer
		 * actually removes them from the cache later, under the exclusive
		 * lock.
		 */
		if (!has_lwlock)
			LWLockAcquire(&tc_head->lwlock, LW_SHARED);

		tcache_vacuum_row_store(tc_head, buffer);
		tcache_vacuum_column_store(tc_head, buffer);

		if (!has_lwlock)
			LWLockRelease(&tc_head->lwlock);
	}
}

/*
 * tcache_on_object_access
 *
 * It invalidates an existing columnar-cache if cached tables were altered
 * or dropped. Also, it enforces to assign synchronizer trigger on new table
 * creation/
 */
static void
tcache_on_object_access(ObjectAccessType access,
						Oid classId,
						Oid objectId,
						int subId,
						void *arg)
{

	if (object_access_hook_next)
		object_access_hook_next(access, classId, objectId, subId, arg);

	/*
	 * Only relations, we are interested in
	 */
	if (classId == RelationRelationId)
	{
		if (access == OAT_POST_CREATE)
		{
			/*
			 * We consider to assign synchronizer trigger on statement-
			 * and row-level. It is needed to synchronize / invalidate
			 * cached object being constructed.
			 */
			pgstrom_assign_synchronizer(objectId);
		}
		else if (access == OAT_DROP || access == OAT_POST_ALTER)
		{
			/*
			 * Existing columnar-cache is no longer available across
			 * DROP or ALTER command (TODO: it's depend on the context.
			 * it may possible to have existing cache, if ALTER command
			 * does not change something related to the cached columns).
			 * So, we simply unlink the tcache_head associated with this
			 * relation, eventually someone who decrement its reference
			 * counter into zero releases the cache.
			 */
			int		hindex = tcache_hash_index(MyDatabaseId, objectId);

			SpinLockAcquire(&tc_common->lock);
			PG_TRY();
			{
				dlist_mutable_iter	iter;

				dlist_foreach_modify(iter, &tc_common->slot[hindex])
				{
					tcache_head	   *tc_head
						= dlist_container(tcache_head, chain, iter.cur);

					/* XXX - usually, only one cache per relation is linked */
					if (tc_head->datoid == MyDatabaseId &&
						tc_head->reloid == objectId)
						tcache_unlink_tchead_nolock(tc_head);
				}
			}
			PG_CATCH();
			{
				SpinLockRelease(&tc_common->lock);
				PG_RE_THROW();
			}
			PG_END_TRY();
			SpinLockRelease(&tc_common->lock);
		}
	}
}







static void
pgstrom_wakeup_columnizer(bool wakeup_all)
{
	dlist_iter	iter;

	SpinLockAcquire(&tc_common->lock);
	dlist_foreach(iter, &tc_common->inactive_list)
	{
		tcache_columnizer  *columnizer
			= dlist_container(tcache_columnizer, chain, iter.cur);

		SetLatch(columnizer->latch);
		if (!wakeup_all)
			break;
	}
    SpinLockRelease(&tc_common->lock);
}

static void
pgstrom_columnizer_main(Datum index)
{
	tcache_columnizer  *columnizer;
	int		rc;

	Assert(tc_common != NULL);
	Assert(index < num_columnizers);

	columnizer = &tc_common->columnizers[index];
	memset(columnizer, 0, sizeof(tcache_columnizer));
	columnizer->pid = getpid();
	columnizer->latch = &MyProc->procLatch;

	SpinLockAcquire(&tc_common->lock);
	dlist_push_tail(&tc_common->inactive_list, &columnizer->chain);
	SpinLockRelease(&tc_common->lock);

	while (true)
	{
		tcache_head		   *tc_head = NULL;
		tcache_node		   *tc_node;
		tcache_row_store   *trs;
		dlist_node	*dnode;

		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   15 * 1000);	/* wake up per 15s at least */
		if (rc & WL_POSTMASTER_DEATH)
			return;

		SpinLockAcquire(&tc_common->lock);
		if (!dlist_is_empty(&tc_common->pending_list))
		{
			dnode = dlist_pop_head_node(&tc_common->pending_list);
			tc_head = dlist_container(tcache_head, pending_chain, dnode);
			tc_head->refcnt++;
			columnizer->datoid = tc_head->datoid;
			columnizer->reloid = tc_head->reloid;
		}
		SpinLockRelease(&tc_common->lock);

		if (!tc_head)
			continue;

		/*
		 * TODO: add error handler routine
		 */
		LWLockAcquire(&tc_head->lwlock, LW_EXCLUSIVE);
		// SpinLockAcquire(&tc_head->lock);	/* probably unneeded */
		PG_TRY();
		{
			if (!dlist_is_empty(&tc_head->trs_list))
			{
				int		index;

				dnode = dlist_pop_head_node(&tc_head->trs_list);
				trs = dlist_container(tcache_row_store, chain, dnode);
				memset(&trs->chain, 0, sizeof(dlist_node));

				/*
				 * Move tuples in row-store into column-store
				 */
				for (index=0; index < trs->kern.nrows; index++)
				{
					rs_tuple *rs_tup
						= kern_rowstore_get_tuple(&trs->kern, index);
					if (rs_tup)
						tcache_insert_tuple(tc_head,
											tc_head->tcs_root,
											&rs_tup->htup);
				}
				/* row-store shall be released */
				tcache_put_row_store(trs);
			}
			else if (!dlist_is_empty(&tc_head->pending_list))
			{
				dnode = dlist_pop_head_node(&tc_head->pending_list);
				tc_node = dlist_container(tcache_node, chain, dnode);
				memset(&tc_node->chain, 0, sizeof(dlist_node));

				tcache_compaction_tcnode(tc_head, tc_node);
				tcache_try_merge_tcnode(tc_head, tc_node);
			}
		}
		PG_CATCH();
		{
			LWLockRelease(&tc_head->lwlock);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* OK, release this tcache_head */
		SpinLockAcquire(&tc_common->lock);
		columnizer->datoid = InvalidOid;
		columnizer->reloid = InvalidOid;
		tcache_put_tchead_nolock(tc_head);
		SpinLockRelease(&tc_common->lock);
	}
}











static void
pgstrom_startup_tcache(void)
{
	int		i;
	Size	length;
	bool	found;

	if (shmem_startup_hook_next)
		(*shmem_startup_hook_next)();

	length = offsetof(tcache_common, columnizers[num_columnizers]);
	tc_common = ShmemInitStruct("tc_common", MAXALIGN(length), &found);
	Assert(!found);
	memset(tc_common, 0, length);
	SpinLockInit(&tc_common->lock);
	dlist_init(&tc_common->lru_list);
	dlist_init(&tc_common->pending_list);
	for (i=0; i < TCACHE_HASH_SIZE; i++)
		dlist_init(&tc_common->slot[i]);
	dlist_init(&tc_common->inactive_list);
}

void
pgstrom_init_tcache(void)
{
	BackgroundWorker	worker;
	Size	length;
	int		i;

	/* number of columnizer worker processes */
	DefineCustomIntVariable("pgstrom.num_columnizers",
							"number of columnizer worker processes",
							NULL,
							&num_columnizers,
							1,
							1,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	/* launch background worker processes */
	for (i=0; i < num_columnizers; i++)
	{
		memset(&worker, 0, sizeof(BackgroundWorker));
		snprintf(worker.bgw_name, sizeof(worker.bgw_name),
				 "PG-Strom columnizer-%u", i);
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
		worker.bgw_start_time = BgWorkerStart_PostmasterStart;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		worker.bgw_main = pgstrom_columnizer_main;
		worker.bgw_main_arg = i;
		RegisterBackgroundWorker(&worker);
	}

	/* callback on vacuum-pages for cache invalidation */
	heap_page_prune_hook_next = heap_page_prune_hook;
	heap_page_prune_hook = tcache_on_page_prune;

	/* callback on object-access for cache invalidation */
	object_access_hook_next = object_access_hook;
	object_access_hook = tcache_on_object_access;

	/* aquires shared memory region */
	length = offsetof(tcache_common, columnizers[num_columnizers]);
	RequestAddinShmemSpace(MAXALIGN(length));
	shmem_startup_hook_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_startup_tcache;
}
