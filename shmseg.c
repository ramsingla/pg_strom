/*
 * shmem.c
 *
 * Routines to manage shared memory segment & queues
 *
 * --
 * Copyright 2013 (c) PG-Strom Development Team
 * Copyright 2012-2013 (c) KaiGai Kohei <kaigai@kaigai.gr.jp>
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "pg_strom.h"
#include <limits.h>
#include <unistd.h>

#define SHMEM_BLOCK_FREE			0xF9EEA9EA
#define SHMEM_BLOCK_USED			0xA110CED0
#define SHMEM_BLOCK_USED_MASK		0xfffffff0
#define SHMEM_BLOCK_OVERRUN_MARK	0xDEADBEAF
#define SHMEM_BLOCK_STROM_QUEUE		(SHMEM_BLOCK_USED | 0x01)
#define SHMEM_BLOCK_KERNEL_PARAMS	(SHMEM_BLOCK_USED | 0x02)
#define SHMEM_BLOCK_CHUNK_BUFFER	(SHMEM_BLOCK_USED | 0x03)
#define SHMEM_BLOCK_VARLENA_BUFFER	(SHMEM_BLOCK_USED | 0x04)
#define SHMEM_BLOCK_DEVICE_PROPERTY	(SHMEM_BLOCK_USED | 0x05)

#define SHMEM_BLOCK_OVERRUN_MARKER(block)	\
	(*((uint32 *)(((char *)(block)) + (block)->size - sizeof(uint32))))

typedef struct {
	uint32			magic;		/* one of SHMEM_BLOCK_* */
	Size			size;		/* size of this block includes metadata */
	dlist_node		addr_list;	/* list in order of address */
	dlist_node		free_list;	/* list of free blocks, if free block.
								 * Also note that this field is used to
								 * chain this block on the private hash
								 * slot to track blocks being allocated
								 * on a particular processes.
								 */
	pid_t			pid;		/* pid of process that uses this block */
	ResourceOwner	owner;		/* transaction owner of this block */
	Datum			data[0];
} ShmemBlock;

typedef struct {
	Size			total_size;	/* size of total shmem segment */
	Size			free_size;	/* size of total free area */
	dlist_head		addr_head;	/* list head of all nodes in address oder */
	dlist_head		free_head;	/* list head of free blocks  */
	pthread_mutex_t	lock;

	/* for device properties */
	dlist_head			dev_head;	/* head of device properties */
	pthread_rwlock_t	dev_lock;	/* lock of device properties */

	ShmemBlock		first_block[0];
} ShmemHead;

static int						pgstrom_shmem_size;
static pthread_mutexattr_t		shmem_mutex_attr;
static pthread_rwlockattr_t		shmem_rwlock_attr;
static pthread_condattr_t		shmem_cond_attr;
static dlist_head				shmem_private_track;
static shmem_startup_hook_type	shmem_startup_hook_next = NULL;
static ShmemHead			   *pgstrom_shmem_head;

/*
 * Utility routines of synchronization objects
 */
bool
pgstrom_mutex_init(pthread_mutex_t *mutex)
{
	int		rc;

	if ((rc = pthread_mutex_init(mutex, &shmem_mutex_attr)) != 0)
	{
		elog(NOTICE, "failed to initialize mutex at %p (%s)",
			 mutex, strerror(rc));
		return false;
	}
	return true;
}

bool
pgstrom_rwlock_init(pthread_rwlock_t *rwlock)
{
	int		rc;

	if ((rc = pthread_rwlock_init(rwlock, &shmem_rwlock_attr)) != 0)
	{
		elog(NOTICE, "failed to initialize rwlock at %p (%s)",
			 rwlock, strerror(rc));
		return false;
	}
	return true;
}

