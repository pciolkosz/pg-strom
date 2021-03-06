/*
 * gpuscan.c
 *
 * Sequential scan accelerated by GPU processors
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"
#include "cuda_numeric.h"
#include "cuda_gpuscan.h"

static set_rel_pathlist_hook_type	set_rel_pathlist_next;
static CustomPathMethods	gpuscan_path_methods;
static CustomScanMethods	gpuscan_plan_methods;
static CustomExecMethods	gpuscan_exec_methods;
static bool					enable_gpuscan;
static bool					enable_pullup_outer_scan;

/*
 * form/deform interface of private field of CustomScan(GpuScan)
 */
typedef struct {
	ExtensibleNode	ex;
	char	   *kern_source;	/* source of the CUDA kernel */
	cl_uint		extra_flags;	/* extra libraries to be included */
	cl_uint		proj_tuple_sz;	/* nbytes of the expected result tuple size */
	cl_uint		proj_extra_sz;	/* length of extra-buffer on kernel */
	cl_uint		nrows_per_block;/* estimated tuple density per block */
	List	   *ccache_refs;	/* attributed to be referenced by ccache */
	List	   *used_params;
	List	   *dev_quals;		/* implicitly-ANDed device quals */
} GpuScanInfo;

static inline void
form_gpuscan_info(CustomScan *cscan, GpuScanInfo *gs_info)
{
	List	   *privs = NIL;
	List	   *exprs = NIL;

	privs = lappend(privs, makeString(gs_info->kern_source));
	privs = lappend(privs, makeInteger(gs_info->extra_flags));
	privs = lappend(privs, makeInteger(gs_info->proj_tuple_sz));
	privs = lappend(privs, makeInteger(gs_info->proj_extra_sz));
	privs = lappend(privs, makeInteger(gs_info->nrows_per_block));
	privs = lappend(privs, gs_info->ccache_refs);
	exprs = lappend(exprs, gs_info->used_params);
	exprs = lappend(exprs, gs_info->dev_quals);

	cscan->custom_private = privs;
	cscan->custom_exprs = exprs;
}

static inline GpuScanInfo *
deform_gpuscan_info(CustomScan *cscan)
{
	GpuScanInfo *gs_info = palloc0(sizeof(GpuScanInfo));
	List	   *privs = cscan->custom_private;
	List	   *exprs = cscan->custom_exprs;
	int			pindex = 0;
	int			eindex = 0;

	gs_info->kern_source = strVal(list_nth(privs, pindex++));
	gs_info->extra_flags = intVal(list_nth(privs, pindex++));
	gs_info->proj_tuple_sz = intVal(list_nth(privs, pindex++));
	gs_info->proj_extra_sz = intVal(list_nth(privs, pindex++));
	gs_info->nrows_per_block = intVal(list_nth(privs, pindex++));
	gs_info->ccache_refs = list_nth(privs, pindex++);
	gs_info->used_params = list_nth(exprs, eindex++);
	gs_info->dev_quals = list_nth(exprs, eindex++);

	return gs_info;
}

typedef struct {
	pg_atomic_uint64 nitems_filtered;
	pg_atomic_uint64 ccache_count;
} GpuScanRuntimeStat;

typedef struct {
	dsm_handle		ss_handle;		/* DSM handle of the SharedState */
	cl_uint			ss_length;		/* Length of the SharedState */
	GpuScanRuntimeStat gs_rtstat;
} GpuScanSharedState;

typedef struct {
	GpuTaskState	gts;
	GpuScanSharedState *gs_sstate;
	GpuScanRuntimeStat *gs_rtstat;
	HeapTupleData	scan_tuple;		/* buffer to fetch tuple */
#if PG_VERSION_NUM < 100000
	List		   *dev_quals;		/* quals to be run on the device */
#else
	ExprState	   *dev_quals;		/* quals to be run on the device */
#endif
	bool			dev_projection;	/* true, if device projection is valid */
	cl_uint			proj_tuple_sz;
	cl_uint			proj_extra_sz;
	/* resource for CPU fallback */
	TupleTableSlot *base_slot;
	ProjectionInfo *base_proj;
} GpuScanState;

typedef struct
{
	GpuTask				task;
	bool				with_nvme_strom;
	bool				with_projection;
	/* DMA buffers */
	pgstrom_data_store *pds_src;
	pgstrom_data_store *pds_dst;
	kern_resultbuf	   *kresults;
	kern_gpuscan		kern;
} GpuScanTask;

/*
 * static functions
 */
static GpuTask  *gpuscan_next_task(GpuTaskState *gts);
static TupleTableSlot *gpuscan_next_tuple(GpuTaskState *gts);
static void gpuscan_switch_task(GpuTaskState *gts, GpuTask *gtask);
static int gpuscan_process_task(GpuTask *gtask, CUmodule cuda_module);
static void gpuscan_release_task(GpuTask *gtask);

static GpuScanSharedState *createGpuScanSharedState(GpuScanState *gss,
													ParallelContext *pcxt,
													void *dsm_addr);
static void resetGpuScanSharedState(GpuScanState *gss);

/*
 * cost_gpuscan_common - common part of cost estimation for GpuScan
 * 
 * Once a simple scan path is pulled up to upper node, this node takes over
 * the jobs of relation scan and execution of outer qualifiers instead of
 * execution of GpuScan node. So, its cost needs to be added to the upper
 * node.
 */
void
cost_gpuscan_common(PlannerInfo *root,
					RelOptInfo *scan_rel,
					Expr *scan_quals,
					int parallel_workers,
					double *p_parallel_divisor,
					double *p_scan_ntuples,
					double *p_scan_nchunks,
					cl_uint *p_nrows_per_block,
					Cost *p_startup_cost,
					Cost *p_run_cost)
{
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	double		gpu_ratio = pgstrom_gpu_operator_cost / cpu_operator_cost;
	double		parallel_divisor = (double) parallel_workers;
	double		ntuples = scan_rel->tuples;
	double		nchunks;
	double		selectivity;
	double		spc_seq_page_cost;
	cl_uint		nrows_per_block;
	Size		heap_size;
	Size		htup_size;
	QualCost	qcost;

	Assert(scan_rel->reloptkind == RELOPT_BASEREL &&
		   scan_rel->relid > 0 &&
		   scan_rel->relid < root->simple_rel_array_size);

	/* selectivity of device executable qualifiers */
	selectivity = clause_selectivity(root,
									 (Node *)scan_quals,
									 scan_rel->relid,
									 JOIN_INNER,
									 NULL);

	/* fetch estimated page cost for tablespace containing the table */
	/*
	 * TODO: we may need to discount page cost if NVMe-Strom is capable
	 */
	get_tablespace_page_costs(scan_rel->reltablespace,
							  NULL, &spc_seq_page_cost);

	/*
	 * Discount page scan cost if NVMe-Strom is capable
	 *
	 * XXX - acceleration ratio depends on number of SSDs configured
	 * as MD0-RAID volume, number of parallel workers and so on.
	 * Once NVMe-Strom driver supports hardware configuration info,
	 * we follow it.
	 */
	if (ScanPathWillUseNvmeStrom(root, scan_rel))
	{
		/* FIXME: discount 50% if NVMe-Strom is ready */
		spc_seq_page_cost /= 1.5;
		/*
		 * FIXME: i/o concurrency will effective throughput according
		 * to the number of parallel workers
		 */
		if (parallel_workers > 0)
			spc_seq_page_cost /= (Cost)(1 + Min(parallel_workers, 4));
	}

	/*
	 * Disk i/o cost; we may add special treatment for NVMe-Strom.
	 * On the other hands, planner usually choose PG-Strom's path
	 * for large scale of data.
	 */
	run_cost += spc_seq_page_cost * (double)scan_rel->pages;

	/*
	 * Cost adjustment by CPU parallelism, if used.
	 * (overall logic is equivalent to cost_seqscan())
	 */
	if (parallel_workers > 0)
	{
		double		leader_contribution;

		/* How much leader process can contribute query execution? */
		leader_contribution = 1.0 - (0.3 * (double)parallel_workers);
		if (leader_contribution > 0)
			parallel_divisor += leader_contribution;

		/* number of tuples to be actually processed */
		ntuples  = clamp_row_est(ntuples / parallel_divisor);

		/*
		 * After the v2.0, pg_strom.gpu_setup_cost represents the cost for
		 * run-time code build by NVRTC. Once binary is constructed, it can
		 * be shared with all the worker process, so we can discount the
		 * cost by parallel_divisor.
		 */
		startup_cost += pgstrom_gpu_setup_cost / parallel_divisor;
	}
	else
	{
		parallel_divisor = 1.0;
		startup_cost += pgstrom_gpu_setup_cost;
	}

	/* estimation for number of chunks (assume KDS_FORMAT_ROW) */
	heap_size = (double)(BLCKSZ - SizeOfPageHeaderData) * scan_rel->pages;
	htup_size = (MAXALIGN(offsetof(HeapTupleHeaderData,
								   t_bits[BITMAPLEN(scan_rel->max_attr)])) +
				 MAXALIGN(heap_size / Max(scan_rel->tuples, 1.0) -
						  sizeof(ItemIdData) - SizeofHeapTupleHeader));
	nchunks =  (((double)(offsetof(kern_tupitem, htup) + htup_size +
						  sizeof(cl_uint)) * Max(ntuples, 1.0)) /
				((double)(pgstrom_chunk_size() -
						  KDS_CALCULATE_HEAD_LENGTH(scan_rel->max_attr))));
	nchunks = Max(nchunks, 1);

	/*
	 * estimation of the tuple density per block - this logic follows
	 * the manner in estimate_rel_size()
	 */
	if (scan_rel->pages > 0)
		nrows_per_block = ceil(scan_rel->tuples / (double)scan_rel->pages);
	else
	{
		RangeTblEntry *rte = root->simple_rte_array[scan_rel->relid];
		size_t		tuple_width = get_relation_data_width(rte->relid, NULL);

		tuple_width += MAXALIGN(SizeofHeapTupleHeader);
		tuple_width += sizeof(ItemIdData);
		/* note: integer division is intentional here */
		nrows_per_block = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
	}

	/* Cost for GPU qualifiers */
	cost_qual_eval_node(&qcost, (Node *)scan_quals, root);
	startup_cost += qcost.startup;
	run_cost += qcost.per_tuple * gpu_ratio * ntuples;
	ntuples *= selectivity;

	/* Cost for DMA transfer (host/storage --> GPU) */
	run_cost += pgstrom_gpu_dma_cost * nchunks;

	*p_parallel_divisor = parallel_divisor;
	*p_scan_ntuples = ntuples / parallel_divisor;
	*p_scan_nchunks = nchunks / parallel_divisor;
	*p_nrows_per_block = nrows_per_block;
	*p_startup_cost = startup_cost;
	*p_run_cost = run_cost;
}

/*
 * cost_for_dma_receive - cost estimation for DMA receive (GPU->host)
 */
Cost
cost_for_dma_receive(RelOptInfo *rel, double ntuples)
{
	PathTarget *reltarget = rel->reltarget;
	cl_int		nattrs = list_length(reltarget->exprs);
	cl_int		width_per_tuple;

	if (ntuples < 0.0)
		ntuples = rel->rows;
	width_per_tuple = offsetof(kern_tupitem, htup) +
		MAXALIGN(offsetof(HeapTupleHeaderData,
						  t_bits[BITMAPLEN(nattrs)])) +
		MAXALIGN(reltarget->width);
	return pgstrom_gpu_dma_cost *
		(((double)width_per_tuple * ntuples) / (double)pgstrom_chunk_size());
}

/*
 * create_gpuscan_path - constructor of CustomPath(GpuScan) node
 */
static Path *
create_gpuscan_path(PlannerInfo *root,
					RelOptInfo *baserel,
					List *dev_quals,
					List *host_quals,
					int parallel_nworkers)
{
	GpuScanInfo	   *gs_info = palloc0(sizeof(GpuScanInfo));
	CustomPath	   *cpath;
	ParamPathInfo  *param_info;
	List		   *ppi_quals;
	Expr		   *dev_quals_expr = NULL;
	Cost			startup_cost;
	Cost			run_cost;
	Cost			startup_delay;
	QualCost		qcost;
	double			parallel_divisor;
	double			scan_ntuples;
	double			scan_nchunks;
	double			cpu_per_tuple = 0.0;

	/* cost for disk i/o + GPU qualifiers */
	if (dev_quals != NIL)
	{
		List   *extract_list = extract_actual_clauses(dev_quals, false);
		dev_quals_expr = make_flat_ands_explicit(extract_list);
	}
	cost_gpuscan_common(root, baserel,
						dev_quals_expr,
						parallel_nworkers,
						&parallel_divisor,
						&scan_ntuples,
						&scan_nchunks,
						&gs_info->nrows_per_block,
						&startup_cost,
						&run_cost);

	param_info = get_baserel_parampathinfo(root, baserel,
										   baserel->lateral_relids);
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = baserel;
	cpath->path.pathtarget = baserel->reltarget;
	cpath->path.param_info = param_info;
	cpath->path.parallel_aware = parallel_nworkers > 0 ? true : false;
	cpath->path.parallel_safe = baserel->consider_parallel;
	cpath->path.parallel_workers = parallel_nworkers;
	cpath->path.rows = (param_info
						? param_info->ppi_rows
						: baserel->rows) / parallel_divisor;

	/* cost for DMA receive (GPU-->host) */
	run_cost += cost_for_dma_receive(baserel, scan_ntuples);

	/* cost for CPU qualifiers */
	cost_qual_eval(&qcost, host_quals, root);
	startup_cost += qcost.startup;
	cpu_per_tuple += qcost.per_tuple;

	/* PPI costs (as a part of host quals, if any) */
	ppi_quals = (param_info ? param_info->ppi_clauses : NIL);
	cost_qual_eval(&qcost, ppi_quals, root);
	startup_cost += qcost.startup;
	cpu_per_tuple += qcost.per_tuple;
	run_cost += (cpu_per_tuple + cpu_tuple_cost) * scan_ntuples;

	/*
	 * Cost for projection
	 *
	 * MEMO: Even if GpuScan can run complicated projection on the device,
	 * expression on the target-list shall be assigned on the CustomPath node
	 * after the selection of the cheapest path, and its cost shall be
	 * discounted by the core logic (see apply_projection_to_path).
	 * In the previous implementation, we discounted the cost to be processed
	 * by GpuProjection, however, it leads unexpected optimizer behavior.
	 * Right now, we stop to discount the cost for GpuProjection.
	 * Probably, it needs API enhancement of CustomScan.
	 */
	startup_cost += baserel->reltarget->cost.startup;
	run_cost += baserel->reltarget->cost.per_tuple * scan_ntuples;

	/* Latency to get the first chunk */
	startup_delay = run_cost * (1.0 / scan_nchunks);

	cpath->path.startup_cost = startup_cost + startup_delay;
	cpath->path.total_cost = startup_cost + run_cost;
	cpath->path.pathkeys = NIL;	/* unsorted results */
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = list_make1(gs_info);
	cpath->methods = &gpuscan_path_methods;

	return &cpath->path;
}

