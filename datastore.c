/*
 * datastore.c
 *
 * Routines to manage data store; row-store, column-store, toast-buffer,
 * and param-buffer.
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "access/relscan.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "pg_strom.h"

/*
 * pgstrom_create_param_buffer
 *
 * It construct a param-buffer on the shared memory segment, according to
 * the supplied Const/Param list. Its initial reference counter is 1, so
 * this buffer can be released using pgstrom_put_param_buffer().
 */
kern_parambuf *
pgstrom_create_kern_parambuf(List *used_params,
							 ExprContext *econtext)
{
	StringInfoData	str;
	kern_parambuf  *kpbuf;
	char		padding[STROMALIGN_LEN];
	ListCell   *cell;
	Size		offset;
	int			index = 0;
	int			nparams = list_length(used_params);

	/* seek to the head of variable length field */
	offset = STROMALIGN(offsetof(kern_parambuf, poffset[nparams]));
	initStringInfo(&str);
	enlargeStringInfo(&str, offset);
	str.len = offset;
	/* walks on the Para/Const list */
	foreach (cell, used_params)
	{
		Node   *node = lfirst(cell);

		if (IsA(node, Const))
		{
			Const  *con = (Const *) node;

			kpbuf = (kern_parambuf *)str.data;
			if (con->constisnull)
				kpbuf->poffset[index] = 0;	/* null */
			else
			{
				kpbuf->poffset[index] = str.len;
				if (con->constlen > 0)
					appendBinaryStringInfo(&str,
										   (char *)&con->constvalue,
										   con->constlen);
				else
					appendBinaryStringInfo(&str,
										   DatumGetPointer(con->constvalue),
										   VARSIZE(con->constvalue));
			}
		}
		else if (IsA(node, Param))
		{
			ParamListInfo param_info = econtext->ecxt_param_list_info;
			Param  *param = (Param *) node;

			if (param_info &&
				param->paramid > 0 && param->paramid <= param_info->numParams)
			{
				ParamExternData	*prm = &param_info->params[param->paramid - 1];

				/* give hook a chance in case parameter is dynamic */
				if (!OidIsValid(prm->ptype) && param_info->paramFetch != NULL)
					(*param_info->paramFetch) (param_info, param->paramid);

				kpbuf = (kern_parambuf *)str.data;
				if (!OidIsValid(prm->ptype))
				{
					kpbuf->poffset[index] = 0;	/* null */
					continue;
				}
				/* safety check in case hook did something unexpected */
				if (prm->ptype != param->paramtype)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
									param->paramid,
									format_type_be(prm->ptype),
									format_type_be(param->paramtype))));
				if (prm->isnull)
					kpbuf->poffset[index] = 0;	/* null */
				else
				{
					int		typlen = get_typlen(prm->ptype);

					if (typlen == 0)
						elog(ERROR, "cache lookup failed for type %u",
							 prm->ptype);
					if (typlen > 0)
						appendBinaryStringInfo(&str,
											   (char *)&prm->value,
											   typlen);
					else
						appendBinaryStringInfo(&str,
											   DatumGetPointer(prm->value),
											   VARSIZE(prm->value));
				}
			}
		}
		else
			elog(ERROR, "unexpected node: %s", nodeToString(node));

		/* alignment */
		if (STROMALIGN(str.len) != str.len)
			appendBinaryStringInfo(&str, padding,
								   STROMALIGN(str.len) - str.len);
		index++;
	}
	Assert(STROMALIGN(str.len) == str.len);
	kpbuf = (kern_parambuf *)str.data;
	kpbuf->length = str.len;
	kpbuf->nparams = nparams;

	return kpbuf;
}

/*
 * pgstrom_load_row_store_heap
 *
 * It creates a new row-store and loads tuples from the supplied heap.
 */