bool
pgstrom_cond_init(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	int		rc;

	if ((rc = pthread_mutex_init(mutex, &shmem_mutex_attr)) != 0)
    {
        elog(NOTICE, "failed to initialize mutex at %p (%s)",
             mutex, strerror(rc));
        return false;
    }

	if ((rc = pthread_cond_init(cond, &shmem_cond_attr)) != 0)
	{
		elog(NOTICE, "failed to initialize conditional variable at %p (%s)",
			 cond, strerror(rc));
		pthread_mutex_destroy(mutex);
		return false;
	}
	return true;
}

/*
 * pgstrom_cond_wait - wait for wake-up of conditional variable
 *
 * XXX - Note that this function may wake-up by signal. Even if it backs
 * control the caller, don't forget to check whether the condition is
 * really satisfied, or not. So, typical coding style shall be as follows.
 *
 * pthread_mutex_lock(&lock);
 * do {
 *     if (!pgstrom_cond_wait(&cond, &lock, 1000))
 *         break;     // timeout
 *     if (!queue_has_item)
 *         continue;  // signal interruption
 *     // do the works to be synchronized
 *     break;
 * } while(true);
 * pthread_mutex_unlock(&lock)
 */
bool
pgstrom_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex,
				  unsigned int timeout)
{
	int		rc;

	if (timeout > 0)
	{
		struct timespec	abstime;
		struct timeval tv;

		gettimeofday(&tv, NULL);
		abstime.tv_sec = tv.tv_sec + timeout / 1000;
		abstime.tv_nsec = (tv.tv_usec + (timeout % 1000) * 1000) * 1000;

		rc = pthread_cond_timedwait(cond, mutex, &abstime);
	}
	else
		rc = pthread_cond_wait(cond, mutex);

	Assert(rc == 0 || rc == ETIMEDOUT);

	return (rc == 0 ? true : false);
}

/*
 * Routines to allocate / free shared memory region
 */
static void
pgstrom_shmem_free(ShmemBlock *block)
{
	ShmemBlock	   *prev;
	ShmemBlock	   *next;
	dlist_node	   *temp;

	Assert((block->magic & SHMEM_BLOCK_USED_MASK) == SHMEM_BLOCK_USED);
	Assert(SHMEM_BLOCK_OVERRUN_MARKER(block) == SHMEM_BLOCK_OVERRUN_MARK);

	pthread_mutex_lock(&pgstrom_shmem_head->lock);
	pgstrom_shmem_head->free_size += block->size;

	/* merge, if previous block is also free */
	if (dlist_has_prev(&pgstrom_shmem_head->addr_head, &block->addr_list))
	{
		temp = dlist_prev_node(&pgstrom_shmem_head->addr_head,
							   &block->addr_list);
		prev = dlist_container(ShmemBlock, addr_list, temp);

		if (prev->magic == SHMEM_BLOCK_FREE)
		{
			dlist_delete(&block->addr_list);
			dlist_delete(&prev->free_list);
			prev->size += block->size;
			block = prev;
		}
	}

	/* merge, if next block is also free */
	if (dlist_has_next(&pgstrom_shmem_head->addr_head, &block->addr_list))
	{
		temp = dlist_next_node(&pgstrom_shmem_head->addr_head,
							   &block->addr_list);
		next = dlist_container(ShmemBlock, addr_list, temp);

		if (next->magic == SHMEM_BLOCK_FREE)
		{
			dlist_delete(&next->addr_list);
			dlist_delete(&next->free_list);
			block->size += next->size;
		}
	}
	block->magic = SHMEM_BLOCK_FREE;
	dlist_push_head(&pgstrom_shmem_head->free_head, &block->free_list);

	pthread_mutex_unlock(&pgstrom_shmem_head->lock);
}