/*
 * gpuscan_add_scan_path - entrypoint of the set_rel_pathlist_hook
 */
static void
gpuscan_add_scan_path(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Index rtindex,
					  RangeTblEntry *rte)
{
	Path	   *pathnode;
	List	   *dev_quals = NIL;
	List	   *host_quals = NIL;
	ListCell   *lc;

	/* call the secondary hook */
	if (set_rel_pathlist_next)
		set_rel_pathlist_next(root, baserel, rtindex, rte);

	/* nothing to do, if either PG-Strom or GpuScan is not enabled */
	if (!pgstrom_enabled || !enable_gpuscan)
		return;

	/* We already proved the relation empty, so nothing more to do */
	if (IS_DUMMY_REL(baserel))
		return;

	/* It is the role of built-in Append node */
	if (rte->inh)
		return;

	/* only base relation we can handle */
	if (rte->rtekind != RTE_RELATION)
		return;
	if (rte->relkind != RELKIND_RELATION &&
		rte->relkind != RELKIND_MATVIEW)
		return;

	/* Check whether the qualifier can run on GPU device */
	foreach (lc, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		if (pgstrom_device_expression(rinfo->clause))
			dev_quals = lappend(dev_quals, rinfo);
		else
			host_quals = lappend(host_quals, rinfo);
	}
	if (dev_quals == NIL)
		return;

	/* add GpuScan path in single process */
	pathnode = create_gpuscan_path(root, baserel,
								   dev_quals,
								   host_quals,
								   0);
	add_path(baserel, pathnode);

	/* If appropriate, consider parallel GpuScan */
	if (baserel->consider_parallel && baserel->lateral_relids == NULL)
	{
		int		parallel_nworkers;

		parallel_nworkers = compute_parallel_worker(baserel,
													baserel->pages, -1.0);
		/*
		 * XXX - Do we need a something specific logic for GpuScan to adjust
		 * parallel_workers.
		 */
		if (parallel_nworkers <= 0)
			return;

		/* add GpuScan path performing on parallel workers */
		pathnode = create_gpuscan_path(root, baserel,
									   dev_quals,
									   host_quals,
									   parallel_nworkers);
		add_partial_path(baserel, pathnode);

		/* then, potentially generate Gather + GpuScan path */
		generate_gather_paths(root, baserel);

		foreach (lc, baserel->pathlist)
		{
			pathnode = lfirst(lc);
		}
	}
}

/*
 * Code generator for GpuScan's qualifier
 */
void
codegen_gpuscan_quals(StringInfo kern, codegen_context *context,
					  Index scanrelid, Expr *dev_quals)
{
	devtype_info   *dtype;
	StringInfoData	tfunc;
	StringInfoData	cfunc;
	StringInfoData	temp;
	Var			   *var;
	char		   *expr_code = NULL;
	ListCell	   *lc;

	initStringInfo(&tfunc);
	initStringInfo(&cfunc);
	initStringInfo(&temp);

	if (!dev_quals)
		goto output;

	/* Let's walk on the device expression tree */
	expr_code = pgstrom_codegen_expression((Node *)dev_quals, context);
	/* Const/Param declarations */
	pgstrom_codegen_param_declarations(&cfunc, context);
	pgstrom_codegen_param_declarations(&tfunc, context);
	/* Sanity check of used_vars */
	foreach (lc, context->used_vars)
	{
		var = lfirst(lc);
		if (var->varno != scanrelid)
			elog(ERROR, "unexpected var-node reference: %s expected %u",
				 nodeToString(var), scanrelid);
		if (var->varattno == 0)
			elog(ERROR, "cannot have whole-row reference on GPU expression");
		if (var->varattno < 0)
			elog(ERROR, "cannot have system column on GPU expression");
		dtype = pgstrom_devtype_lookup(var->vartype);
		if (!dtype)
			elog(ERROR, "failed to lookup device type: %s",
				 format_type_be(var->vartype));
	}

	/*
	 * Var declarations - if qualifier uses only one variables (like x > 0),
	 * the pg_xxxx_vref() service routine is more efficient because it may
	 * use attcacheoff to skip walking on tuple attributes.
	 */
	if (list_length(context->used_vars) <= 1)
	{
		foreach (lc, context->used_vars)
		{
			var = lfirst(lc);

			if (var->varattno <= 0)
				elog(ERROR, "Bug? system column appeared in expression");

			dtype = pgstrom_devtype_lookup(var->vartype);
			appendStringInfo(
				&tfunc,
				"  pg_%s_t %s_%u;\n\n"
				"  addr = kern_get_datum_tuple(kds->colmeta,htup,%u);\n"
				"  %s_%u = pg_%s_datum_ref(kcxt,addr);\n",
				dtype->type_name,
				context->var_label,
				var->varattno,
				var->varattno - 1,
				context->var_label,
                var->varattno,
				dtype->type_name);
			appendStringInfo(
				&cfunc,
				"  pg_%s_t %s_%u;\n\n"
				"  addr = kern_get_datum_column(kds,%u,row_index);\n"
				"  %s_%u = pg_%s_datum_ref(kcxt,addr);\n",
				dtype->type_name,
				context->var_label,
				var->varattno,
				var->varattno - 1,
				context->var_label,
				var->varattno,
				dtype->type_name);
		}
	}
	else
	{
		AttrNumber		anum, varattno_max = 0;

		/* declarations */
		/* note that no expression including system column reference are*/
		resetStringInfo(&temp);
		foreach (lc, context->used_vars)
		{
			var = lfirst(lc);
			Assert(var->varattno > 0);
			dtype = pgstrom_devtype_lookup(var->vartype);
			appendStringInfo(
				&temp,
				"  pg_%s_t %s_%u;\n",
				dtype->type_name,
				context->var_label,
				var->varattno);
			varattno_max = Max(varattno_max, var->varattno);
		}
		appendStringInfoString(&tfunc, temp.data);
		appendStringInfoString(&cfunc, temp.data);

		appendStringInfoString(
			&tfunc,
			"  assert(htup != NULL);\n"
			"  EXTRACT_HEAP_TUPLE_BEGIN(addr, kds, htup);\n");
		for (anum=1; anum <= varattno_max; anum++)
		{
			foreach (lc, context->used_vars)
			{
				var = lfirst(lc);

				if (var->varattno == anum)
				{
					dtype = pgstrom_devtype_lookup(var->vartype);

					appendStringInfo(
						&tfunc,
						"  %s_%u = pg_%s_datum_ref(kcxt,addr);\n",
						context->var_label,
						var->varattno,
						dtype->type_name);
					appendStringInfo(
						&cfunc,
						"  addr = kern_get_datum_column(kds,%u,row_index);\n"
						"  %s_%u = pg_%s_datum_ref(kcxt,addr);\n",
						var->varattno - 1,
						context->var_label,
						var->varattno,
						dtype->type_name);
					break;	/* no need to read same value twice */
				}
			}

			if (anum < varattno_max)
				appendStringInfoString(
					&tfunc,
					"  EXTRACT_HEAP_TUPLE_NEXT(addr);\n");
		}
		appendStringInfoString(
			&tfunc,
			"  EXTRACT_HEAP_TUPLE_END();\n");
	}
output:
	appendStringInfo(
		kern,
		"STATIC_FUNCTION(cl_bool)\n"
		"gpuscan_quals_eval(kern_context *kcxt,\n"
		"                   kern_data_store *kds,\n"
		"                   ItemPointerData *t_self,\n"
		"                   HeapTupleHeaderData *htup)\n"
		"{\n"
		"  void *addr __attribute__((unused));\n"
		"%s\n"
		"  return %s;\n"
		"}\n\n"
		"STATIC_FUNCTION(cl_bool)\n"
		"gpuscan_quals_eval_column(kern_context *kcxt,\n"
		"                          kern_data_store *kds,\n"
		"                          cl_uint row_index)\n"
		"{\n"
		"  void *addr __attribute__((unused));\n"
		"%s\n"
		"  return %s;\n"
		"}\n\n",
		tfunc.data,
		!expr_code ? "true" : psprintf("EVAL(%s)", expr_code),
		cfunc.data,
		!expr_code ? "true" : psprintf("EVAL(%s)", expr_code));
}

/*
 * Code generator for GpuScan's projection
 */