pgstrom_row_store *
pgstrom_load_row_store_heap(HeapScanDesc scan,
							ScanDirection direction,
							kern_colmeta *rs_colmeta,
							kern_colmeta *cs_colmeta,
							int cs_colnums,
							bool *scan_done)
{
	pgstrom_row_store *rstore;
	Relation	rel = scan->rs_rd;
	AttrNumber	rs_ncols = RelationGetNumberOfAttributes(rel);
	HeapTuple	tuple;
	cl_uint		nrows;
	cl_uint		usage_head;
	cl_uint		usage_tail;
	cl_uint		offset;
	cl_uint	   *p_offset;
	kern_column_store *kcs_head;
	int			index;

	Assert(direction != 0);

	rstore = pgstrom_shmem_alloc(ROWSTORE_DEFAULT_SIZE);
	if (!rstore)
		elog(ERROR, "out of shared memory");

	/*
	 * We put header portion of kern_column_store next to the kern_row_store
	 * as source of copy for in-kernel column store. It has offset of column
	 * array, but contents shall be set up by kernel prior to evaluation of
	 * qualifier expression.
	 */
	rstore->stag = StromTag_RowStore;
	rstore->kern.length
		= STROMALIGN_DOWN(ROWSTORE_DEFAULT_SIZE -
						  STROMALIGN(offsetof(kern_column_store,
											  colmeta[cs_colnums])) -
						  offsetof(pgstrom_row_store, kern));
	rstore->kern.ncols = rs_ncols;
	rstore->kern.nrows = 0;
	memcpy(rstore->kern.colmeta,
		   rs_colmeta,
		   sizeof(kern_colmeta) * rs_ncols);

	/*
	 * OK, load tuples and put them onto the row-store.
	 * The offset array of rs_tuple begins next to the column-metadata.
	 */
	p_offset = (cl_uint *)(&rstore->kern.colmeta[rs_ncols]);
	usage_head = offsetof(kern_row_store, colmeta[rs_ncols]);
	usage_tail = rstore->kern.length;
	nrows = 0;

	while (HeapTupleIsValid(tuple = heap_getnext(scan, direction)))
	{
		Size		length = HEAPTUPLESIZE + MAXALIGN(tuple->t_len);
		rs_tuple   *rs_tup;

		if (usage_tail - length < sizeof(cl_uint) + usage_head)
		{
			/*
			 * if we have no room to put the fetched tuple on the row-store,
			 * we rewind the tuple (to be read on the next time) and break
			 * the loop.
			 */
			heap_getnext(scan, -direction);
			break;
		}
		usage_tail -= length;
		usage_head += sizeof(cl_uint);
		rs_tup = (rs_tuple *)((char *)&rstore->kern + usage_tail);
		memcpy(&rs_tup->htup, tuple, sizeof(HeapTupleData));
		rs_tup->htup.t_data = &rs_tup->data;
		memcpy(&rs_tup->data, tuple->t_data, tuple->t_len);

		p_offset[nrows++] = usage_tail;
	}
	Assert(nrows > 0);
	rstore->kern.nrows = nrows;

	/* needs to inform where heap-scan reached to end of the relation */
	if (!HeapTupleIsValid(tuple))
		*scan_done = true;
	else
		*scan_done = false;

	/*
	 * Header portion of the kern_column_store is put on the tail of
	 * shared memory block; to be copied to in-kernel data structure.
	 */
	kcs_head = (kern_column_store *)((char *)(&rstore->kern) +
									 rstore->kern.length);
	kcs_head->ncols = cs_colnums;
	kcs_head->nrows = nrows;
	memcpy(kcs_head->colmeta, cs_colmeta,
		   sizeof(kern_colmeta) * cs_colnums);

	offset = STROMALIGN(offsetof(kern_column_store, colmeta[cs_colnums]));
	for (index=0; index < cs_colnums; index++)
	{
		kern_colmeta   *ccmeta = &kcs_head->colmeta[index];

		Assert((ccmeta->flags & KERN_COLMETA_ATTREFERENCED) != 0);
		ccmeta->cs_ofs = offset;
		if ((ccmeta->flags & KERN_COLMETA_ATTNOTNULL) == 0)
			offset += STROMALIGN((nrows + 7) / 8);
		offset += STROMALIGN(nrows * (ccmeta->attlen > 0
									  ? ccmeta->attlen
									  : sizeof(cl_uint)));
	}
	kcs_head->length = offset;
	rstore->kcs_head = kcs_head;
	Assert(pgstrom_shmem_sanitycheck(rstore));
	return rstore;
}