static ShmemBlock *
pgstrom_shmem_alloc(uint32 magic, Size size)
{
	ShmemBlock *block = NULL;
	dlist_iter	iter;
	Size		required;

	required = MAXALIGN(offsetof(ShmemBlock, data) +
						MAXALIGN(size) + sizeof(uint32));

	pthread_mutex_lock(&pgstrom_shmem_head->lock);
	dlist_foreach(iter, &pgstrom_shmem_head->free_head)
	{
		block = dlist_container(ShmemBlock, free_list, iter.cur);

		Assert(block->magic == SHMEM_BLOCK_FREE);

		/*
		 * Size of the current free block is not enough to assign shared
		 * memory block with required size, so we try next free block.
		 */
		if (block->size < required)
			continue;

		/*
		 * In case when size of the current free block is similar to the 
		 * required size, we replace whole the block for the requirement
		 * to avoid management overhead on such a small fraction.
		 */
		if (block->size < required + 4096)
		{
			dlist_delete(&block->free_list);
			block->magic = magic;
			pgstrom_shmem_head->free_size -= block->size;
		}
		else
		{
			ShmemBlock *block_new;

			dlist_delete(&block->free_list);

			block_new = (ShmemBlock *) (((char *) block) + required);
			block_new->magic = SHMEM_BLOCK_FREE;
			dlist_insert_after(&block->addr_list, &block_new->addr_list);
			dlist_push_head(&pgstrom_shmem_head->free_head,
							&block_new->free_list);
			block_new->size = block->size - required;

			Assert((magic & SHMEM_BLOCK_USED_MASK) == SHMEM_BLOCK_USED);
			block->magic = magic;
			block->size = required;
		}
		break;
	}
	pthread_mutex_unlock(&pgstrom_shmem_head->lock);

	if (block)
	{
		block->pid = getpid();
		block->owner = CurrentResourceOwner;
		SHMEM_BLOCK_OVERRUN_MARKER(block) = SHMEM_BLOCK_OVERRUN_MARK;
	}
	return block;
}

void
pgstrom_shmem_dump(void)
{
	dlist_iter	iter;

	pthread_mutex_lock(&pgstrom_shmem_head->lock);
	elog(INFO, "0x%p - 0x%p size: %lu, used-size: %lu, free-size: %lu",
		 pgstrom_shmem_head,
		 ((char *)pgstrom_shmem_head)
		 + offsetof(ShmemHead, first_block)
		 + pgstrom_shmem_head->total_size,
		 pgstrom_shmem_head->total_size,
		 pgstrom_shmem_head->total_size - pgstrom_shmem_head->free_size,
		 pgstrom_shmem_head->free_size);

	dlist_foreach(iter, &pgstrom_shmem_head->addr_head)
	{
		ShmemBlock *block = dlist_container(ShmemBlock, addr_list, iter.cur);

		if (block->magic == SHMEM_BLOCK_FREE)
		{
			elog(INFO, "0x%p - 0x%p size: %lu, type: free",
				 block, ((char *)block) + block->size, block->size);
		}
		else
		{
			const char *block_type;
			uint32		block_overrun;

			switch (block->magic)
			{
				case SHMEM_BLOCK_STROM_QUEUE:
					block_type = "strom-queue";
					break;
				case SHMEM_BLOCK_KERNEL_PARAMS:
					block_type = "kernel_params";
					break;
				case SHMEM_BLOCK_CHUNK_BUFFER:
					block_type = "chunk-buffer";
					break;
				case SHMEM_BLOCK_VARLENA_BUFFER:
					block_type = "varlena-buffer";
					break;
				case SHMEM_BLOCK_DEVICE_PROPERTY:
					block_type = "device-property";
					break;
				default:
					block_type = "unknown";
					break;
			}
			block_overrun = SHMEM_BLOCK_OVERRUN_MARKER(block);
			elog(INFO, "0x%p - 0x%p size: %lu, "
				 "used by pid: %u, type: %s, overrun: %s",
				 block, ((char *)block) + block->size, block->size,
				 block->pid, block_type,
				 block_overrun != SHMEM_BLOCK_OVERRUN_MARK ? "yes" : "no");
		}
	}
	pthread_mutex_unlock(&pgstrom_shmem_head->lock);
}