static void
codegen_gpuscan_projection(StringInfo kern, codegen_context *context,
						   Index scanrelid, Relation relation,
						   List *__tlist_dev)
{
	TupleDesc		tupdesc = RelationGetDescr(relation);
	List		   *tlist_dev = NIL;
	AttrNumber	   *varremaps;
	Bitmapset	   *varattnos;
	ListCell	   *lc;
	int				prev;
	int				i, j, k;
	bool			has_extract_tuple = false;
	size_t			extra_size = 0;
	devtype_info   *dtype;
	StringInfoData	tdecl;
	StringInfoData	cdecl;
	StringInfoData	tbody;
	StringInfoData	cbody;
	StringInfoData	temp;

	initStringInfo(&tdecl);
	initStringInfo(&tbody);
	initStringInfo(&cdecl);
	initStringInfo(&cbody);
	initStringInfo(&temp);
	/*
	 * step.0 - extract non-junk attributes
	 */
	foreach (lc, __tlist_dev)
	{
		TargetEntry	   *tle = lfirst(lc);

		if (!tle->resjunk)
			tlist_dev = lappend(tlist_dev, tle);
	}

	/*
	 * step.1 - declaration of functions and KVAR_xx for expressions
	 */
	appendStringInfoString(
		&tdecl,
		"STATIC_FUNCTION(void)\n"
		"gpuscan_projection_tuple(kern_context *kcxt,\n"
		"                         kern_data_store *kds_src,\n"
		"                         HeapTupleHeaderData *htup,\n"
		"                         ItemPointerData *t_self,\n"
		"                         Datum *tup_values,\n"
		"                         cl_bool *tup_isnull,\n"
		"                         char *tup_extra)\n"
		"{\n"
		"  void    *curr __attribute__((unused));\n"
		"  cl_int   len __attribute__((unused));\n");

	appendStringInfoString(
		&cdecl,
		"STATIC_FUNCTION(void)\n"
		"gpuscan_projection_column(kern_context *kcxt,\n"
		"                          kern_data_store *kds_src,\n"
		"                          size_t src_index,\n"
		"                          Datum *tup_values,\n"
		"                          cl_bool *tup_isnull,\n"
		"                          char *tup_extra)\n"
		"{\n"
		"  void    *addr __attribute__((unused));\n"
		"  cl_uint  len  __attribute__((unused));\n");

	varremaps = palloc0(sizeof(AttrNumber) * list_length(tlist_dev));
	varattnos = NULL;
	foreach (lc, tlist_dev)
	{
		TargetEntry	   *tle = lfirst(lc);

		Assert(tle->resno > 0 && tle->resno <= list_length(tlist_dev));
		/*
		 * NOTE: If expression of TargetEntry is a simple Var-node,
		 * we can load the value into tup_values[]/tup_isnull[]
		 * array regardless of the data type. We have to track which
		 * column is the source of this TargetEntry.
		 * Elsewhere, we will construct device side expression using
		 * KVAR_xx variables.
		 */
		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;

			Assert(var->varno == scanrelid);
			Assert(var->varattno > FirstLowInvalidHeapAttributeNumber &&
				   var->varattno != InvalidAttrNumber &&
				   var->varattno <= tupdesc->natts);
			varremaps[tle->resno - 1] = var->varattno;
		}
		else
		{
			pull_varattnos((Node *)tle->expr, scanrelid, &varattnos);
		}
	}

	prev = -1;
	while ((prev = bms_next_member(varattnos, prev)) >= 0)
	{
		Form_pg_attribute attr;
		AttrNumber		anum = prev + FirstLowInvalidHeapAttributeNumber;

		Assert(anum != InvalidAttrNumber);
		if (anum < 0)
			attr = SystemAttributeDefinition(anum, true);
		else
			attr = tupdesc->attrs[anum - 1];

		dtype = pgstrom_devtype_lookup(attr->atttypid);
		if (!dtype)
			elog(ERROR, "Bug? failed to lookup device supported type: %s",
				 format_type_be(attr->atttypid));
		if (anum < 0)
			elog(ERROR, "Bug? system column appear in device expression");
		appendStringInfo(&tdecl, "  pg_%s_t KVAR_%u;\n",
						 dtype->type_name, anum);
		appendStringInfo(&cdecl, "  pg_%s_t KVAR_%u;\n",
						 dtype->type_name, anum);
	}

	/*
	 * System columns reference if any
	 */
	for (i=0; i < list_length(tlist_dev); i++)
	{
		Form_pg_attribute	attr;

		if (varremaps[i] >= 0)
			continue;
		attr = SystemAttributeDefinition(varremaps[i], true);
		j = attr->attnum + 1 + FirstLowInvalidHeapAttributeNumber;

		if (attr->attnum == TableOidAttributeNumber)
		{
			resetStringInfo(&temp);
			appendStringInfo(
				&temp,
				"  /* %s system column */\n"
				"  tup_isnull[%d] = !htup;\n"
				"  tup_values[%d] = kds_src->table_oid;\n",
				NameStr(attr->attname), i, i);
			appendStringInfoString(&tbody, temp.data);
			appendStringInfoString(&cbody, temp.data);
			continue;
		}

		if (attr->attnum == SelfItemPointerAttributeNumber)
		{
			appendStringInfo(
				&tbody,
				"  /* %s system column */\n"
				"  tup_isnull[%d] = !t_self;\n"
				"  if (t_self)\n"
				"  {\n"
				"    tup_values[%d] = PointerGetDatum(tup_extra);\n"
				"    memcpy(tup_extra, t_self, sizeof(ItemPointerData));\n"
				"    tup_extra += MAXALIGN(sizeof(ItemPointerData));\n"
				"  }\n",
				NameStr(attr->attname), i, i);
		}
		else
		{
			appendStringInfo(
				&tbody,
				"  /* %s system column */\n"
				"  tup_isnull[%d] = !htup;\n"
				"  if (!htup)\n"
				"    tup_values[%d] = kern_getsysatt_%s(htup);\n",
				NameStr(attr->attname),
				i, i, NameStr(attr->attname));
		}
		appendStringInfo(
			&cbody,
			"  /* %s system column */\n"
			"  addr = kern_get_datum_column(kds_src,kds_src->ncols%d,src_index);\n"
			"  tup_isnull[%d] = !addr;\n",
			NameStr(attr->attname),
			attr->attnum, i);
		if (!attr->attbyval)
			appendStringInfo(
				&cbody,
				"  tup_values[%d] = PointerGetDatum(addr);\n",
				i);
		else
			appendStringInfo(
				&cbody,
				"  tup_values[%d] = READ_INT%d_PTR(addr);\n",
				i, 8 * attr->attlen);
	}

	/*
	 * Extract regular tuples
	 */
	resetStringInfo(&temp);
	appendStringInfoString(
		&temp,
		"  EXTRACT_HEAP_TUPLE_BEGIN(curr, kds_src, htup);\n");

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];
		bool		referenced = false;

		dtype = pgstrom_devtype_lookup(attr->atttypid);
		k = attr->attnum - FirstLowInvalidHeapAttributeNumber;

		/* Put values on tup_values/tup_isnull if referenced */
		for (j=0; j < list_length(tlist_dev); j++)
		{
			if (varremaps[j] != attr->attnum)
				continue;

			/* tuple */
			if (attr->attbyval)
			{
				appendStringInfo(
					&temp,
					"  tup_isnull[%d] = !curr;\n"
					"  if (curr)\n"
					"    tup_values[%d] = READ_INT%d_PTR(curr);\n",
					j, j, 8 * attr->attlen);
			}
			else
			{
				appendStringInfo(
					&temp,
					"  tup_isnull[%d] = !curr;\n"
					"  if (curr)\n"
					"    tup_values[%d] = PointerGetDatum(curr);\n",
					j, j);
			}

			/* column */
			if (!referenced)
				appendStringInfo(
					&cbody,
					"  addr = kern_get_datum_column(kds_src,%u,src_index);\n",
					attr->attnum - 1);

			if (attr->attbyval)
			{
				appendStringInfo(
					&cbody,
					"  tup_isnull[%d] = !addr;\n"
					"  if (addr)\n"
					"    tup_values[%d] = READ_INT%d_PTR(addr);\n",
					j, j, 8 * attr->attlen);
			}
			else
			{
				appendStringInfo(
					&cbody,
					"  tup_isnull[%d] = !addr;\n"
					"  if (addr)\n"
					"    tup_values[%d] = PointerGetDatum(addr);\n",
					j, j);
			}
			referenced = true;
		}
		/* Load values to KVAR_xx */
		k = attr->attnum - FirstLowInvalidHeapAttributeNumber;
		if (bms_is_member(k, varattnos))
		{
			/* tuple */
			appendStringInfo(
				&temp,
				"  KVAR_%u = pg_%s_datum_ref(kcxt,curr);\n",
				attr->attnum,
				dtype->type_name);

			/* column */
			if (!referenced)
				appendStringInfo(
					&cbody,
					"  addr = kern_get_datum_column(kds_src,%u,src_index);\n",
					attr->attnum - 1);
			appendStringInfo(
				&cbody,
				"  KVAR_%u = pg_%s_datum_ref(kcxt,addr);\n",
				attr->attnum,
				dtype->type_name);
			referenced = true;
		}

		if (referenced)
		{
			appendStringInfoString(&tbody, temp.data);
			resetStringInfo(&temp);
			has_extract_tuple = true;
		}
		appendStringInfoString(
			&temp,
			"  EXTRACT_HEAP_TUPLE_NEXT(curr);\n");
	}
	if (has_extract_tuple)
		appendStringInfoString(
			&tbody,
			"  EXTRACT_HEAP_TUPLE_END();\n"
			"\n");

	/*
	 * step.3 - execute expression node, then store the result onto KVAR_xx
	 */
    foreach (lc, tlist_dev)
    {
        TargetEntry    *tle = lfirst(lc);
		Oid				type_oid;

		if (IsA(tle->expr, Var))
			continue;
		/* NOTE: Const/Param are once loaded to expr_%u variable. */
		type_oid = exprType((Node *)tle->expr);
		dtype = pgstrom_devtype_lookup(type_oid);
		if (!dtype)
			elog(ERROR, "Bug? device supported type is missing: %s",
				 format_type_be(type_oid));

		resetStringInfo(&temp);
		appendStringInfo(
			&temp,
			"  pg_%s_t expr_%u_v;\n",
			dtype->type_name,
			tle->resno);
		appendStringInfoString(&tdecl, temp.data);
		appendStringInfoString(&cdecl, temp.data);

		resetStringInfo(&temp);
		appendStringInfo(
			&temp,
			"  expr_%u_v = %s;\n",
			tle->resno,
			pgstrom_codegen_expression((Node *) tle->expr, context));
		appendStringInfoString(&tbody, temp.data);
        appendStringInfoString(&cbody, temp.data);
	}

	/*
	 * step.5 - Store the expressions on the slot.
	 */
	resetStringInfo(&temp);
	foreach (lc, tlist_dev)
	{
		TargetEntry	   *tle = lfirst(lc);
		Oid				type_oid;

		/* host pointer should be already set */
		if (varremaps[tle->resno-1])
		{
			Assert(IsA(tle->expr, Var));
			continue;
		}

		type_oid = exprType((Node *)tle->expr);
		dtype = pgstrom_devtype_lookup(type_oid);
		if (!dtype)
			elog(ERROR, "Bug? device supported type is missing: %u", type_oid);

		appendStringInfo(
			&temp,
			"  tup_isnull[%d] = expr_%u_v.isnull;\n",
			tle->resno - 1, tle->resno);

		if (dtype->type_byval)
		{
			appendStringInfo(
				&temp,
				"  if (!expr_%u_v.isnull)\n"
				"    tup_values[%d] = pg_%s_as_datum(&expr_%u_v.value);\n",
				tle->resno,
				tle->resno - 1,
				dtype->type_name,
				tle->resno);
		}
		else if (dtype->extra_sz > 0)
		{
			appendStringInfo(
				&temp,
				"  if (!expr_%u_v.isnull)\n"
				"  {\n"
				"    len = pg_%s_datum_store(kcxt,tup_extra,expr_%u_v);\n"
				"    tup_values[%d] = PointerGetDatum(tup_extra);\n"
				"    tup_extra += MAXALIGN(len);\n"
				"  }\n",
				tle->resno,
				dtype->type_name, tle->resno,
				tle->resno - 1);
			extra_size += MAXALIGN(dtype->extra_sz);
		}
		else
		{
			appendStringInfo(
				&temp,
				"  if (!expr_%u_v.isnull)\n"
				"    tup_values[%d] = PointerGetDatum(expr_%u_v.value);\n",
				tle->resno,
				tle->resno - 1,
				tle->resno);
		}
	}
	appendStringInfo(&tbody, "%s}\n", temp.data);
	appendStringInfo(&cbody, "%s}\n", temp.data);

	/* parameter references */
	pgstrom_codegen_param_declarations(&tdecl, context);
	pgstrom_codegen_param_declarations(&cdecl, context);

	/* OK, write back the kernel source */
	appendStringInfo(
		kern,
		"%s\n%s\n%s\n%s",
		tdecl.data,
		tbody.data,
		cdecl.data,
		cbody.data);
	list_free(tlist_dev);
	pfree(temp.data);
	pfree(tdecl.data);
	pfree(cdecl.data);
	pfree(tbody.data);
	pfree(cbody.data);
}

/*
 * add_unique_expression - adds an expression node on the supplied
 * target-list, then returns true, if new target-entry was added.
 */
bool
add_unique_expression(Expr *expr, List **p_targetlist, bool resjunk)
{
	TargetEntry	   *tle;
	ListCell	   *lc;
	AttrNumber		resno;

	foreach (lc, *p_targetlist)
	{
		tle = (TargetEntry *) lfirst(lc);
		if (equal(expr, tle->expr))
			return false;
	}
	/* Not found, so add this expression */
	resno = list_length(*p_targetlist) + 1;
	tle = makeTargetEntry(copyObject(expr), resno, NULL, resjunk);
	*p_targetlist = lappend(*p_targetlist, tle);

	return true;
}

/*
 * build_gpuscan_projection
 *
 * It checks whether the GpuProjection of GpuScan makes sense.
 * If executor may require the physically compatible tuple as result,
 * we don't need to have a projection in GPU side.
 */
typedef struct
{
	Index		scanrelid;
	TupleDesc	tupdesc;
	int			attnum;
	int			depth;
	bool		compatible_tlist;
	List	   *tlist_dev;
} build_gpuscan_projection_context;

static bool
build_gpuscan_projection_walker(Node *node, void *__context)
{
	build_gpuscan_projection_context *context = __context;
	TupleDesc	tupdesc = context->tupdesc;
	int			attnum = context->attnum;
	int			depth_saved;
	bool		retval;

	if (IsA(node, Var))
	{
		Var	   *varnode = (Var *) node;

		/* if these Asserts fail, planner messed up */
		Assert(varnode->varno == context->scanrelid);
		Assert(varnode->varlevelsup == 0);

		/* GPU projection cannot contain whole-row var */
		if (varnode->varattno == InvalidAttrNumber)
			return true;

		/*
		 * check whether the original tlist matches the physical layout
		 * of the base relation. GPU can reorder the var reference
		 * regardless of the data-type support.
		 */
		if (varnode->varattno != context->attnum || attnum > tupdesc->natts)
			context->compatible_tlist = false;
		else
		{
			Form_pg_attribute	attr = tupdesc->attrs[attnum-1];

			/* should not be a reference to dropped columns */
			Assert(!attr->attisdropped);
			/* See the logic in tlist_matches_tupdesc */
			if (varnode->vartype != attr->atttypid ||
				(varnode->vartypmod != attr->atttypmod &&
				 varnode->vartypmod != -1))
				context->compatible_tlist = false;
		}
		/* add a primitive var-node on the tlist_dev */
		if (!add_unique_expression((Expr *) varnode,
								   &context->tlist_dev, false))
			context->compatible_tlist = false;
		return false;
	}
	else if (context->depth == 0 && (IsA(node, Const) || IsA(node, Param)))
	{
		/* no need to have top-level constant values on the device side */
		context->compatible_tlist = false;
		return false;
	}
	else if (pgstrom_device_expression((Expr *) node))
	{
		/* add device executable expression onto the tlist_dev */
		add_unique_expression((Expr *) node, &context->tlist_dev, false);
		/* of course, it is not a physically compatible tlist */
		context->compatible_tlist = false;
		return false;
	}
	/*
	 * walks down if expression is host-only.
	 */
	depth_saved = context->depth++;
	retval = expression_tree_walker(node, build_gpuscan_projection_walker,
									context);
	context->depth = depth_saved;
	context->compatible_tlist = false;
	return retval;
}