#if 0
pgstrom_row_store *
pgstrom_load_row_store_subplan(void)
{
	elog(ERROR, "not implemented now");
	return NULL;
}
#endif

#ifdef USE_ASSERT_CHECKING
/*
 * SanityCheck_kern_column_store
 *
 * It applies sanity check on the supplied column-store being generated
 * in the device kern_row_to_column() function.
 */
void
SanityCheck_kern_column_store(kern_row_store *krs,		/* source */
							  kern_column_store *kcs)	/* generated */
{
	int		i, j, k;

	Assert(krs->nrows == kcs->nrows);

	for (i=0; i < krs->nrows; i++)
	{
		rs_tuple   *rs_tup = kern_rowstore_get_tuple(krs, i);
		size_t		rs_ofs = (rs_tup != NULL ? rs_tup->data.t_hoff : 0);

		for (j=0, k=0; j < krs->ncols; j++)
		{
			kern_colmeta   *rcmeta = &krs->colmeta[j];
			kern_colmeta   *ccmeta;

			/* is it columnized? */
			if ((rcmeta->flags & KERN_COLMETA_ATTREFERENCED) == 0)
				continue;

			ccmeta = &kcs->colmeta[k];
			Assert(rcmeta->flags == ccmeta->flags);
			Assert(rcmeta->attalign == ccmeta->attalign);
			Assert(rcmeta->attlen == ccmeta->attlen);

			/* is this cell on the row-store null? */
			if (!rs_tup || ((rs_tup->data.t_infomask & HEAP_HASNULL) != 0 &&
							att_isnull(j, rs_tup->data.t_bits)))
			{
				/* must be null in the column store also */
				Assert((ccmeta->flags & KERN_COLMETA_ATTNOTNULL) == 0);
				Assert(att_isnull(i, (char *)kcs + ccmeta->cs_ofs));
			}
			else
			{
				size_t	cs_ofs = ccmeta->cs_ofs;

				/* must not be null in the column store also */
				if ((ccmeta->flags & KERN_COLMETA_ATTNOTNULL) == 0)
				{
					Assert(!att_isnull(i, (char *)kcs + cs_ofs));
					cs_ofs += STROMALIGN((kcs->nrows + 7) >> 3);
				}
				if (rcmeta->attlen > 0)
					rs_ofs = TYPEALIGN(rcmeta->attalign, rs_ofs);
				else if (!VARATT_NOT_PAD_BYTE((char *)&rs_tup->data + rs_ofs))
					rs_ofs = TYPEALIGN(rcmeta->attalign, rs_ofs);

				cs_ofs += (ccmeta->attlen > 0
						   ? ccmeta->attlen
						   : sizeof(cl_uint)) * i;
				if (rcmeta->attlen > 0)
				{
					Assert(memcmp((char *)&rs_tup->data + rs_ofs,
								  (char *)kcs + cs_ofs,
								  ccmeta->attlen) == 0);
				}
				else
				{
					cl_uint		vl_ofs = *((cl_uint *)((char *)kcs + cs_ofs));

					Assert(((uintptr_t)(&rs_tup->data + rs_ofs) -
							(uintptr_t)(krs)) == vl_ofs);
				}
			}
			k++;
		}
		Assert(kcs->ncols == k);
	}
}
#endif /* USE_ASSERT_CHECKING */