void
pgstrom_shmem_range(uintptr_t *start, uintptr_t *end)
{
	*start = (uintptr_t) pgstrom_shmem_head;
	*end   = (uintptr_t) pgstrom_shmem_head
		+ offsetof(ShmemHead, first_block)
		+ pgstrom_shmem_head->total_size;
}

/*
 * pgstrom_shmem_cleanup
 *
 * callback routine to cleanup shared memory block being acquired 
 */
static void
pgstrom_shmem_cleanup(ResourceReleasePhase phase,
					  bool is_commit,
					  bool is_toplevel,
					  void *arg)
{
	dlist_mutable_iter	iter;
	ShmemBlock	   *block;

	if (phase != RESOURCE_RELEASE_AFTER_LOCKS)
		return;

	/*
	 * Step.1 - Release chunk-buffers and relevant varlena-buffers,
	 * and wait for completion of its execution if it is running.
	 */
	dlist_foreach_modify(iter, &shmem_private_track)
	{
		block = dlist_container(ShmemBlock, free_list, iter.cur);

		/*
		 * All blocks should be already released on regular code path
		 * when transaction is normally committed.
		 */
		Assert(!is_commit);

		/* No free blocks should appeared */
		Assert((block->magic & SHMEM_BLOCK_USED_MASK) == SHMEM_BLOCK_USED);

		/*
		 * Only blocks relevant to CurrentResourceOwner shall be released.
		 */
		if (block->owner != CurrentResourceOwner)
			continue;

		if (block->magic == SHMEM_BLOCK_CHUNK_BUFFER)
		{
			ChunkBuffer *chunk = (ChunkBuffer *)block->data;

			/*
			 * Wait for completion of kernel-execution on this chunk
			 */
		retry:
			pgstrom_cond_wait(&chunk->cond, &chunk->lock, 30*1000);
			if (chunk->is_running)
			{
				pthread_mutex_unlock(&chunk->lock);
				elog(LOG, "waiting for completion of kernel execution...");
				goto retry;
			}
			pthread_mutex_unlock(&chunk->lock);

			/*
			 * Note: relevant varlena-buffers are also released
			 * at pgstrom_chunk_buffer_free()
			 */
			pgstrom_chunk_buffer_free(chunk);
		}
	}

	/*
	 * Step.2 - Release Kernel-Params buffers
	 */
	dlist_foreach_modify(iter, &shmem_private_track)
	{
		block = dlist_container(ShmemBlock, free_list, iter.cur);

		/* See above */
		if (block->owner != CurrentResourceOwner)
			continue;

		if (block->magic == SHMEM_BLOCK_KERNEL_PARAMS)
			pgstrom_kernel_params_free((KernelParams *)block->data);
	}

	/*
	 * Step.3 - Release queues that should not have any valid items yet
	 */
	dlist_foreach_modify(iter, &shmem_private_track)
	{
		block = dlist_container(ShmemBlock, free_list, iter.cur);

		/* See above */
		if (block->owner != CurrentResourceOwner)
			continue;

		Assert(block->magic == SHMEM_BLOCK_KERNEL_PARAMS);
		pgstrom_queue_free((StromQueue *)block->data);
	}
}

/*
 * Routines for PG-Strom Queue
 */
StromQueue *
pgstrom_queue_alloc(bool abort_on_error)
{
	StromQueue *queue;
	ShmemBlock *block;

	block = pgstrom_shmem_alloc(SHMEM_BLOCK_STROM_QUEUE, sizeof(StromQueue));
	if (!block)
	{
		if (abort_on_error)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of shared memory segment"),
					 errhint("enlarge pg_strom.shmem_size")));
		return NULL;
	}
	queue = (StromQueue *)block->data;
	dlist_init(&queue->head);
	if (!pgstrom_cond_init(&queue->cond, &queue->lock))
	{
		pgstrom_shmem_free(block);
		if (abort_on_error)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("failed to init mutex object")));
		return NULL;
	}
	queue->is_shutdown = false;

	/* add this block into private tracker */
	dlist_push_tail(&shmem_private_track, &block->free_list);

	return queue;
}