static List *
build_gpuscan_projection(Index scanrelid,
						 Relation relation,
						 List *tlist,
						 List *host_quals,
						 List *dev_quals)
{
	build_gpuscan_projection_context context;
	ListCell   *lc;

	memset(&context, 0, sizeof(context));
	context.scanrelid = scanrelid;
	context.tupdesc = RelationGetDescr(relation);
	context.attnum = 0;
	context.depth = 0;
	context.tlist_dev = NIL;
	context.compatible_tlist = true;

	foreach (lc, tlist)
	{
		TargetEntry	   *tle = lfirst(lc);

		context.attnum++;
		if (build_gpuscan_projection_walker((Node *)tle->expr, &context))
			return NIL;
		Assert(context.depth == 0);
	}

	/* Is the tlist shorter than relation's definition? */
	if (RelationGetNumberOfAttributes(relation) != context.attnum)
		context.compatible_tlist = false;

	/*
	 * Host quals needs 
	 */
	if (host_quals)
	{
		List	   *vars_list = pull_vars_of_level((Node *)host_quals, 0);

		foreach (lc, vars_list)
		{
			Var	   *var = lfirst(lc);
			if (var->varattno == InvalidAttrNumber)
				return NIL;		/* no whole-row support */
			add_unique_expression((Expr *)var, &context.tlist_dev, false);
		}
		list_free(vars_list);
	}

	/*
	 * Device quals need junk var-nodes
	 */
	if (dev_quals)
	{
		List	   *vars_list = pull_vars_of_level((Node *)dev_quals, 0);

		foreach (lc, vars_list)
		{
			Var	   *var = lfirst(lc);
			if (var->varattno == InvalidAttrNumber)
				return NIL;		/* no whole-row support */
			add_unique_expression((Expr *)var, &context.tlist_dev, true);
		}
		list_free(vars_list);
	}

	/*
	 * At this point, device projection is "executable".
	 * However, if compatible_tlist == true, it implies the upper node
	 * expects physically compatible tuple, thus, it is uncertain whether
	 * we should run GpuProjection for this GpuScan.
	 */
	if (context.compatible_tlist)
		return NIL;
	return context.tlist_dev;
}

/*
 * bufsz_estimate_gpuscan_projection - GPU Projection may need larger
 * destination buffer than the source buffer. 
 */
static void
bufsz_estimate_gpuscan_projection(RelOptInfo *baserel,
								  Relation relation,
								  List *tlist_proj,
								  cl_int *p_proj_tuple_sz,
								  cl_int *p_proj_extra_sz)
{
	TupleDesc	tupdesc = RelationGetDescr(relation);
	cl_int		proj_tuple_sz = 0;
	cl_int		proj_extra_sz = 0;
	int			j, nattrs;
	ListCell   *lc;

	if (!tlist_proj)
	{
		proj_tuple_sz = offsetof(kern_tupitem,
								 htup.t_bits[BITMAPLEN(tupdesc->natts)]);
		if (tupdesc->tdhasoid)
			proj_tuple_sz += sizeof(Oid);
		proj_tuple_sz = MAXALIGN(proj_tuple_sz);

		for (j=0; j < tupdesc->natts; j++)
		{
			Form_pg_attribute attr = tupdesc->attrs[j];

			proj_tuple_sz = att_align_nominal(proj_tuple_sz, attr->attalign);
			proj_tuple_sz += baserel->attr_widths[j + 1 - baserel->min_attr];
		}
		proj_tuple_sz = MAXALIGN(proj_tuple_sz);
		goto out;
	}

	nattrs = list_length(tlist_proj);
	proj_tuple_sz = offsetof(kern_tupitem,
							 htup.t_bits[BITMAPLEN(nattrs)]);
	proj_tuple_sz = MAXALIGN(proj_tuple_sz);
	foreach (lc, tlist_proj)
	{
		TargetEntry *tle = lfirst(lc);
		Oid		type_oid = exprType((Node *)tle->expr);
		int32	type_mod = exprTypmod((Node *)tle->expr);
		int16	typlen;
		bool	typbyval;
		char	typalign;

		/* alignment */
		get_typlenbyvalalign(type_oid, &typlen, &typbyval, &typalign);
		proj_tuple_sz = att_align_nominal(proj_tuple_sz, typalign);
		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;

			Assert(var->vartype == type_oid &&
				   var->vartypmod == type_mod);
			Assert(var->varno == baserel->relid &&
				   var->varattno >= baserel->min_attr &&
				   var->varattno <= baserel->max_attr);
			proj_tuple_sz += baserel->attr_widths[var->varattno -
												  baserel->min_attr];
		}
		else if (IsA(tle->expr, Const))
		{
			Const  *con = (Const *) tle->expr;

			/* raw-data is the most reliable information source :) */
			if (!con->constisnull)
				proj_tuple_sz += (con->constlen > 0
								  ? con->constlen
								  : VARSIZE_ANY(con->constvalue));
		}
		else
		{
			devtype_info   *dtype;

			dtype = pgstrom_devtype_lookup(type_oid);
			if (!dtype)
				elog(ERROR, "device type %u lookup failed", type_oid);
			proj_tuple_sz += (dtype->type_length > 0
							  ? dtype->type_length
							  : get_typavgwidth(type_oid, type_mod));
			if (!dtype->type_byval)
			{
				if (dtype->extra_sz == 0)
					elog(ERROR, "Bug? device type '%s' has indirect/varlena definition but no extra-size parameter at expression of: %s",
						 dtype->type_name,
						 nodeToString(tle->expr));
				proj_extra_sz += MAXALIGN(dtype->extra_sz);
			}
		}
	}
	proj_tuple_sz = MAXALIGN(proj_tuple_sz);
out:
	*p_proj_tuple_sz = proj_tuple_sz;
	*p_proj_extra_sz = proj_extra_sz;
}

/*
 * PlanGpuScanPath - construction of a new GpuScan plan node
 */
static Plan *
PlanGpuScanPath(PlannerInfo *root,
				RelOptInfo *baserel,
				CustomPath *best_path,
				List *tlist,
				List *clauses,
				List *custom_children)
{
	GpuScanInfo	   *gs_info = linitial(best_path->custom_private);
	CustomScan	   *cscan;
	RangeTblEntry  *rte;
	Relation		relation;
	List		   *host_quals = NIL;
	List		   *dev_quals = NIL;
	Expr		   *dev_quals_expr = NULL;
	List		   *tlist_dev = NIL;
	List		   *ccache_refs = NIL;
	ListCell	   *cell;
	Bitmapset	   *varattnos = NULL;
	cl_int			proj_tuple_sz = 0;
	cl_int			proj_extra_sz = 0;
	cl_int			i, j;
	StringInfoData	kern;
	StringInfoData	source;
	codegen_context	context;

	/* It should be a base relation */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);
	Assert(custom_children == NIL);

	/*
	 * Distribution of clauses into device executable and others.
	 *
	 * NOTE: Why we don't sort out on Path construction stage is,
	 * create_scan_plan() may add parameterized scan clause, thus
	 * we have to delay the final decision until this point.
	 */
	foreach (cell, clauses)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		if (exprType((Node *)rinfo->clause) != BOOLOID)
			elog(ERROR, "Bug? clause on GpuScan does not have BOOL type");
		if (!pgstrom_device_expression(rinfo->clause))
			host_quals = lappend(host_quals, rinfo);
		else
			dev_quals = lappend(dev_quals, rinfo);
	}
	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	host_quals = extract_actual_clauses(host_quals, false);
	dev_quals = extract_actual_clauses(dev_quals, false);
	if (dev_quals)
		dev_quals_expr = make_flat_ands_explicit(dev_quals);

	/*
	 * Code construction for the CUDA kernel code
	 */
	rte = planner_rt_fetch(baserel->relid, root);
	relation = heap_open(rte->relid, NoLock);

	initStringInfo(&kern);
	initStringInfo(&source);
	pgstrom_init_codegen_context(&context);
	codegen_gpuscan_quals(&kern, &context, baserel->relid, dev_quals_expr);
	tlist_dev = build_gpuscan_projection(baserel->relid, relation,
										 tlist,
										 host_quals,
										 dev_quals);
	bufsz_estimate_gpuscan_projection(baserel, relation, tlist_dev,
									  &proj_tuple_sz,
									  &proj_extra_sz);
	context.param_refs = NULL;
	codegen_gpuscan_projection(&kern, &context,
							   baserel->relid,
							   relation,
							   tlist_dev ? tlist_dev : tlist);
	heap_close(relation, NoLock);
	appendStringInfoString(&source, kern.data);
	pfree(kern.data);

	/* pickup referenced attributes */
	pull_varattnos((Node *)dev_quals, baserel->relid, &varattnos);
	pull_varattnos((Node *)host_quals, baserel->relid, &varattnos);
	pull_varattnos((Node *)tlist, baserel->relid, &varattnos);
	for (i = bms_first_member(varattnos);
		 i >= 0;
		 i = bms_next_member(varattnos, i))
	{
		j = i + FirstLowInvalidHeapAttributeNumber;
		ccache_refs = lappend_int(ccache_refs, j);
	}

	/*
	 * Construction of GpuScanPlan node; on top of CustomPlan node
	 */
	cscan = makeNode(CustomScan);
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = host_quals;
	cscan->scan.plan.lefttree = NULL;
	cscan->scan.plan.righttree = NULL;
	cscan->scan.scanrelid = baserel->relid;
	cscan->flags = best_path->flags;
	cscan->methods = &gpuscan_plan_methods;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = tlist_dev;

	gs_info->kern_source = source.data;
	gs_info->extra_flags = context.extra_flags |
		DEVKERNEL_NEEDS_DYNPARA | DEVKERNEL_NEEDS_GPUSCAN;
	gs_info->proj_tuple_sz = proj_tuple_sz;
	gs_info->proj_extra_sz = proj_extra_sz;
	gs_info->ccache_refs = ccache_refs;
	gs_info->used_params = context.used_params;
	gs_info->dev_quals = dev_quals;
	form_gpuscan_info(cscan, gs_info);

	return &cscan->scan.plan;
}

/*
 * pgstrom_pullup_outer_scan - pull up outer_path if it is a simple relation
 * scan with device executable qualifiers.
 */
bool
pgstrom_pullup_outer_scan(const Path *outer_path,
						  Index *p_outer_relid,
						  Expr **p_outer_quals)
{
	RelOptInfo *baserel = outer_path->parent;
	PathTarget *outer_target = outer_path->pathtarget;
	List	   *outer_quals = NIL;
	ListCell   *lc;

	if (!enable_pullup_outer_scan)
		return false;

	for (;;)
	{
		if (outer_path->pathtype == T_SeqScan)
			break;	/* OK */
		if (pgstrom_path_is_gpuscan(outer_path))
			break;	/* OK, only if GpuScan */
		if (outer_path->pathtype == T_Result)
		{
			ProjectionPath *ppath = (ProjectionPath *) outer_path;

			if (ppath->dummypp)
			{
				outer_path = ppath->subpath;
				continue;	/* Dive into one more deep level */
			}
		}
		return false;	/* Elsewhere, we cannot pull-up the scan path */
	}

	/* qualifier has to be device executable */
	foreach (lc, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(lc);

		if (!pgstrom_device_expression(rinfo->clause))
			return false;
		outer_quals = lappend(outer_quals, rinfo->clause);
	}

	/* target entry has to be */
	foreach (lc, outer_target->exprs)
	{
		Expr   *expr = (Expr *) lfirst(lc);

		if (IsA(expr, Var))
		{
			Var	   *var = (Var *) expr;

			/* we don't support whole-row reference */
			if (var->varattno == InvalidAttrNumber)
				return false;
		}
		else if (!pgstrom_device_expression(expr))
			return false;
	}
	*p_outer_relid = baserel->relid;
	*p_outer_quals = (outer_quals != NIL
					  ? make_flat_ands_explicit(outer_quals)
					  : NULL);
	return true;
}

/*
 * pgstrom_path_is_gpuscan
 *
 * It returns true, if supplied path node is gpuscan.
 */
bool
pgstrom_path_is_gpuscan(const Path *path)
{
	if (IsA(path, CustomPath) &&
		path->pathtype == T_CustomScan &&
		((CustomPath *) path)->methods == &gpuscan_path_methods)
		return true;
	return false;
}

/*
 * pgstrom_plan_is_gpuscan
 *
 * It returns true, if supplied plan node is gpuscan.
 */
bool
pgstrom_plan_is_gpuscan(const Plan *plan)
{
	if (IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &gpuscan_plan_methods)
		return true;
	return false;
}

/*
 * pgstrom_planstate_is_gpuscan
 *
 * It returns true, if supplied planstate node is gpuscan
 */
bool
pgstrom_planstate_is_gpuscan(const PlanState *ps)
{
	if (IsA(ps, CustomScanState) &&
		((CustomScanState *) ps)->methods == &gpuscan_exec_methods)
		return true;
	return false;
}

/*
 * fixup_varnode_to_origin
 */
static Node *
fixup_varnode_to_origin(Node *node, List *custom_scan_tlist)
{
	if (!node)
		return NULL;
	if (IsA(node, Var))
	{
		Var	   *varnode = (Var *)node;
		TargetEntry *tle;

		if (custom_scan_tlist != NIL)
		{
			Assert(varnode->varno == INDEX_VAR);
			Assert(varnode->varattno >= 1 &&
				   varnode->varattno <= list_length(custom_scan_tlist));
			tle = list_nth(custom_scan_tlist,
						   varnode->varattno - 1);
			return (Node *)copyObject(tle->expr);
		}
		Assert(!IS_SPECIAL_VARNO(varnode->varno));
	}
	return expression_tree_mutator(node, fixup_varnode_to_origin,
								   (void *)custom_scan_tlist);
}

