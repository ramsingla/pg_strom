/*
 * main.c
 *
 * Entrypoint of PG-Strom extension, and misc uncategolized functions.
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include <limits.h>
#include "pg_strom.h"

PG_MODULE_MAGIC;

/*
 * miscellaneous GUC parameters
 */
bool	pgstrom_enabled;
bool	pgstrom_perfmon_enabled;
int		pgstrom_max_async_chunks;
int		pgstrom_min_async_chunks;

static void
pgstrom_init_misc_guc(void)
{
	/* GUC variables according to the device information */
	DefineCustomBoolVariable("pg_strom.enabled",
							 "Enables the planner's use of PG-Strom",
							 NULL,
							 &pgstrom_enabled,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_strom.perfmon",
							 "Enables the performance monitor of PG-Strom",
							 NULL,
							 &pgstrom_perfmon_enabled,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("pg_strom.min_async_chunks",
							"least number of chunks to be run asynchronously",
							NULL,
							&pgstrom_min_async_chunks,
							2,
							2,
							INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("pg_strom.max_async_chunks",
							"max number of chunk to be run asynchronously",
							NULL,
							&pgstrom_max_async_chunks,
							32,
							pgstrom_min_async_chunks + 1,
							INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	if (pgstrom_max_async_chunks <= pgstrom_min_async_chunks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"pg_strom.max_async_chunks\" must be larger than \"pg_strom.min_async_chunks\"")));
}

void
_PG_init(void)
{
	/*
	 * PG-Strom has to be loaded using shared_preload_libraries option
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		errmsg("PG-Strom must be loaded via shared_preload_libraries")));

	/* load OpenCL runtime and initialize entrypoints */
	pgstrom_init_opencl_entry();

	/* initialization of device info on postmaster stage */
	pgstrom_init_opencl_devinfo();
	pgstrom_init_opencl_devprog();

	/* initialization of message queue on postmaster stage */
	pgstrom_init_mqueue();

	/* initialization of resource tracking subsystem */
	pgstrom_init_restrack();

	/* initialize shared memory segment and memory context stuff */
	pgstrom_init_shmem();

	/* registration of OpenCL background worker process */
	pgstrom_init_opencl_server();

	/* registration of custom-plan providers */
	pgstrom_init_gpuscan();

	/* miscellaneous initializations */
	pgstrom_init_misc_guc();
	pgstrom_init_debug();
	pgstrom_codegen_init();
}

/*
 * pgstrom_strerror
 *
 * translation from StromError_* to human readable form
 */
const char *
pgstrom_strerror(cl_int errcode)
{
	static char		unknown_buf[256];

	if (errcode < 0)
		return opencl_strerror(errcode);

	switch (errcode)
	{
		case StromError_Success:
			return "success";
		case StromError_RowFiltered:
			return "row is filtered";
		case StromError_RowReCheck:
			return "row should be rechecked";
		case StromError_ServerNotReady:
			return "OpenCL server is not ready";
		case StromError_BadRequestMessage:
			return "request message is bad";
		case StromError_OpenCLInternal:
			return "OpenCL internal error";
		case StromError_OutOfSharedMemory:
			return "out of shared memory";
		case StromError_DivisionByZero:
			return "division by zero";
		default:
			snprintf(unknown_buf, sizeof(unknown_buf),
					 "undefined strom error (code: %d)", errcode);
			break;
	}
	return unknown_buf;
}

/* ------------------------------------------------------------
 *
 * Routines copied from core PostgreSQL implementation
 *
 * ------------------------------------------------------------
 */
void
show_scan_qual(List *qual, const char *qlabel,
               PlanState *planstate, List *ancestors,
               ExplainState *es)
{
	bool        useprefix;
	Node	   *node;
	List       *context;
	char       *exprstr;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/* Set up deparsing context */
	context = deparse_context_for_planstate((Node *) planstate,
											ancestors,
											es->rtable,
											es->rtable_names);

	/* Deparse the expression */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}

/*
 * If it's EXPLAIN ANALYZE, show instrumentation information for a plan node
 *
 * "which" identifies which instrumentation counter to print
 */
void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* In text mode, suppress zero counts; they're not interesting enough */
	if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, 0.0, 0, es);
	}
}