void
pgstrom_queue_free(StromQueue *queue)
{
	ShmemBlock *block = container_of(ShmemBlock, data, queue);

	/* untrack this block in private tracker */
	dlist_delete(&block->free_list);
	Assert(block->magic == SHMEM_BLOCK_STROM_QUEUE);

	/* release it */
	pthread_mutex_destroy(&queue->lock);
	pthread_cond_destroy(&queue->cond);
	pgstrom_shmem_free(block);
}

bool
pgstrom_queue_enqueue(StromQueue *queue, dlist_node *chain)
{
	bool	result = true;

	pthread_mutex_lock(&queue->lock);
	if (!queue->is_shutdown)
		dlist_push_tail(&queue->head, chain);
	else
		result = false;
	pthread_cond_signal(&queue->cond);
	pthread_mutex_unlock(&queue->lock);

	return result;
}

dlist_node *
pgstrom_queue_dequeue(StromQueue *queue, unsigned int timeout)
{
	dlist_node *result = NULL;

	pthread_mutex_lock(&queue->lock);
	if (!dlist_is_empty(&queue->head))
		result = dlist_pop_head_node(&queue->head);
	else
	{
		/*
		 * XXX - Note that signal can interrupt pthread_cond_wait, thus
		 * queue->head may be still empty even if pgstrom_cond_wait
		 * returns true.
		 */
		if (pgstrom_cond_wait(&queue->cond, &queue->lock, timeout) &&
			!dlist_is_empty(&queue->head))
			result = dlist_pop_head_node(&queue->head);
	}
	pthread_mutex_unlock(&queue->lock);

	return result;
}

dlist_node *
pgstrom_queue_try_dequeue(StromQueue *queue)
{
	dlist_node *result = NULL;

	pthread_mutex_lock(&queue->lock);
	if (!dlist_is_empty(&queue->head))
		result = dlist_pop_head_node(&queue->head);
	pthread_mutex_unlock(&queue->lock);

	return result;
}

bool
pgstrom_queue_is_empty(StromQueue *queue)
{
	bool	result;

	pthread_mutex_lock(&queue->lock);
	result = dlist_is_empty(&queue->head);
	pthread_mutex_unlock(&queue->lock);

	return result;
}

void
pgstrom_queue_shutdown(StromQueue *queue)
{
	pthread_mutex_lock(&queue->lock);
	queue->is_shutdown = true;
	pthread_mutex_unlock(&queue->lock);
}

/*
 * Interface for KernelParams
 */
KernelParams *
pgstrom_kernel_params_alloc(Size total_length, bool abort_on_error)
{
	ShmemBlock *block;

	block = pgstrom_shmem_alloc(SHMEM_BLOCK_KERNEL_PARAMS, total_length);
	if (!block)
	{
		if (abort_on_error)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of shared memory segment"),
					 errhint("enlarge pg_strom.shmem_size")));
		return NULL;
	}
	/* add this block into private tracker */
	dlist_push_tail(&shmem_private_track, &block->free_list);

	return (KernelParams *)block->data;
}

void
pgstrom_kernel_params_free(KernelParams *kernel_params)
{
	ShmemBlock *block = container_of(ShmemBlock, data, kernel_params);

	Assert(block->magic == SHMEM_BLOCK_KERNEL_PARAMS);
	/* untrack this block in private tracker */
	dlist_delete(&block->free_list);

	pgstrom_shmem_free(block);
}

/*
 * Interface for VarlenaBuffer
 */