/*
 * assign_gpuscan_session_info
 *
 * Gives some definitions to the static portion of GpuScan implementation
 */
void
assign_gpuscan_session_info(StringInfo buf, GpuTaskState *gts)
{
	CustomScan	   *cscan = (CustomScan *)gts->css.ss.ps.plan;

	if (pgstrom_plan_is_gpuscan((Plan *) cscan))
	{
		GpuScanState   *gss = (GpuScanState *)gts;
		TupleTableSlot *slot = gts->css.ss.ss_ScanTupleSlot;
		TupleDesc		tupdesc = slot->tts_tupleDescriptor;

		appendStringInfo(
			buf,
			"#define GPUSCAN_KERNEL_REQUIRED                1\n");
		if (gss->dev_projection)
			appendStringInfo(
				buf,
				"#define GPUSCAN_HAS_DEVICE_PROJECTION          1\n");
		appendStringInfo(
			buf,
			"#define GPUSCAN_DEVICE_PROJECTION_NFIELDS      %d\n"
			"#define GPUSCAN_DEVICE_PROJECTION_EXTRA_SIZE   %d\n",
			tupdesc->natts,
			gss->proj_extra_sz);

		if (gss->dev_quals)
		{
			appendStringInfoString(
				buf,
				"#define GPUSCAN_HAS_WHERE_QUALS                1\n");
		}
	}
}

/*
 * gpuscan_create_scan_state - allocation of GpuScanState
 */
static Node *
gpuscan_create_scan_state(CustomScan *cscan)
{
	GpuScanState   *gss = MemoryContextAllocZero(CurTransactionContext,
												 sizeof(GpuScanState));
	/* Set tag and executor callbacks */
	NodeSetTag(gss, T_CustomScanState);
	gss->gts.css.flags = cscan->flags;
	if (cscan->methods == &gpuscan_plan_methods)
		gss->gts.css.methods = &gpuscan_exec_methods;
	else
		elog(ERROR, "Bug? unexpected CustomPlanMethods");

	return (Node *) gss;
}

/*
 * ExecInitGpuScan
 */
static void
ExecInitGpuScan(CustomScanState *node, EState *estate, int eflags)
{
	Relation		scan_rel = node->ss.ss_currentRelation;
	GpuScanState   *gss = (GpuScanState *) node;
	CustomScan	   *cscan = (CustomScan *)node->ss.ps.plan;
	GpuScanInfo	   *gs_info = deform_gpuscan_info(cscan);
	GpuContext	   *gcontext;
	bool			explain_only = ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0);
	List		   *dev_tlist = NIL;
	List		   *dev_quals_raw;
	Expr		   *dev_quals_expr;
	ListCell	   *lc;
	StringInfoData	kern_define;
	ProgramId		program_id;

	/* gpuscan should not have inner/outer plan right now */
	Assert(scan_rel != NULL);
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/* setup GpuContext for CUDA kernel execution */
	gcontext = AllocGpuContext(-1, false);
	if (!explain_only)
		ActivateGpuContext(gcontext);
	gss->gts.gcontext = gcontext;

	/*
	 * Re-initialization of scan tuple-descriptor and projection-info,
	 * because commit 1a8a4e5cde2b7755e11bde2ea7897bd650622d3e of
	 * PostgreSQL makes to assign result of ExecTypeFromTL() instead
	 * of ExecCleanTypeFromTL; that leads incorrect projection.
	 * So, we try to remove junk attributes from the scan-descriptor.
	 */
	if (cscan->custom_scan_tlist != NIL)
	{
		TupleDesc		scan_tupdesc;

		scan_tupdesc = ExecCleanTypeFromTL(cscan->custom_scan_tlist, false);
		ExecAssignScanType(&gss->gts.css.ss, scan_tupdesc);
		ExecAssignScanProjectionInfoWithVarno(&gss->gts.css.ss, INDEX_VAR);
		/* valid @custom_scan_tlist means device projection is required */
		gss->dev_projection = true;
	}
	/* setup common GpuTaskState fields */
	pgstromInitGpuTaskState(&gss->gts,
							gcontext,
							GpuTaskKind_GpuScan,
							gs_info->ccache_refs,
							gs_info->used_params,
							estate);
	gss->gts.cb_next_task   = gpuscan_next_task;
	gss->gts.cb_next_tuple  = gpuscan_next_tuple;
	gss->gts.cb_switch_task = gpuscan_switch_task;
	gss->gts.cb_process_task = gpuscan_process_task;
	gss->gts.cb_release_task = gpuscan_release_task;
	/* estimated number of rows per block */
	gss->gts.outer_nrows_per_block = gs_info->nrows_per_block;

	/*
	 * initialize device qualifiers/projection stuff, for CPU fallback
	 *
	 * @dev_quals for CPU fallback references raw tuples regardless of device
	 * projection. So, it must be initialized to reference the raw tuples.
	 */
	dev_quals_raw = (List *)fixup_varnode_to_origin((Node *)gs_info->dev_quals,
													cscan->custom_scan_tlist);
	dev_quals_expr = make_ands_explicit(dev_quals_raw);
#if PG_VERSION_NUM < 100000
	gss->dev_quals = list_make1(ExecInitExpr(dev_quals_expr,
											 &gss->gts.css.ss.ps));
#else
	gss->dev_quals = ExecInitExpr(dev_quals_expr,
								  &gss->gts.css.ss.ps);
#endif

	foreach (lc, cscan->custom_scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);

		if (tle->resjunk)
			break;
#if PG_VERSION_NUM < 100000
		/*
		 * Caution: before PG v10, the targetList was a list of ExprStates;
		 * now it should be the planner-created targetlist.
		 * See, ExecBuildProjectionInfo
		 */
		dev_tlist = lappend(dev_tlist, ExecInitExpr((Expr *) tle,
													&gss->gts.css.ss.ps));
#else
		dev_tlist = lappend(dev_tlist, tle);
#endif
	}

	/* device projection related resource consumption */
	gss->proj_tuple_sz = gs_info->proj_tuple_sz;
	gss->proj_extra_sz = gs_info->proj_extra_sz;
	/* 'tableoid' should not change during relation scan */
	gss->scan_tuple.t_tableOid = RelationGetRelid(scan_rel);
	/* initialize resource for CPU fallback */
	gss->base_slot = MakeSingleTupleTableSlot(RelationGetDescr(scan_rel));
	if (gss->dev_projection)
	{
		ExprContext	   *econtext = gss->gts.css.ss.ps.ps_ExprContext;
		TupleTableSlot *scan_slot = gss->gts.css.ss.ss_ScanTupleSlot;

		gss->base_proj = ExecBuildProjectionInfo(dev_tlist,
												 econtext,
												 scan_slot,
#if PG_VERSION_NUM >= 100000
												 &gss->gts.css.ss.ps,
#endif
												 RelationGetDescr(scan_rel));
	}
	else
		gss->base_proj = NULL;

	/* Get CUDA program and async build if any */
	initStringInfo(&kern_define);
	pgstrom_build_session_info(&kern_define,
							   &gss->gts,
							   gs_info->extra_flags);
	program_id = pgstrom_create_cuda_program(gcontext,
											 gs_info->extra_flags,
											 gs_info->kern_source,
											 kern_define.data,
											 false,
											 explain_only);
	gss->gts.program_id = program_id;
	pfree(kern_define.data);
}

/*
 * gpuscan_exec_recheck
 *
 * Routine of EPQ recheck on GpuScan. If any, HostQual shall be checked
 * on ExecScan(), all we have to do here is recheck of device qualifier.
 */
static bool
ExecReCheckGpuScan(CustomScanState *node, TupleTableSlot *slot)
{
	GpuScanState   *gss = (GpuScanState *) node;
	ExprContext	   *econtext = node->ss.ps.ps_ExprContext;
	HeapTuple		tuple = slot->tts_tuple;
	TupleTableSlot *scan_slot	__attribute__((unused));
	ExprDoneCond	is_done		__attribute__((unused));
	bool			retval;

	/*
	 * Does the tuple meet the device qual condition?
	 * Please note that we should not use the supplied 'slot' as is,
	 * because it may not be compatible with relations's definition
	 * if device projection is valid.
	 */
	ExecStoreTuple(tuple, gss->base_slot, InvalidBuffer, false);
	econtext->ecxt_scantuple = gss->base_slot;
	ResetExprContext(econtext);

#if PG_VERSION_NUM < 100000
	retval = ExecQual(gss->dev_quals, econtext, false);
#else
	retval = ExecQual(gss->dev_quals, econtext);
#endif
	if (retval && gss->base_proj)
	{
		/*
		 * NOTE: If device projection is valid, we have to adjust the
		 * supplied tuple (that follows the base relation's definition)
		 * into ss_ScanTupleSlot, to fit tuple descriptor of the supplied
		 * 'slot'.
		 */
		Assert(!slot->tts_shouldFree);
		ExecClearTuple(slot);
#if PG_VERSION_NUM < 100000
		scan_slot = ExecProject(gss->base_proj, &is_done);
#else
		scan_slot = ExecProject(gss->base_proj);
#endif
		Assert(scan_slot == slot);
	}
	return retval;
}

/*
 * ExecGpuScan
 */
static TupleTableSlot *
ExecGpuScan(CustomScanState *node)
{
	GpuScanState   *gss = (GpuScanState *) node;

	if (!gss->gs_sstate)
	{
		gss->gs_sstate = createGpuScanSharedState(gss, NULL, NULL);
		gss->gs_rtstat = &gss->gs_sstate->gs_rtstat;
	}
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) pgstromExecGpuTaskState,
					(ExecScanRecheckMtd) ExecReCheckGpuScan);
}

/*
 * ExecEndGpuScan
 */
static void
ExecEndGpuScan(CustomScanState *node)
{
	GpuScanState	   *gss = (GpuScanState *)node;

	/* wait for completion of asynchronous GpuTaks */
	SynchronizeGpuContext(gss->gts.gcontext);
	/* reset fallback resources */
	if (gss->base_slot)
		ExecDropSingleTupleTableSlot(gss->base_slot);
	pgstromReleaseGpuTaskState(&gss->gts);
}

/*
 * ExecReScanGpuScan
 */
static void
ExecReScanGpuScan(CustomScanState *node)
{
	GpuScanState	   *gss = (GpuScanState *) node;

	/* wait for completion of asynchronous GpuTaks */
	SynchronizeGpuContext(gss->gts.gcontext);
	/* reset shared state */
	resetGpuScanSharedState(gss);
	/* common rescan handling */
	pgstromRescanGpuTaskState(&gss->gts);
	/* rewind the position to read */
	gpuscanRewindScanChunk(&gss->gts);
}

/*
 * ExecGpuScanEstimateDSM - return required size of shared memory
 */
Size
ExecGpuScanEstimateDSM(CustomScanState *node,
					   ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	Size		required;

	required = MAXALIGN(sizeof(GpuScanSharedState));
	if (node->ss.ss_currentRelation)
		required += heap_parallelscan_estimate(estate->es_snapshot);
	return required;
}

/*
 * ExecGpuScanInitDSM - initialize the coordinate memory on the master backend
 */
void
ExecGpuScanInitDSM(CustomScanState *node,
				   ParallelContext *pcxt,
				   void *coordinate)
{
	GpuScanState   *gss = (GpuScanState *) node;
	Relation		relation = node->ss.ss_currentRelation;
	EState		   *estate = gss->gts.css.ss.ps.state;

	gss->gts.pcxt = pcxt;
	/* NOTE: GpuJoin or GpuPreAgg may also call this function */
	if (pgstrom_planstate_is_gpuscan(&gss->gts.css.ss.ps))
	{
		gss->gs_sstate = createGpuScanSharedState(gss, pcxt, coordinate);
		gss->gs_rtstat = &gss->gs_sstate->gs_rtstat;
		on_dsm_detach(pcxt->seg,
					  SynchronizeGpuContextOnDSMDetach,
					  PointerGetDatum(gss->gts.gcontext));
		coordinate = ((char *)coordinate + 
					  MAXALIGN(sizeof(GpuScanSharedState)));
	}

	if (relation)
	{
		ParallelHeapScanDescData *pscan = coordinate;
		/* setup of parallel scan descriptor */
		heap_parallelscan_initialize(pscan,
									 relation,
									 estate->es_snapshot);
		node->ss.ss_currentScanDesc =
			heap_beginscan_parallel(relation, pscan);
		/* Try to choose NVMe-Strom, if available */
		PDS_init_heapscan_state(&gss->gts, gss->gts.outer_nrows_per_block);
	}
}

/*
 * ExecGpuScanInitWorker - initialize GpuScan on the backend worker process
 */
