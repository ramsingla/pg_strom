/*
 * strom_types.h
 *
 * Type definitions of PG-Strom
 *
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#ifndef STROM_TYPES_H
#define STROM_TYPES_H

/*
 * pgstrom_platform_info
 *
 * Properties of OpenCL platform being choosen. Usually, a particular
 * platform shall be choosen on starting up time according to the GUC
 * configuration (including automatic policy).
 * Note that the properties below are supported on the OpenCL 1.1 only,
 * because older drivers cannot understand newer parameter names appeared
 * in v1.2.
 */
typedef struct {
	cl_uint		pl_index;
	char	   *pl_profile;
	char	   *pl_version;
	char	   *pl_name;
	char	   *pl_vendor;
	char	   *pl_extensions;
	Size		buflen;
	char		buffer[FLEXIBLE_ARRAY_MEMBER];
} pgstrom_platform_info;

/*
 * pgstrom_device_info
 *
 * A set of OpenCL properties of a particular device. See above comments.
 */
typedef struct {
	pgstrom_platform_info *pl_info;
	cl_uint		dev_index;
	cl_uint		dev_address_bits;
	cl_bool		dev_available;
	cl_bool		dev_compiler_available;
	cl_device_fp_config	dev_double_fp_config;
	cl_bool		dev_endian_little;
	cl_bool		dev_error_correction_support;
	cl_device_exec_capabilities dev_execution_capabilities;
	char	   *dev_device_extensions;
	cl_ulong	dev_global_mem_cache_size;
	cl_device_mem_cache_type	dev_global_mem_cache_type;
	cl_uint		dev_global_mem_cacheline_size;
	cl_ulong	dev_global_mem_size;
	cl_bool		dev_host_unified_memory;
	cl_ulong	dev_local_mem_size;
	cl_device_local_mem_type	dev_local_mem_type;
	cl_uint		dev_max_clock_frequency;
	cl_uint		dev_max_compute_units;
	cl_uint		dev_max_constant_args;
	cl_ulong	dev_max_constant_buffer_size;
	cl_ulong	dev_max_mem_alloc_size;
	size_t		dev_max_parameter_size;
	cl_uint		dev_max_samplers;
	size_t		dev_max_work_group_size;
	cl_uint		dev_max_work_item_dimensions;
	size_t		dev_max_work_item_sizes[3];
	cl_uint		dev_mem_base_addr_align;
	char	   *dev_name;
	cl_uint		dev_native_vector_width_char;
	cl_uint		dev_native_vector_width_short;
	cl_uint		dev_native_vector_width_int;
	cl_uint		dev_native_vector_width_long;
	cl_uint		dev_native_vector_width_float;
	cl_uint		dev_native_vector_width_double;
	char	   *dev_opencl_c_version;
	cl_uint		dev_preferred_vector_width_char;
	cl_uint		dev_preferred_vector_width_short;
	cl_uint		dev_preferred_vector_width_int;
	cl_uint		dev_preferred_vector_width_long;
	cl_uint		dev_preferred_vector_width_float;
	cl_uint		dev_preferred_vector_width_double;
	char	   *dev_profile;
	size_t		dev_profiling_timer_resolution;
	cl_command_queue_properties	dev_queue_properties;
	cl_device_fp_config	dev_single_fp_config;
	cl_device_type	dev_type;
	char	   *dev_vendor;
	cl_uint		dev_vendor_id;
	char	   *dev_version;
	char	   *driver_version;
	Size		buflen;
	char		buffer[FLEXIBLE_ARRAY_MEMBER];
} pgstrom_device_info;

/*
 * pgstrom_queue
 *
 * A message queue allocated on shared memory, to send messages to/from
 * OpenCL background server. A message queue is constructed with refcnt=1,
 * then its reference counter shall be incremented for each message enqueue
 * to be returned
 */
typedef struct {
	pthread_mutex_t	lock;
	pthread_cond_t	cond;
	dlist_head		qhead;
	int				refcnt;
	bool			closed;
} pgstrom_queue;




typedef struct pgstrom_message {
	MessageTag		mtag;
	dlist_node		chain;
	pgstrom_queue  *respq;	/* queue for response message */
	/* destructor of this message if needed */
	void			(*cb_release)(struct pgstrom_message *message);
} pgstrom_message;

/* maximum number of items to be placed on a row/column store */
#define NITEMS_PER_CHUNK	(1 << 18)

typedef struct {
	MessageTag		mtag;	/* StromMsg_ParamBuf */
	cl_uint			refcnt;
	cl_uint			nparams;
	cl_uint			params[FLEXIBLE_ARRAY_MEMBER];	/* offset of params */
} pgstrom_parambuf;

typedef struct {
	MessageTag		mtag;	/* StromMsg_RowStore */
	dlist_node		chain;	/* to be chained to subject node */
	cl_uint			usage;	/* usage; tuple body */
	cl_uint			nrows;	/* number of records in this store */
	cl_uint			usage;	/* usage; tuple body is put from the tail */
	cl_uint			tuples[FLEXIBLE_ARRAY_MEMBER];	/* offset of tuples */
} pgstrom_row_store;

typedef struct {
	MessageTag		mtag;	/* StromMsg_ColumnStore */
	cl_uint			length;	/* total length of this column-store */
	dlist_node		chain;	/* to be chained to subject node */
	cl_uint			nrows;	/* number of records in this store */
	cl_uint			length;	/* length of this column-store */
} pgstrom_column_store;

typedef kern_toastbuf	pgstrom_toastbuf;

typedef struct {
	MessageTag		type;	/* StromMsg_GpuScan */
	Datum			program_id;
	union {
		MessageTag			   *head;
		pgstrom_row_store	   *rs;
		pgstrom_column_store   *cs;
	} store;
	cl_int			errcode;
	cl_int			nrows;
	cl_int			results[FLEXIBLE_ARRAY_MEMBER];
} pgstrom_gpuscan;


/*
 * Type declarations for code generator
 */
#define DEVINFO_IS_NEGATIVE			0x0001
#define DEVTYPE_IS_VARLENA			0x0002
#define DEVTYPE_IS_BUILTIN			0x0004
#define DEVFUNC_NEEDS_TIMELIB		0x0008
#define DEVFUNC_NEEDS_TEXTLIB		0x0010
#define DEVFUNC_NEEDS_NUMERICLIB	0x0020
#define DEVFUNC_INCL_FLAGS			\
	(DEVFUNC_NEEDS_TIMELIB | DEVFUNC_NEEDS_TEXTLIB | DEVFUNC_NEEDS_NUMERICLIB)

struct devtype_info;
struct devfunc_info;

typedef struct devtype_info {
	Oid			type_oid;
	uint32		type_flags;
	char	   *type_ident;
	char	   *type_base;
	char	   *type_decl;
	struct devfunc_info *type_is_null_fn;
	struct devfunc_info	*type_is_not_null_fn;
} devtype_info;

typedef struct devfunc_info {
	const char *func_name;
	Oid			func_namespace;
	Oid		   *func_argtypes;
	int32		func_flags;
	const char *func_ident;	/* identifier of device function */
	List	   *func_args;	/* list of devtype_info */
	devtype_info *func_rettype;
	const char *func_decl;	/* declaration of function */
} devfunc_info;




#endif	/* STROM_TYPES_H */