VarlenaBuffer *
pgstrom_varlena_buffer_alloc(Size total_length, bool abort_on_error)
{
	ShmemBlock	   *block;
	VarlenaBuffer  *vlbuf;

	block = pgstrom_shmem_alloc(SHMEM_BLOCK_VARLENA_BUFFER, total_length);
	if (!block)
	{
		if (abort_on_error)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of shared memory segment"),
					 errhint("enlarge pg_strom.shmem_size")));
		return NULL;
	}

	/*
	 * Note: varlena buffer shall be always associated with a particular
	 * chunk-buffer, then released same timing with its master. So, we
	 * don't track individual varlena-buffers on private-tracker.
	 * Its design reason is, varlena-buffers can be allocated by parallel-
	 * loader that is a different process from the process that acquires
	 * the chunk-buffer to be associated with.
	 */
	vlbuf = (VarlenaBuffer *)block->data;
	memset(vlbuf, 0, offsetof(VarlenaBuffer, data));
	vlbuf->length = total_length - offsetof(VarlenaBuffer, data);
	vlbuf->usage = 0;

	return vlbuf;
}

void
pgstrom_varlena_buffer_free(VarlenaBuffer *vlbuf)
{
	ShmemBlock *block = container_of(ShmemBlock, data, vlbuf);

	Assert(block->magic == SHMEM_BLOCK_VARLENA_BUFFER);
	/* Note: Also, no need to untrack varlena buffers */

	pgstrom_shmem_free(block);
}

/*
 * Routines for ChunkBuffer
 */
ChunkBuffer *
pgstrom_chunk_buffer_alloc(Size total_length, bool abort_on_error)
{
	ChunkBuffer	   *chunk;
	ShmemBlock	   *block;

	/* ensure total_length is larger than ChunkBuffer */
	if (total_length < sizeof(ChunkBuffer))
		total_length = sizeof(ChunkBuffer);

	block = pgstrom_shmem_alloc(SHMEM_BLOCK_CHUNK_BUFFER, total_length);
	if (!block)
	{
		if (abort_on_error)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
					 errmsg("out of shared memory segment"),
					 errhint("enlarge pg_strom.shmem_size")));
		return NULL;
	}

	chunk = (ChunkBuffer *)block->data;
	if (!pgstrom_cond_init(&chunk->cond, &chunk->lock))
	{
		pgstrom_shmem_free(block);
		if (abort_on_error)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("failed to init mutex object")));
		return NULL;
	}
	/* add this block into private tracker */
	dlist_push_tail(&shmem_private_track, &block->free_list);

	/*
	 * Some fundamental members has to be initialized correctly because
	 * resource cleanup routine tries to synchronize completion of the
	 * execution of this chunk, and also tries to release varalena-
	 * buffers relevant to this chunk-buffer.
	 */
	chunk->recvq = NULL;
	chunk->kernel_params = NULL;
	dlist_init(&chunk->vlbuf_list);
	chunk->is_loaded = false;
	chunk->is_running = false;

	return chunk;
}

void
pgstrom_chunk_buffer_free(ChunkBuffer *chunk)
{
	dlist_mutable_iter	iter;
	ShmemBlock *block = container_of(ShmemBlock, data, chunk);

	Assert(block->magic == SHMEM_BLOCK_CHUNK_BUFFER);

	/*
	 * Note: we assume any chunk-buffers shall be released by the process
	 * that acquired this buffer, thus, it is safe to touch rs_memcxt and
	 * rs_cache pointers that have private pointers.
	 * The assertion below is a test to check whether this context is
	 * identical with the one on allocation.
	 */
	Assert(block->pid == getpid());
	if (chunk->rs_memcxt)
		MemoryContextDelete(chunk->rs_memcxt);
	if (chunk->rs_cache)
		pfree(chunk->rs_cache);

	dlist_foreach_modify(iter, &chunk->vlbuf_list)
	{
		VarlenaBuffer  *vlbuf
			= dlist_container(VarlenaBuffer, chain, iter.cur);

		pgstrom_varlena_buffer_free(vlbuf);
	}

	/* untrack this block in private tracker */
	dlist_delete(&block->free_list);

	/* release it */
	pthread_mutex_destroy(&chunk->lock);
	pthread_cond_destroy(&chunk->cond);
	pgstrom_shmem_free(block);
}