void
ExecGpuScanInitWorker(CustomScanState *node,
					  shm_toc *toc,
					  void *coordinate)
{
	GpuScanState	   *gss = (GpuScanState *) node;
	Relation			relation = node->ss.ss_currentRelation;

	/* NOTE: GpuJoin or GpuPreAgg may also call this function */
	if (pgstrom_planstate_is_gpuscan(&gss->gts.css.ss.ps))
	{
		gss->gs_sstate = (GpuScanSharedState *)coordinate;
		gss->gs_rtstat = &gss->gs_sstate->gs_rtstat;
		on_dsm_detach(dsm_find_mapping(gss->gs_sstate->ss_handle),
					  SynchronizeGpuContextOnDSMDetach,
					  PointerGetDatum(gss->gts.gcontext));
		coordinate = ((char *)coordinate +
					  MAXALIGN(sizeof(GpuScanSharedState)));
	}

	if (relation)
	{
		ParallelHeapScanDescData *pscan = coordinate;
		/* begin parallel sequential scan */
		gss->gts.css.ss.ss_currentScanDesc =
			heap_beginscan_parallel(relation, pscan);
		/* Try to choose NVMe-Strom, if available */
		PDS_init_heapscan_state(&gss->gts, gss->gts.outer_nrows_per_block);
	}
}

#if PG_VERSION_NUM >= 100000
static void
ExecShutdownGpuScan(CustomScanState *node)
{
	GpuScanState   *gss = (GpuScanState *) node;
	GpuScanRuntimeStat *gs_rtstat_old = gss->gs_rtstat;

	/*
	 * Note that GpuScan may not be executed if GpuScan node is located
	 * under the GpuJoin at parallel background worker context, because
	 * only master process of GpuJoin is responsible to run inner nodes
	 * to load inner tuples. In other words, any inner plan nodes are
	 * not executed at the parallel worker context.
	 * So, we may not have a valid GpuScanSharedState here.
	 *
	 * Elsewhere, move the statistics from DSM
	 */
	if (gs_rtstat_old)
	{
		gss->gs_rtstat = MemoryContextAlloc(CurTransactionContext,
											sizeof(GpuScanRuntimeStat));
		memcpy(gss->gs_rtstat,
			   gs_rtstat_old,
			   sizeof(GpuScanRuntimeStat));
	}
}
#endif

/*
 * ExplainGpuScan - EXPLAIN callback
 */
static void
ExplainGpuScan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpuScanState	   *gss = (GpuScanState *) node;
	GpuScanRuntimeStat *gs_rtstat = gss->gs_rtstat;
	CustomScan		   *cscan = (CustomScan *) gss->gts.css.ss.ps.plan;
	GpuScanInfo		   *gs_info = deform_gpuscan_info(cscan);
	List			   *dcontext;
	List			   *dev_proj = NIL;
	char			   *exprstr;
	ListCell		   *lc;
	uint64				nitems_filtered = 0;

	if (gs_rtstat)
	{
		nitems_filtered = pg_atomic_read_u64(&gs_rtstat->nitems_filtered);
		gss->gts.ccache_count = pg_atomic_read_u64(&gs_rtstat->ccache_count);
	}

	/* Set up deparsing context */
	dcontext = set_deparse_context_planstate(es->deparse_cxt,
											 (Node *)&gss->gts.css.ss.ps,
											 ancestors);
	/* Show device projection */
	foreach (lc, cscan->custom_scan_tlist)
	{
		TargetEntry	   *tle = lfirst(lc);

		if (!tle->resjunk)
			dev_proj = lappend(dev_proj, tle->expr);
	}

	if (dev_proj != NIL)
	{
		exprstr = deparse_expression((Node *)dev_proj, dcontext,
									 es->verbose, false);
		ExplainPropertyText("GPU Projection", exprstr, es);
	}

	/* Show device filters */
	if (gs_info->dev_quals != NIL)
	{
		Node   *dev_quals = (Node *)make_ands_explicit(gs_info->dev_quals);

		exprstr = deparse_expression(dev_quals, dcontext,
									 es->verbose, false);
		ExplainPropertyText("GPU Filter", exprstr, es);
		if (gss->gts.css.ss.ps.instrument && nitems_filtered > 0)
		{
			Instrumentation *instr = gss->gts.css.ss.ps.instrument;

			ExplainPropertyLong("Rows Removed by GPU Filter",
								nitems_filtered / instr->nloops, es);
		}
	}

	/* common portion of EXPLAIN */
	pgstromExplainGpuTaskState(&gss->gts, es);
}

/*
 * createGpuScanSharedState
 */
static GpuScanSharedState *
createGpuScanSharedState(GpuScanState *gss,
						 ParallelContext *pcxt,
						 void *dsm_addr)
{
	GpuScanSharedState *gs_sstate;
	size_t		ss_length = MAXALIGN(sizeof(GpuScanSharedState));

	Assert(!IsParallelWorker());
	if (dsm_addr)
		gs_sstate = dsm_addr;
	else
		gs_sstate = MemoryContextAlloc(CurTransactionContext, ss_length);
	memset(gs_sstate, 0, ss_length);
	gs_sstate->ss_handle = (pcxt ? dsm_segment_handle(pcxt->seg) : UINT_MAX);
	gs_sstate->ss_length = ss_length;

	return gs_sstate;
}

/*
 * resetGpuScanSharedState
 */
static void
resetGpuScanSharedState(GpuScanState *gss)
{
	/* do nothing */
}

/*
 * gpuscan_create_task - constructor of GpuScanTask
 */
static GpuScanTask *
gpuscan_create_task(GpuScanState *gss,
					pgstrom_data_store *pds_src)
{
	TupleDesc		scan_tupdesc = GTS_GET_SCAN_TUPDESC(gss);
	GpuContext	   *gcontext = gss->gts.gcontext;
	pgstrom_data_store *pds_dst = NULL;
	kern_resultbuf *kresults = NULL;
	GpuScanTask	   *gscan;
	cl_uint			nresults = 0;
	size_t			length;
	CUdeviceptr		m_deviceptr;
	CUresult		rc;

	/*
	 * allocation of destination buffer
	 */
	if (pds_src->kds.format == KDS_FORMAT_ROW && !gss->dev_projection)
		nresults = pds_src->kds.nitems;
	else
	{
		double	ntuples = pds_src->kds.nitems;
		double	proj_tuple_sz = gss->proj_tuple_sz;
		Size	length;

		if (pds_src->kds.format == KDS_FORMAT_BLOCK)
		{
			Assert(pds_src->kds.nrows_per_block > 0);
			ntuples *= (double)pds_src->kds.nrows_per_block;
		}
		length = STROMALIGN(offsetof(kern_data_store,
									 colmeta[scan_tupdesc->natts])) +
			STROMALIGN((Size)(sizeof(cl_uint) * ntuples)) +
			STROMALIGN((Size)(1.2 * proj_tuple_sz * ntuples));

		pds_dst = PDS_create_row(gcontext,
								 scan_tupdesc,
								 length);
	}

	/*
	 * allocation of pgstrom_gpuscan
	 */
	length = (STROMALIGN(offsetof(GpuScanTask, kern.kparams)) +
			  STROMALIGN(gss->gts.kern_params->length) +
			  STROMALIGN(offsetof(kern_resultbuf, results[nresults])));
	rc = gpuMemAllocManaged(gcontext,
							&m_deviceptr,
							length,
							CU_MEM_ATTACH_GLOBAL);
	if (rc != CUDA_SUCCESS)
		elog(ERROR, "failed on gpuMemAllocManaged: %s", errorText(rc));
	gscan = (GpuScanTask *) m_deviceptr;
	memset(gscan, 0, (offsetof(GpuScanTask, kern) +
					  offsetof(kern_gpuscan, kparams)));
	pgstromInitGpuTask(&gss->gts, &gscan->task);
	gscan->with_nvme_strom = (pds_src->kds.format == KDS_FORMAT_BLOCK &&
							  pds_src->nblocks_uncached > 0);
	gscan->pds_src = pds_src;
	gscan->pds_dst = pds_dst;

	/* kern_parambuf */
	memcpy(KERN_GPUSCAN_PARAMBUF(&gscan->kern),
		   gss->gts.kern_params,
		   gss->gts.kern_params->length);
	/* kern_resultbuf, if any */
	kresults = KERN_GPUSCAN_RESULTBUF(&gscan->kern);
	memset(kresults, 0, sizeof(kern_resultbuf));
	kresults->nrels = 1;
	kresults->nrooms = nresults;
	if (!pds_dst)
		gscan->kresults = kresults;

	return gscan;
}

/*
 * gpuscan_parallel_nextpage
 *
 * similar role of heap_parallelscan_nextpage in access/heap/heapam.c,
 * however, it reserves multiple pages at once, and may construct
 * a new PDS if columnar cache is valid.
 */
static pgstrom_data_store *
gpuscan_parallel_nextpage(HeapScanDesc scan,
						  GpuContext *gcontext,
						  Relids ccache_refs,
						  cl_uint nr_blocks)
{
	Relation		relation = scan->rs_rd;
	BlockNumber		sync_startpage = InvalidBlockNumber;
	BlockNumber		report_page = InvalidBlockNumber;
	BlockNumber		page = InvalidBlockNumber;
	BlockNumber		base;
	struct ccacheChunk *cc_chunk = NULL;
	pgstrom_data_store *pds_column = NULL;
	ParallelHeapScanDesc parallel_scan;

	Assert(scan->rs_numblocks == 0);
	Assert(scan->rs_parallel);
	parallel_scan = scan->rs_parallel;

retry:
	/* Grab the spinlock. */
	SpinLockAcquire(&parallel_scan->phs_mutex);

	/*
	 * If the scan's startblock has not yet been initialized, we must do so
	 * now.  If this is not a synchronized scan, we just start at block 0, but
	 * if it is a synchronized scan, we must get the starting position from
	 * the synchronized scan machinery.  We can't hold the spinlock while
	 * doing that, though, so release the spinlock, get the information we
	 * need, and retry.  If nobody else has initialized the scan in the
	 * meantime, we'll fill in the value we fetched on the second time
	 * through.
	 */
	if (parallel_scan->phs_startblock == InvalidBlockNumber)
	{
		if (!parallel_scan->phs_syncscan)
			parallel_scan->phs_startblock = 0;
		else if (sync_startpage != InvalidBlockNumber)
			parallel_scan->phs_startblock = sync_startpage;
		else
		{
			SpinLockRelease(&parallel_scan->phs_mutex);
			sync_startpage = ss_get_location(relation, scan->rs_nblocks);
			goto retry;
		}
		parallel_scan->phs_cblock = parallel_scan->phs_startblock;
	}

	/*
	 * The current block number is the next one that needs to be scanned,
	 * unless it's InvalidBlockNumber already, in which case there are no more
	 * blocks to scan.  After remembering the current value, we must advance
	 * it so that the next call to this function returns the next block to be
	 * scanned.
	 */
	page = parallel_scan->phs_cblock;
	if (page == InvalidBlockNumber)
		goto out_unlock;

	Assert(page < scan->rs_nblocks);
	Assert(nr_blocks > 0 && nr_blocks < RELSEG_SIZE);
	/* should never read multiple blocks across the segment boundary */
	if ((page / RELSEG_SIZE) != (page + nr_blocks - 1) / RELSEG_SIZE)
		nr_blocks = RELSEG_SIZE - (page % RELSEG_SIZE);
	/* terminate multiple block reads beyond end of the relation */
	if (page + nr_blocks > scan->rs_nblocks)
		nr_blocks = scan->rs_nblocks - page;
	/* terminate multiple block reads across start block */
	if (page < parallel_scan->phs_startblock &&
		page + nr_blocks >= parallel_scan->phs_startblock)
		nr_blocks = parallel_scan->phs_startblock - page;
	Assert(nr_blocks > 0);

	/* try to lookup columnar cache if any */
	base = (page + CCACHE_CHUNK_NBLOCKS-1) & ~(CCACHE_CHUNK_NBLOCKS-1);
	if (ccache_refs &&
		(page <= base && page + nr_blocks >= base) &&
		(base >= parallel_scan->phs_startblock ||
		 base + CCACHE_CHUNK_NBLOCKS <= parallel_scan->phs_startblock) &&
		(base + CCACHE_CHUNK_NBLOCKS <= scan->rs_nblocks))
	{
		cc_chunk = pgstrom_ccache_get_chunk(relation, base);
		if (cc_chunk)
		{
			nr_blocks = base - page;
			parallel_scan->phs_cblock = base + CCACHE_CHUNK_NBLOCKS;
			/*
			 * corner case: if ccache chunk is empty, we can skip blocks
			 * and try to pick up next segment if any.
			 */
			while (pgstrom_ccache_is_empty(cc_chunk))
			{
				pgstrom_ccache_put_chunk(cc_chunk);
				cc_chunk = pgstrom_ccache_get_chunk(relation,
													parallel_scan->phs_cblock);
				if (!cc_chunk)
					break;
				parallel_scan->phs_cblock += CCACHE_CHUNK_NBLOCKS;
			}
		}
	}
	if (!cc_chunk)
		parallel_scan->phs_cblock = page + nr_blocks;

	if (parallel_scan->phs_cblock >= scan->rs_nblocks)
		parallel_scan->phs_cblock = 0;
	if (parallel_scan->phs_cblock == parallel_scan->phs_startblock)
	{
		parallel_scan->phs_cblock = InvalidBlockNumber;
		report_page = parallel_scan->phs_startblock;
	}
	/* Release the lock */
out_unlock:
	SpinLockRelease(&parallel_scan->phs_mutex);

	/*
	 * Report scan location.  Normally, we report the current page number.
	 * When we reach the end of the scan, though, we report the starting page,
	 * not the ending page, just so the starting positions for later scans
	 * doesn't slew backwards.  We only report the position at the end of the
	 * scan once, though: subsequent callers will have report nothing, since
	 * they will have page == InvalidBlockNumber.
	 */
	if (scan->rs_syncscan)
	{
		if (report_page == InvalidBlockNumber)
			report_page = page;
		if (report_page != InvalidBlockNumber)
			ss_report_location(scan->rs_rd, report_page);
	}

	/*
	 * construction of PDS based on the columnar cache, if any
	 */
	if (cc_chunk)
	{
		PG_TRY();
		{
			pds_column = pgstrom_ccache_load_chunk(cc_chunk,
												   gcontext,
												   relation,
												   ccache_refs);
		}
		PG_CATCH();
		{
			pgstrom_ccache_put_chunk(cc_chunk);
			PG_RE_THROW();
		}
		PG_END_TRY();
		pgstrom_ccache_put_chunk(cc_chunk);
	}
	scan->rs_cblock = page;
	scan->rs_numblocks = nr_blocks;

	return pds_column;
}