void
show_device_kernel(Datum dprog_key, ExplainState *es)
{
	StringInfoData	str;
	const char *kernel_source;
	int32		extra_flags;

	if (!es->verbose)
		return;

	kernel_source = pgstrom_get_devprog_kernel_source(dprog_key);
	extra_flags = pgstrom_get_devprog_extra_flags(dprog_key);

	initStringInfo(&str);
	/*
	 * In case of EXPLAIN command context, we show the built-in logics
	 * like a usual #include preprocessor command.
	 * Practically, clCreateProgramWithSource() accepts multiple cstrings
	 * as if external files are included.
	 */
	if (extra_flags & DEVKERNEL_NEEDS_DEBUG)
		appendStringInfo(&str, "#define PGSTROM_KERNEL_DEBUG 1\n");

	appendStringInfo(&str, "#include \"opencl_common.h\"\n");
	if (extra_flags & DEVFUNC_NEEDS_TIMELIB)
		appendStringInfo(&str, "#include \"opencl_timelib.h\"\n");
	if (extra_flags & DEVFUNC_NEEDS_TEXTLIB)
		appendStringInfo(&str, "#include \"opencl_textlib.h\"\n");
	if (extra_flags & DEVFUNC_NEEDS_NUMERICLIB)
		appendStringInfo(&str, "#include \"opencl_numericlib.h\"\n");
	if (extra_flags & DEVKERNEL_NEEDS_GPUSCAN)
		appendStringInfo(&str, "#include \"opencl_gpuscan.h\"\n");
	if (extra_flags & DEVKERNEL_NEEDS_GPUSORT)
		appendStringInfo(&str, "#include \"opencl_gpusort.h\"\n");
	if (extra_flags & DEVKERNEL_NEEDS_HASHJOIN)
		appendStringInfo(&str, "#include \"opencl_hashjoin.h\"\n");
	appendStringInfo(&str, "\n%s", kernel_source);

	ExplainPropertyText("Kernel Source", str.data, es);

	pfree(str.data);
}

void
pgstrom_perfmon_add(pgstrom_perfmon *pfm_sum, pgstrom_perfmon *pfm_item)
{
	if (!pfm_sum->enabled)
		return;

	pfm_sum->num_samples++;
	pfm_sum->time_to_load	+= pfm_item->time_to_load;
	pfm_sum->time_in_sendq	+= pfm_item->time_in_sendq;
	pfm_sum->time_kern_build
		= Max(pfm_sum->time_kern_build,
			  pfm_item->time_kern_build);
	pfm_sum->time_dma_send	+= pfm_item->time_dma_send;
	pfm_sum->time_kern_exec	+= pfm_item->time_kern_exec;
	pfm_sum->time_dma_recv	+= pfm_item->time_dma_recv;
	pfm_sum->time_in_recvq	+= pfm_item->time_in_recvq;
}

void
pgstrom_perfmon_explain(pgstrom_perfmon *pfm, ExplainState *es)
{
	double	n = (double)pfm->num_samples;
	char	buf[256];

	if (!pfm->enabled)
		return;

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_to_load / 1000.0);
	ExplainPropertyText("Total time to load", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_in_sendq / n / 1000.0);
	ExplainPropertyText("Avg time in send-mq", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_kern_build / 1000.0);
	ExplainPropertyText("Max time to build kernel", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_dma_send / n / 1000.0);
	ExplainPropertyText("Avg time of DMA send", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_dma_send / 1000.0);
	ExplainPropertyText("Total time of DMA send", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_kern_exec / n / 1000.0);
	ExplainPropertyText("Avg time of kernel exec", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_kern_exec / 1000.0);
	ExplainPropertyText("Total time of kernel exec", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_dma_recv / n / 1000.0);
	ExplainPropertyText("Avg time of DMA recv", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_dma_recv / 1000.0);
	ExplainPropertyText("Total time of DMA recv", buf, es);

	snprintf(buf, sizeof(buf), "%.3f ms",
			 (double)pfm->time_in_recvq / n / 1000.0);
	ExplainPropertyText("Avg time in recv-mq", buf, es);
}