/*
 * pgstrom_shmem_startup
 *
 * A callback routine during initialization of shared memory segment.
 * It acquires shared memory segment from the core, and initializes
 * this region for future allocation for chunk-buffers and so on.
 */
static void
pgstrom_shmem_startup(void)
{
	ShmemBlock *block;
	Size		segment_sz = (pgstrom_shmem_size << 20);
	bool		found;

	/* call the startup hook */
	if (shmem_startup_hook_next)
		(*shmem_startup_hook_next)();

	/* acquire shared memory segment */
	pgstrom_shmem_head = ShmemInitStruct("shared memory segment of PG-Strom",
										 segment_sz, &found);
	Assert(!found);

	/* init ShmemHead field */
	pgstrom_shmem_head->total_size
		= segment_sz - offsetof(ShmemHead, first_block);
	pgstrom_shmem_head->free_size = pgstrom_shmem_head->total_size;
	dlist_init(&pgstrom_shmem_head->free_head);
	dlist_init(&pgstrom_shmem_head->addr_head);
	if (!pgstrom_mutex_init(&pgstrom_shmem_head->lock))
		elog(ERROR, "failed to init mutex lock");

	if (!pgstrom_rwlock_init(&pgstrom_shmem_head->dev_lock))
		elog(ERROR, "failed to init read-write lock");
	dlist_init(&pgstrom_shmem_head->dev_head);

	/* init ShmemBlock as an empty big block */
	block = pgstrom_shmem_head->first_block;
	block->magic = SHMEM_BLOCK_FREE;
	dlist_push_head(&pgstrom_shmem_head->addr_head, &block->addr_list);
	dlist_push_head(&pgstrom_shmem_head->free_head, &block->free_list);
	block->size = pgstrom_shmem_head->total_size;
}

void
pgstrom_shmem_init(void)
{
	/* prepare mutex-attribute on shared memory segment */
	if (pthread_mutexattr_init(&shmem_mutex_attr) != 0 ||
		pthread_mutexattr_setpshared(&shmem_mutex_attr,
									 PTHREAD_PROCESS_SHARED) != 0)
		elog(ERROR, "failed to init mutex attribute");

	/* prepare rwlock-attribute on shared memory segment */
	if (pthread_rwlockattr_init(&shmem_rwlock_attr) != 0 ||
		pthread_rwlockattr_setpshared(&shmem_rwlock_attr,
									  PTHREAD_PROCESS_SHARED) != 0)
		elog(ERROR, "failed to init rwlock attribute");

	/* prepare cond-attribute on shared memory segment */
	if (pthread_condattr_init(&shmem_cond_attr) != 0 ||
		pthread_condattr_setpshared(&shmem_cond_attr,
									PTHREAD_PROCESS_SHARED) != 0)
		elog(ERROR, "failed to init condition attribute");

	/* GUC */
	DefineCustomIntVariable("pg_strom.shmem_size",
							"size of shared memory segment in MB",
							NULL,
							&pgstrom_shmem_size,
							256,	/* 256MB */
							64,		/* 64MB */
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	/* acquire shared memory segment */
	RequestAddinShmemSpace(pgstrom_shmem_size << 20);
	shmem_startup_hook_next = shmem_startup_hook;
	shmem_startup_hook = pgstrom_shmem_startup;

	/* init private list to track acquired memory blocks */
	dlist_init(&shmem_private_track);

	/* registration of shared-memory cleanup handler  */
	RegisterResourceReleaseCallback(pgstrom_shmem_cleanup, NULL);
}