/*
 * gpuscanExecScanChunk - read the relation by one chunk
 */
pgstrom_data_store *
gpuscanExecScanChunk(GpuTaskState *gts)
{
	Relation		base_rel = gts->css.ss.ss_currentRelation;
	HeapScanDesc	scan;
	pgstrom_data_store *pds = NULL;
	pgstrom_data_store *pds_column = NULL;

	/*
	 * Setup scan-descriptor, if the scan is not parallel, of if we're
	 * executing a scan that was intended to be parallel serially.
	 */
	if (!gts->css.ss.ss_currentScanDesc)
	{
		EState	   *estate = gts->css.ss.ps.state;

		gts->css.ss.ss_currentScanDesc = heap_beginscan(base_rel,
														estate->es_snapshot,
														0, NULL);
		/*
		 * Try to choose NVMe-Strom, if relation is deployed on the supported
		 * tablespace and expected total i/o size is enough large than cache-
		 * only scan.
		 */
		PDS_init_heapscan_state(gts, gts->outer_nrows_per_block);
	}
	scan = gts->css.ss.ss_currentScanDesc;
	InstrStartNode(&gts->outer_instrument);

	/* fetch suspended PDS, if any */
	pds = gts->outer_pds_suspend;
	gts->outer_pds_suspend = NULL;
	for (;;)
	{
		if (!scan->rs_inited)
		{
			if (scan->rs_nblocks == 0)
			{
				Assert(pds == NULL);
				goto out;
			}
			if (!scan->rs_parallel)
			{
				scan->rs_cblock = scan->rs_startblock;
				Assert(scan->rs_numblocks == InvalidBlockNumber);
			}
			else
			{
				/* force to call gpuscan_parallel_nextpage() */
				scan->rs_cblock = InvalidBlockNumber;
				scan->rs_numblocks = 0;
			}
			scan->rs_inited = true;
		}
		else if (scan->rs_cblock == InvalidBlockNumber)
		{
			/* no more blocks to read */
			break;
		}

		/* move to the next position to load */
		if (!scan->rs_parallel)
		{
			BlockNumber		page = scan->rs_cblock;
			struct ccacheChunk *cc_chunk;

			/* try to fetch columnar-cache, if any */
			if (gts->ccache_refs &&
				(page & (CCACHE_CHUNK_NBLOCKS - 1)) == 0 &&
				(page >= scan->rs_startblock ||
				 page + CCACHE_CHUNK_NBLOCKS <= scan->rs_startblock) &&
				(page + CCACHE_CHUNK_NBLOCKS <= scan->rs_nblocks))
			{
				cc_chunk = pgstrom_ccache_get_chunk(scan->rs_rd, page);
				if (cc_chunk)
				{
					PG_TRY();
					{
						pds_column =
							pgstrom_ccache_load_chunk(cc_chunk,
													  gts->gcontext,
													  scan->rs_rd,
													  gts->ccache_refs);
					}
					PG_CATCH();
					{
						pgstrom_ccache_put_chunk(cc_chunk);
						PG_RE_THROW();
					}
					PG_END_TRY();
					pgstrom_ccache_put_chunk(cc_chunk);

					scan->rs_cblock += CCACHE_CHUNK_NBLOCKS;
					if (scan->rs_cblock >= scan->rs_nblocks)
						scan->rs_cblock = 0;
					Assert(scan->rs_numblocks == InvalidBlockNumber);
					if (scan->rs_syncscan)
						ss_report_location(scan->rs_rd, scan->rs_cblock);
					if (scan->rs_cblock == scan->rs_startblock)
						scan->rs_cblock = InvalidBlockNumber;
					break;
				}
			}
		}
		else if (scan->rs_numblocks == 0)
		{
			NVMEScanState  *nvme_sstate = gts->nvme_sstate;
			cl_uint			nblocks_atonce;

			Assert(scan->rs_parallel != NULL);

			/*
			 * Suspend the heap-scan of row-based PDS, and returns columnar
			 * PDS instead. In case when bgworker tries to fetch multiple
			 * blocks which contains the head block of ccache, "gap" blocks
			 * are loaded to row-based PDS, then resumed when bgworker meets
			 * the range with no ccache.
			 */
			if (pds_column)
				break;

			/*
			 * MEMO: A key of i/o performance is consolidation of continuous
			 * block reads with a small number of system-call invocation.
			 * The default one-by-one block read logic tend to generate i/o
			 * request fragmentation under CPU parallel execution, thus it
			 * leads larger number of read commands submit and performance
			 * slow-down.
			 * So, in case of NVMe-Strom under CPU parallel, we make the
			 * @scan->rs_cblock pointer advanced by multiple blocks at once.
			 * It ensures the block numbers to read are continuous, thus,
			 * i/o stack will be able to load storage blocks with minimum
			 * number of DMA requests.
			 */
			if (!nvme_sstate)
				nblocks_atonce = 8;
			else if (pds)
			{
				if (pds->kds.nitems >= pds->kds.nrooms)
					break;	/* no more rooms in this PDS */
				nblocks_atonce = pds->kds.nrooms - pds->kds.nitems;
			}
			else
				nblocks_atonce = nvme_sstate->nblocks_per_chunk;
			pds_column = gpuscan_parallel_nextpage(scan,
												   gts->gcontext,
												   gts->ccache_refs,
												   nblocks_atonce);
			/* no more blocks to read? */
			if (scan->rs_numblocks == 0)
				break;
		}

		/* allocation of row-based PDS on demand */
		if (!pds)
		{
			if (gts->nvme_sstate)
				pds = PDS_create_block(gts->gcontext,
									   RelationGetDescr(base_rel),
									   gts->nvme_sstate);
			else
				pds = PDS_create_row(gts->gcontext,
									 RelationGetDescr(base_rel),
									 pgstrom_chunk_size());
			pds->kds.table_oid = RelationGetRelid(base_rel);
		}
		/* scan next block */
		if (scan->rs_cblock == InvalidBlockNumber ||
			!PDS_exec_heapscan(gts, pds))
			break;

		/* move to the next block */
		scan->rs_cblock++;
		if (scan->rs_cblock >= scan->rs_nblocks)
			scan->rs_cblock = 0;
		if (scan->rs_numblocks != InvalidBlockNumber)
		{
			Assert(scan->rs_numblocks > 0);
			scan->rs_numblocks--;
		}
		if (scan->rs_syncscan)
			ss_report_location(scan->rs_rd, scan->rs_cblock);
		/* end of the scan? */
		if (scan->rs_cblock == scan->rs_startblock)
			scan->rs_cblock = InvalidBlockNumber;
	}

	if (pds_column)
	{
		gts->outer_pds_suspend = pds;
		pds = pds_column;
	}
	else if (!pds)
	{
		/* end of the scan */
		Assert(!BlockNumberIsValid(scan->rs_cblock));
	}
	else if (pds->kds.nitems == 0)
	{
		Assert(!BlockNumberIsValid(scan->rs_cblock));
		PDS_release(pds);
		pds = NULL;
	}
	else if (pds->kds.format == KDS_FORMAT_BLOCK &&
			 pds->kds.nitems < pds->kds.nrooms &&
			 pds->nblocks_uncached > 0)
	{
		/*
		 * MEMO: Special case handling if KDS_FORMAT_BLOCK was not filled
		 * up entirely. KDS_FORMAT_BLOCK has an array of block-number to
		 * support "ctid" system column, located on next to the KDS-head.
		 * Block-numbers of pre-loaded blocks (hit on shared buffer) are
		 * used from the head, and others (to be read from the file) are
		 * used from the tail. If nitems < nrooms, this array has a hole
		 * on the middle of array.
		 * So, we have to move later half of the array to close the hole
		 * and make a flat array.
		 */
		BlockNumber	   *block_nums
			= (BlockNumber *)KERN_DATA_STORE_BODY(&pds->kds);

		memmove(block_nums + (pds->kds.nitems - pds->nblocks_uncached),
				block_nums + (pds->kds.nrooms - pds->nblocks_uncached),
				sizeof(BlockNumber) * pds->nblocks_uncached);
	}
out:
	InstrStopNode(&gts->outer_instrument,
				  !pds ? 0.0 : (double)pds->kds.nitems);
	return pds;
}

static void
gpuscan_switch_task(GpuTaskState *gts, GpuTask *gtask)
{
	GpuScanTask		   *gscan	__attribute__((unused))
		= (GpuScanTask *) gtask;
}

/*
 * gpuscan_next_task
 */
static GpuTask *
gpuscan_next_task(GpuTaskState *gts)
{
	GpuScanState	   *gss = (GpuScanState *) gts;
	GpuScanRuntimeStat *gs_rtstat = gss->gs_rtstat;
	GpuScanTask		   *gscan;
	pgstrom_data_store *pds;

	pds = gpuscanExecScanChunk(gts);
	if (!pds)
		return NULL;
	if (pds->kds.format == KDS_FORMAT_COLUMN)
		pg_atomic_add_fetch_u64(&gs_rtstat->ccache_count, 1);
	gscan = gpuscan_create_task(gss, pds);

	return &gscan->task;
}

/*
 * gpuscan_next_tuple_fallback - GPU fallback case
 */
static TupleTableSlot *
gpuscan_next_tuple_fallback(GpuScanState *gss, GpuScanTask *gscan)
{
	pgstrom_data_store *pds_src = gscan->pds_src;
	GpuScanRuntimeStat *gs_rtstat = gss->gs_rtstat;
	ExprContext		   *econtext = gss->gts.css.ss.ps.ps_ExprContext;
	TupleTableSlot	   *slot = NULL;

retry_next:
	ExecClearTuple(gss->base_slot);
	if (!PDS_fetch_tuple(gss->base_slot, pds_src, &gss->gts))
		return NULL;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = gss->base_slot;

	/*
	 * (1) - Evaluation of dev_quals if any
	 */
	if (gss->dev_quals)
	{
		bool		retval;
#if PG_VERSION_NUM < 100000
		retval = ExecQual(gss->dev_quals, econtext, false);
#else
		retval = ExecQual(gss->dev_quals, econtext);
#endif
		if (!retval)
		{
			pg_atomic_add_fetch_u64(&gs_rtstat->nitems_filtered, 1);
			goto retry_next;
		}
	}

	/*
	 * (2) - Makes a projection if any
	 */
	if (!gss->base_proj)
		slot = gss->base_slot;
	else
	{
#if PG_VERSION_NUM < 100000
		ExprDoneCond		is_done;

		slot = ExecProject(gss->base_proj, &is_done);
		if (is_done == ExprMultipleResult)
			gss->gts.css.ss.ps.ps_TupFromTlist = true;
		else if (is_done != ExprEndResult)
			gss->gts.css.ss.ps.ps_TupFromTlist = false;
#else
		slot = ExecProject(gss->base_proj);
#endif
	}
	return slot;
}

/*
 * gpuscan_next_tuple
 */
static TupleTableSlot *
gpuscan_next_tuple(GpuTaskState *gts)
{
	GpuScanState	   *gss = (GpuScanState *) gts;
	GpuScanTask		   *gscan = (GpuScanTask *) gts->curr_task;
	TupleTableSlot	   *slot = NULL;

	if (gscan->task.cpu_fallback)
		slot = gpuscan_next_tuple_fallback(gss, gscan);
	else if (gscan->pds_dst)
	{
		pgstrom_data_store *pds_dst = gscan->pds_dst;

		slot = gss->gts.css.ss.ss_ScanTupleSlot;
		ExecClearTuple(slot);
		if (!PDS_fetch_tuple(slot, pds_dst, &gss->gts))
			slot = NULL;
	}
	else
	{
		pgstrom_data_store *pds_src = gscan->pds_src;
		kern_resultbuf	   *kresults = gscan->kresults;

		/*
		 * We should not inject GpuScan for all-visible with no device
		 * projection; GPU has no actual works in other words.
		 * NOTE: kresults->results[] keeps offset from the head of
		 * kds_src.
		 */
		Assert(!kresults->all_visible);
		if (gss->gts.curr_index < kresults->nitems)
		{
			HeapTuple	tuple = &gss->scan_tuple;
			cl_uint		kds_offset;

			kds_offset = kresults->results[gss->gts.curr_index++];
			if (pds_src->kds.format == KDS_FORMAT_ROW)
			{
				tuple->t_data = KDS_ROW_REF_HTUP(&pds_src->kds,
												 kds_offset,
												 &tuple->t_self,
												 &tuple->t_len);
			}
			else
			{
				tuple->t_data = KDS_BLOCK_REF_HTUP(&pds_src->kds,
												   kds_offset,
												   &tuple->t_self,
												   &tuple->t_len);
			}
			slot = gss->gts.css.ss.ss_ScanTupleSlot;
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);
		}
	}
	return slot;
}

/*
 * gpuscanRewindScanChunk
 */
void
gpuscanRewindScanChunk(GpuTaskState *gts)
{
	InstrEndLoop(&gts->outer_instrument);
	Assert(gts->css.ss.ss_currentRelation != NULL);
	heap_rescan(gts->css.ss.ss_currentScanDesc, NULL);
	ExecScanReScan(&gts->css.ss);
}

/*
 * gpuscan_process_task
 */
static int
gpuscan_process_task(GpuTask *gtask, CUmodule cuda_module)
{
	GpuContext	   *gcontext = GpuWorkerCurrentContext;
	GpuScanTask	   *gscan = (GpuScanTask *) gtask;
	pgstrom_data_store *pds_src = gscan->pds_src;
	pgstrom_data_store *pds_dst = gscan->pds_dst;
	CUfunction		kern_gpuscan_quals;
	CUdeviceptr		m_gpuscan = (CUdeviceptr)&gscan->kern;
	CUdeviceptr		m_kds_src = 0UL;
	CUdeviceptr		m_kds_dst = (pds_dst ? (CUdeviceptr)&pds_dst->kds : 0UL);
	const char	   *kern_fname;
	void		   *kern_args[5];
	size_t			offset;
	size_t			length;
	size_t			grid_sz;
	size_t			block_sz;
	size_t			nitems_in;
	size_t			nitems_out;
	size_t			extra_size;
	CUresult		rc;
	int				retval = 100001;

	/*
	 * Lookup GPU kernel functions
	 */
	if (pds_src->kds.format == KDS_FORMAT_ROW)
		kern_fname = "gpuscan_exec_quals_row";
	else if (pds_src->kds.format == KDS_FORMAT_BLOCK)
		kern_fname = "gpuscan_exec_quals_block";
	else if (pds_src->kds.format == KDS_FORMAT_COLUMN)
		kern_fname = "gpuscan_exec_quals_column";
	else
		werror("GpuScan: unknown PDS format: %d", pds_src->kds.format);

	rc = cuModuleGetFunction(&kern_gpuscan_quals,
							 cuda_module,
							 kern_fname);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuModuleGetFunction('%s'): %s",
			   kern_fname, errorText(rc));

	/*
	 * Allocation of device memory
	 *
	 * MEMO: NVMe-Strom requires the DMA destination address is mapped to
	 * PCI BAR area, but it is usually a small window thus easy to run out.
	 * So, if we cannot allocate i/o mapped device memory, we try to read
	 * the blocks synchronously then kicks usual RAM->GPU DMA.
	 */
	if (pds_src->kds.format != KDS_FORMAT_BLOCK)
		m_kds_src = (CUdeviceptr)&pds_src->kds;
	else
	{
		if (gscan->with_nvme_strom)
		{
			rc = gpuMemAllocIOMap(gcontext,
								  &m_kds_src,
								  pds_src->kds.length);
			if (rc == CUDA_ERROR_OUT_OF_MEMORY)
			{
				PDS_fillup_blocks(pds_src);
				gscan->with_nvme_strom = false;
			}
			else if (rc != CUDA_SUCCESS)
				werror("failed on gpuMemAllocIOMap: %s", errorText(rc));
		}
		if (m_kds_src == 0UL)
		{
			rc = gpuMemAlloc(gcontext,
							 &m_kds_src,
							 pds_src->kds.length);
			if (rc == CUDA_ERROR_OUT_OF_MEMORY)
				goto out_of_resource;
			else if (rc != CUDA_SUCCESS)
				werror("failed on gpuMemAlloc: %s", errorText(rc));
		}
	}

	/*
	 * OK, enqueue a series of requests
	 */
	length = KERN_GPUSCAN_DMASEND_LENGTH(&gscan->kern);
	rc = cuMemPrefetchAsync((CUdeviceptr)&gscan->kern,
							length,
							CU_DEVICE_PER_THREAD,
							CU_STREAM_PER_THREAD);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuMemPrefetchAsync: %s", errorText(rc));

	/* kern_data_store *kds_src */
	if (pds_src->kds.format != KDS_FORMAT_BLOCK)
	{
		rc = cuMemPrefetchAsync(m_kds_src,
								pds_src->kds.length,
								CU_DEVICE_PER_THREAD,
								CU_STREAM_PER_THREAD);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemPrefetchAsync: %s", errorText(rc));
	}
	else if (!gscan->with_nvme_strom)
	{
		rc = cuMemcpyHtoDAsync(m_kds_src,
							   &pds_src->kds,
							   pds_src->kds.length,
							   CU_STREAM_PER_THREAD);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemcpyHtoDAsync: %s", errorText(rc));
	}
	else
	{
		Assert(pds_src->kds.format == KDS_FORMAT_BLOCK);
		gpuMemCopyFromSSD(m_kds_src, pds_src);
	}

	/* head of the kds_dst, if any */
	if (pds_dst)
	{
		length = KERN_DATA_STORE_HEAD_LENGTH(&pds_dst->kds);
		rc = cuMemPrefetchAsync((CUdeviceptr)&pds_dst->kds,
								length,
								CU_DEVICE_PER_THREAD,
								CU_STREAM_PER_THREAD);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemPrefetchAsync: %s", errorText(rc));
	}

	/*
	 * KERNEL_FUNCTION(void)
	 * gpuscan_exec_quals_XXXX(kern_gpuscan *kgpuscan,
	 *                         kern_data_store *kds_src,
	 *                         kern_data_store *kds_dst)
	 */
	gpuOptimalBlockSize(&grid_sz,
						&block_sz,
						kern_gpuscan_quals,
						0,		/* max activation */
						0,
						sizeof(cl_int));
	kern_args[0] = &m_gpuscan;
	kern_args[1] = &m_kds_src;
	kern_args[2] = &m_kds_dst;

	rc = cuLaunchKernel(kern_gpuscan_quals,
						grid_sz, 1, 1,
						block_sz, 1, 1,
						sizeof(cl_int) * 1024,
						CU_STREAM_PER_THREAD,
						kern_args,
						NULL);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuLaunchKernel: %s", errorText(rc));

	rc = cuEventRecord(CU_EVENT0_PER_THREAD, CU_STREAM_PER_THREAD);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuEventRecord: %s", errorText(rc));

	/* Point of synchronization */
	rc = cuEventSynchronize(CU_EVENT0_PER_THREAD);
	if (rc != CUDA_SUCCESS)
		werror("failed on cuEventSynchronize: %s", errorText(rc));

	/*
	 * Check GPU kernel status and nitems/usage
	 */
	retval = 0;
	nitems_in  = gscan->kern.nitems_in;
	nitems_out = gscan->kern.nitems_out;
	extra_size = gscan->kern.extra_size;

	gscan->task.kerror = ((kern_gpuscan *)m_gpuscan)->kerror;
	if (gscan->task.kerror.errcode == StromError_Success)
	{
		GpuScanState	   *gss = (GpuScanState *)gscan->task.gts;
		GpuScanRuntimeStat *gs_rtstat = gss->gs_rtstat;

		pg_atomic_add_fetch_u64(&gs_rtstat->nitems_filtered,
								nitems_in - nitems_out);
	}
	else
	{
		if (pgstrom_cpu_fallback_enabled &&
			(gscan->task.kerror.errcode == StromError_CpuReCheck ||
			 gscan->kern.kerror.errcode == StromError_DataStoreNoSpace))
		{
			memset(&gscan->task.kerror, 0, sizeof(kern_errorbuf));
			gscan->task.cpu_fallback = true;

			/*
			 * In case of NVMe-Strom, we have to write-back blocks that are
			 * not loaded onto CPU RAM yet, for fallback processing.
			 */
			if (gscan->with_nvme_strom &&
				pds_dst->nblocks_uncached > 0)
			{
				void  *p_dest = KERN_DATA_STORE_BLOCK_PGPAGE(&pds_src->kds, 0);

				offset = (uintptr_t)p_dest - (uintptr_t)&pds_src->kds;
				rc = cuMemcpyDtoHAsync(p_dest,
									   m_kds_src + offset,
									   pds_src->nblocks_uncached * BLCKSZ,
									   CU_STREAM_PER_THREAD);
				if (rc != CUDA_SUCCESS)
					werror("failed on cuMemcpyDtoHAsync: %s", errorText(rc));

				rc = cuEventRecord(CU_EVENT0_PER_THREAD,
								   CU_STREAM_PER_THREAD);
				if (rc != CUDA_SUCCESS)
					werror("failed on cuEventRecord: %s", errorText(rc));

				/* Point of synchronization */
				rc = cuEventSynchronize(CU_EVENT0_PER_THREAD);
				if (rc != CUDA_SUCCESS)
					werror("failed on cuEventSynchronize: %s", errorText(rc));
			}
		}
		goto out_of_resource;
	}

	if (pds_dst)
	{
		if (nitems_out > 0)
		{
			Assert(extra_size > 0);
			offset = pds_dst->kds.length - extra_size;
			rc = cuMemPrefetchAsync((CUdeviceptr)(&pds_dst->kds) + offset,
									extra_size,
									CU_DEVICE_CPU,
									CU_STREAM_PER_THREAD);
			if (rc != CUDA_SUCCESS)
				werror("failed on cuMemPrefetchAsync: %s", errorText(rc));

			length = KERN_DATA_STORE_HEAD_LENGTH(&pds_dst->kds);
			rc = cuMemPrefetchAsync((CUdeviceptr)(&pds_dst->kds),
									length + sizeof(cl_uint) * nitems_out,
									CU_DEVICE_CPU,
									CU_STREAM_PER_THREAD);
			if (rc != CUDA_SUCCESS)
				werror("failed on cuMemPrefetchAsync: %s", errorText(rc));
		}
	}
	else
	{
		Assert(extra_size == 0);

		rc = cuMemPrefetchAsync((CUdeviceptr)gscan->kresults,
								offsetof(kern_resultbuf,
										 results[nitems_out]),
								CU_DEVICE_CPU,
								CU_STREAM_PER_THREAD);
		if (rc != CUDA_SUCCESS)
			werror("failed on cuMemPrefetchAsync: %s", errorText(rc));
	}

out_of_resource:
	if (retval > 0)
		wnotice("GpuScan: out of resource");
	if (pds_src->kds.format == KDS_FORMAT_BLOCK)
		gpuMemFree(gcontext, m_kds_src);
	return retval;
}

/*
 * gpuscan_release_task
 */
static void
gpuscan_release_task(GpuTask *gtask)
{
	GpuScanTask	   *gscan = (GpuScanTask *) gtask;
	GpuTaskState   *gts = gscan->task.gts;

	if (gscan->pds_src)
		PDS_release(gscan->pds_src);
	if (gscan->pds_dst)
		PDS_release(gscan->pds_dst);
	gpuMemFree(gts->gcontext, (CUdeviceptr) gscan);
}

/*
 * pgstrom_init_gpuscan
 */
void
pgstrom_init_gpuscan(void)
{
	/* pg_strom.enable_gpuscan */
	DefineCustomBoolVariable("pg_strom.enable_gpuscan",
							 "Enables the use of GPU accelerated full-scan",
							 NULL,
							 &enable_gpuscan,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	/* pg_strom.pullup_outer_scan */
	DefineCustomBoolVariable("pg_strom.pullup_outer_scan",
							 "Enables to pull up simple outer scan",
							 NULL,
							 &enable_pullup_outer_scan,
							 true,
							 PGC_USERSET,
                             GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);

	/* setup path methods */
	memset(&gpuscan_path_methods, 0, sizeof(gpuscan_path_methods));
	gpuscan_path_methods.CustomName			= "GpuScan";
	gpuscan_path_methods.PlanCustomPath		= PlanGpuScanPath;

	/* setup plan methods */
	memset(&gpuscan_plan_methods, 0, sizeof(gpuscan_plan_methods));
	gpuscan_plan_methods.CustomName			= "GpuScan";
	gpuscan_plan_methods.CreateCustomScanState = gpuscan_create_scan_state;
	RegisterCustomScanMethods(&gpuscan_plan_methods);

	/* setup exec methods */
	memset(&gpuscan_exec_methods, 0, sizeof(gpuscan_exec_methods));
	gpuscan_exec_methods.CustomName         = "GpuScan";
	gpuscan_exec_methods.BeginCustomScan    = ExecInitGpuScan;
	gpuscan_exec_methods.ExecCustomScan     = ExecGpuScan;
	gpuscan_exec_methods.EndCustomScan      = ExecEndGpuScan;
	gpuscan_exec_methods.ReScanCustomScan   = ExecReScanGpuScan;
	gpuscan_exec_methods.EstimateDSMCustomScan = ExecGpuScanEstimateDSM;
	gpuscan_exec_methods.InitializeDSMCustomScan = ExecGpuScanInitDSM;
	gpuscan_exec_methods.InitializeWorkerCustomScan = ExecGpuScanInitWorker;
#if PG_VERSION_NUM >= 100000
	gpuscan_exec_methods.ShutdownCustomScan	= ExecShutdownGpuScan;
#endif
	gpuscan_exec_methods.ExplainCustomScan  = ExplainGpuScan;

	/* hook registration */
	set_rel_pathlist_next = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gpuscan_add_scan_path;
}
