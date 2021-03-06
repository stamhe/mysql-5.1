/*****************************************************************************

Copyright (c) 1995, 2011, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0lru.c
The database buffer replacement algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0lru.h"

#ifdef UNIV_NONINL
#include "buf0lru.ic"
#endif

#include "ut0byte.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "sync0sync.h"
#include "sync0rw.h"
#include "hash0hash.h"
#include "os0sync.h"
#include "fil0fil.h"
#include "btr0btr.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "os0file.h"
#include "page0zip.h"
#include "log0recv.h"
#include "srv0srv.h"
#include "srv0start.h"

/** The number of blocks from the LRU_old pointer onward, including
the block pointed to, must be buf_LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV
of the whole LRU list length, except that the tolerance defined below
is allowed. Note that the tolerance must be small enough such that for
even the BUF_LRU_OLD_MIN_LEN long LRU list, the LRU_old pointer is not
allowed to point to either end of the LRU list. */

#define BUF_LRU_OLD_TOLERANCE	20

/** The minimum amount of non-old blocks when the LRU_old list exists
(that is, when there are more than BUF_LRU_OLD_MIN_LEN blocks).
@see buf_LRU_old_adjust_len */
#define BUF_LRU_NON_OLD_MIN_LEN	5
#if BUF_LRU_NON_OLD_MIN_LEN >= BUF_LRU_OLD_MIN_LEN
# error "BUF_LRU_NON_OLD_MIN_LEN >= BUF_LRU_OLD_MIN_LEN"
#endif

/** When dropping the search hash index entries before deleting an ibd
file, we build a local array of pages belonging to that tablespace
in the buffer pool. Following is the size of that array.
We also release buf_pool->mutex after scanning this many pages of the
flush_list when dropping a table. This is to ensure that other threads
are not blocked for extended period of time when using very large
buffer pools. */
#define BUF_LRU_DROP_SEARCH_SIZE	1024

/** If we switch on the InnoDB monitor because there are too few available
frames in the buffer pool, we set this to TRUE */
static ibool	buf_lru_switched_on_innodb_mon	= FALSE;

/******************************************************************//**
These statistics are not 'of' LRU but 'for' LRU.  We keep count of I/O
and page_zip_decompress() operations.  Based on the statistics,
buf_LRU_evict_from_unzip_LRU() decides if we want to evict from
unzip_LRU or the regular LRU.  From unzip_LRU, we will only evict the
uncompressed frame (meaning we can evict dirty blocks as well).  From
the regular LRU, we will evict the entire block (i.e.: both the
uncompressed and compressed data), which must be clean. */

/* @{ */

/** Number of intervals for which we keep the history of these stats.
Each interval is 1 second, defined by the rate at which
srv_error_monitor_thread() calls buf_LRU_stat_update(). */
#define BUF_LRU_STAT_N_INTERVAL 50

/** Sampled values buf_LRU_stat_cur.
Protected by buf_pool_mutex.  Updated by buf_LRU_stat_update(). */
static buf_LRU_stat_t		buf_LRU_stat_arr[BUF_LRU_STAT_N_INTERVAL];
/** Cursor to buf_LRU_stat_arr[] that is updated in a round-robin fashion. */
static ulint			buf_LRU_stat_arr_ind;

/** Current operation counters.  Not protected by any mutex.  Cleared
by buf_LRU_stat_update(). */
UNIV_INTERN buf_LRU_stat_t	buf_LRU_stat_cur;

/** Running sum of past values of buf_LRU_stat_cur.
Updated by buf_LRU_stat_update().  Protected by buf_pool_mutex. */
UNIV_INTERN buf_LRU_stat_t	buf_LRU_stat_sum;

/* @} */

/** @name Heuristics for detecting index scan @{ */
/** Reserve this much/BUF_LRU_OLD_RATIO_DIV of the buffer pool for
"old" blocks.  Protected by buf_pool_mutex. */
UNIV_INTERN uint	buf_LRU_old_ratio;
/** Move blocks to "new" LRU list only if the first access was at
least this many milliseconds ago.  Not protected by any mutex or latch. */
UNIV_INTERN uint	buf_LRU_old_threshold_ms;
/* @} */

/******************************************************************//**
Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed and buf_pool_zip_mutex will be released.

If a compressed page or a compressed-only block descriptor is freed,
other compressed pages or compressed-only block descriptors may be
relocated.
@return the new state of the block (BUF_BLOCK_ZIP_FREE if the state
was BUF_BLOCK_ZIP_PAGE, or BUF_BLOCK_REMOVE_HASH otherwise) */
static
enum buf_page_state
buf_LRU_block_remove_hashed_page(
/*=============================*/
	buf_page_t*	bpage,	/*!< in: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
	ibool		zip);	/*!< in: TRUE if should remove also the
				compressed page of an uncompressed page */
/******************************************************************//**
Puts a file page whose has no hash index to the free list. */
static
void
buf_LRU_block_free_hashed_page(
/*===========================*/
	buf_block_t*	block);	/*!< in: block, must contain a file page and
				be in a state where it can be freed */

/******************************************************************//**
Determines if the unzip_LRU list should be used for evicting a victim
instead of the general LRU list.
@return	TRUE if should use unzip_LRU */
UNIV_INLINE
ibool
buf_LRU_evict_from_unzip_LRU(void)
/*==============================*/
{
	double	io_avg;
	double	unzip_avg;
	double	unzip_len;
	double	lru_len;

	ut_ad(buf_pool_mutex_own());

	/* If the unzip_LRU list is empty, we can only use the LRU. */
	if (UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0) {
		return(FALSE);
	}

	/* If unzip_LRU is at most 10% of the size of the LRU list,
	then use the LRU.  This slack allows us to keep hot
	decompressed pages in the buffer pool. */
	unzip_len	= ut_max(UT_LIST_GET_LEN(buf_pool->unzip_LRU), 1);
	lru_len		= ut_max(UT_LIST_GET_LEN(buf_pool->LRU), 1);

	if (((100 * unzip_len) / lru_len) <= srv_unzip_LRU_pct)
		return FALSE; 

	/* If eviction hasn't started yet, we assume by default
	that a workload is disk bound. */
	if (buf_pool->freed_page_clock == 0) {
		return(TRUE);
	}

	/* Calculate the average over past intervals, and add the values
	of the current interval. */
	io_avg = (double) buf_LRU_stat_sum.io / BUF_LRU_STAT_N_INTERVAL
		+ buf_LRU_stat_cur.io;
	unzip_avg = (double) buf_LRU_stat_sum.unzip / BUF_LRU_STAT_N_INTERVAL
		+ buf_LRU_stat_cur.unzip;

	/* Decide based on our formula.  If the load is I/O bound
	(unzip_avg is smaller than the weighted io_avg), evict an
	uncompressed frame from unzip_LRU.  Otherwise we assume that
	the load is CPU bound and evict from the regular LRU. */
	return(unzip_avg <= io_avg * srv_lru_io_to_unzip_factor);
}

/******************************************************************//**
Attempts to drop page hash index on a batch of pages belonging to a
particular space id. */
static
void
buf_LRU_drop_page_hash_batch(
/*=========================*/
	ulint		space_id,	/*!< in: space id */
	ulint		zip_size,	/*!< in: compressed page size in bytes
					or 0 for uncompressed pages */
	const ulint*	arr,		/*!< in: array of page_no */
	ulint		count)		/*!< in: number of entries in array */
{
	ulint	i;

	ut_ad(arr != NULL);
	ut_ad(count <= BUF_LRU_DROP_SEARCH_SIZE);

	for (i = 0; i < count; ++i) {
		btr_search_drop_page_hash_when_freed(space_id, zip_size,
						     arr[i]);
	}
}

/******************************************************************//**
When doing a DROP TABLE/DISCARD TABLESPACE we have to drop all page
hash index entries belonging to that table. This function tries to
do that in batch. Note that this is a 'best effort' attempt and does
not guarantee that ALL hash entries will be removed. Returns the
number of pages that might have been hashed. */
static
ulint
buf_LRU_drop_page_hash_for_tablespace(
/*==================================*/
	ulint	id)	/*!< in: space id */
{
	buf_page_t*	bpage;
	ulint*		page_arr;
	ulint		num_entries;
	ulint		zip_size;
	ulint		num_found = 0;

	zip_size = fil_space_get_zip_size(id);

	if (UNIV_UNLIKELY(zip_size == ULINT_UNDEFINED)) {
		/* Somehow, the tablespace does not exist.  Nothing to drop. */
		ut_ad(0);
		return 0;
	}

	page_arr = ut_malloc(sizeof(ulint)
			     * BUF_LRU_DROP_SEARCH_SIZE);
	buf_pool_mutex_enter();
	num_entries = 0;

scan_again:
	bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	while (bpage != NULL) {
		buf_page_t*	prev_bpage;
		ibool	is_fixed;

		prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		ut_a(buf_page_in_file(bpage));

		if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE
		    || bpage->space != id
		    || bpage->io_fix != BUF_IO_NONE) {
			/* Compressed pages are never hashed.
			Skip blocks of other tablespaces.
			Skip I/O-fixed blocks (to be dealt with later). */
next_page:
			bpage = prev_bpage;
			continue;
		}

		mutex_enter(&((buf_block_t*) bpage)->mutex);
		is_fixed = bpage->buf_fix_count > 0
			|| !((buf_block_t*) bpage)->index;
		mutex_exit(&((buf_block_t*) bpage)->mutex);

		if (is_fixed) {
			goto next_page;
		}

		/* Store the page number so that we can drop the hash
		index in a batch later. */
		page_arr[num_entries] = bpage->offset;
		ut_a(num_entries < BUF_LRU_DROP_SEARCH_SIZE);
		++num_entries;
		++num_found;

		if (num_entries < BUF_LRU_DROP_SEARCH_SIZE) {
			goto next_page;
		}

		/* Array full. We release the buf_pool_mutex to
		obey the latching order. */
		buf_pool_mutex_exit();
		buf_LRU_drop_page_hash_batch(id, zip_size, page_arr,
					     num_entries);
		buf_pool_mutex_enter();
		num_entries = 0;

		/* Note that we released the buf_pool mutex above
		after reading the prev_bpage during processing of a
		page_hash_batch (i.e.: when the array was full).
		Because prev_bpage could belong to a compressed-only
		block, it may have been relocated, and thus the
		pointer cannot be trusted. Because bpage is of type
		buf_block_t, it is safe to dereference.
		bpage can change in the LRU list. This is OK because
		this function is a 'best effort' to drop as many
		search hash entries as possible and it does not
		guarantee that ALL such entries will be dropped. */
		/* If, however, bpage has been removed from LRU list
		to the free list then we should restart the scan.
		bpage->state is protected by buf_pool mutex. */
		if (bpage
		    && buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
			goto scan_again;
		}
	}

	buf_pool_mutex_exit();

	/* Drop any remaining batch of search hashed pages. */
	buf_LRU_drop_page_hash_batch(id, zip_size, page_arr, num_entries);
	ut_free(page_arr);

	return num_found;
}

/******************************************************************//**
While flushing (or removing dirty) pages from a tablespace we don't
want to hog the CPU and resources. Release the buffer pool and block
mutex and try to force a context switch. Then reacquire the same mutexes.
The current page is "fixed" before the release of the mutexes and then
"unfixed" again once we have reacquired the mutexes. */
static
void
buf_flush_yield(
/*============*/
	buf_page_t*	bpage)		/*!< in/out: current page */
{
	mutex_t*	block_mutex;

	ut_ad(buf_pool_mutex_own());
	ut_ad(buf_page_in_file(bpage));

	block_mutex = buf_page_get_mutex(bpage);

	mutex_enter(block_mutex);
	/* "Fix" the block so that the position cannot be
	changed after we release the buffer pool and
	block mutexes. */
	buf_page_set_sticky(bpage);

	/* Now it is safe to release the buf_pool->mutex. */
	buf_pool_mutex_exit();

	mutex_exit(block_mutex);
	/* Try and force a context switch. */
	os_thread_yield();

	buf_pool_mutex_enter();

	mutex_enter(block_mutex);
	/* "Unfix" the block now that we have both the
	buffer pool and block mutex again. */
	buf_page_unset_sticky(bpage);
	mutex_exit(block_mutex);
}

/******************************************************************//**
If we have hogged the resources for too long then release the buffer
pool and flush list mutex and do a thread yield. Set the current page
to "sticky" so that it is not relocated during the yield.
@return TRUE if yielded */
static
ibool
buf_flush_try_yield(
/*================*/
	buf_page_t*	bpage,		/*!< in/out: bpage to remove */
	ulint		processed)	/*!< in: number of pages processed */
{
	ut_ad(buf_pool_mutex_own());

	/* Every BUF_LRU_DROP_SEARCH_SIZE iterations in the
	loop we release buf_pool->mutex to let other threads
	do their job but only if the block is not IO fixed. This
	ensures that the block stays in its position in the
	flush_list. */

	if (bpage != NULL
	    && processed >= BUF_LRU_DROP_SEARCH_SIZE
	    && buf_page_get_io_fix(bpage) == BUF_IO_NONE) {

		/* flush_list mutex arrives in 5.5, OK to ignore */
		/* buf_flush_list_mutex_exit(); */

		/* Release the buffer pool and block mutex
		to give the other threads a go. */

		buf_flush_yield(bpage);

		/* buf_flush_list_mutex_enter(); */

		/* Should not have been removed from the flush
		list during the yield. However, this check is
		not sufficient to catch a remove -> add. */

		ut_ad(bpage->in_flush_list);

		return(TRUE);
	}

	return(FALSE);
}

/******************************************************************//**
Removes a single page from a given tablespace inside a specific
buffer pool instance.
@return TRUE if page was removed. */
static
ibool
buf_flush_or_remove_page(
/*=====================*/
	buf_page_t*	bpage)		/*!< in/out: bpage to remove */
{
	mutex_t*	block_mutex;
	ibool		processed = FALSE;

	ut_ad(buf_pool_mutex_own());
	/* flush_list mutex arrives in 5.5, OK to ignore */
	/* ut_ad(buf_flush_list_mutex_own()); */

	block_mutex = buf_page_get_mutex(bpage);

	/* bpage->space and bpage->io_fix are protected by
	buf_pool->mutex and block_mutex. It is safe to check
	them while holding buf_pool->mutex only. */

	if (buf_page_get_io_fix(bpage) != BUF_IO_NONE) {

		/* We cannot remove this page during this scan
		yet; maybe the system is currently reading it
		in, or flushing the modifications to the file */

	} else {

		/* We have to release the flush_list_mutex to obey the
		latching order. We are however guaranteed that the page
		will stay in the flush_list because buf_flush_remove()
		needs buf_pool->mutex as well (for the non-flush case). */

		/* flush_list mutex arrives in 5.5, OK to ignore */
		/* buf_flush_list_mutex_exit(); */

		mutex_enter(block_mutex);

		ut_ad(bpage->oldest_modification != 0);

		if (bpage->buf_fix_count == 0) {

			buf_flush_remove(bpage);

			processed = TRUE;
		}

		mutex_exit(block_mutex);

		/* buf_flush_list_mutex_enter(); */
	}

	ut_ad(!mutex_own(block_mutex));

	return(processed);
}

/******************************************************************//**
Remove all dirty pages belonging to a given tablespace inside a specific
buffer pool instance when we are deleting the data file(s) of that
tablespace. The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@return TRUE if all freed. */
static
ibool
buf_flush_or_remove_pages(
/*======================*/
	ulint		id)		/*!< in: target space id for which
					to remove or flush pages */
{
	buf_page_t*	prev;
	buf_page_t*	bpage;
	ulint		processed = 0;
	ibool		all_freed = TRUE;

	ut_ad(buf_pool_mutex_own());

	/* flush_list mutex arrives in 5.5, OK to ignore */
	/* buf_flush_list_mutex_enter(); */

	for (bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
	     bpage != NULL;
	     bpage = prev) {

		ut_a(buf_page_in_file(bpage));
		ut_ad(bpage->in_flush_list);

		/* Save the previous link because once we free the
		page we can't rely on the links. */

		prev = UT_LIST_GET_PREV(list, bpage);

		if (buf_page_get_space(bpage) != id) {

			/* Skip this block, as it does not belong to
			the target space. */

		} else if (!buf_flush_or_remove_page(bpage)) {

			/* Remove was unsuccessful, we have to try again
			by scanning the entire list from the end. */

			all_freed = FALSE;
		}

		++processed;

		/* Yield if we have hogged the CPU and mutexes for too long. */
		if (buf_flush_try_yield(prev, processed)) {

			/* Reset the batch size counter if we had to yield. */

			processed = 0;
		}

	}

	/* buf_flush_list_mutex_exit(); */

	return(all_freed);
}

/******************************************************************//**
Remove or flush all the dirty pages that belong to a given tablespace
inside a specific buffer pool instance. The pages will remain in the LRU
list and will be evicted from the LRU list as they age and move towards
the tail of the LRU list. */
static
void
buf_flush_dirty_pages(
/*==================*/
	ulint		id)		/*!< in: space id */
{
	ibool	all_freed;

	do {
		buf_pool_mutex_enter();

		all_freed = buf_flush_or_remove_pages(id);

		buf_pool_mutex_exit();

		ut_ad(buf_flush_validate());

		if (!all_freed) {
			os_thread_sleep(20000);
		}

	} while (!all_freed);
}

/******************************************************************//**
Remove all pages that belong to a given tablespace inside a specific
buffer pool instance when we are DISCARDing the tablespace. */
static
void
buf_LRU_remove_all_pages(
/*=====================*/
	ulint		id)		/*!< in: space id */
{
	buf_page_t*	bpage;
	ibool		all_freed;

scan_again:
	buf_pool_mutex_enter();

	all_freed = TRUE;

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     bpage != NULL;
	     /* No op */) {

		buf_page_t*	prev_bpage;
		mutex_t*	block_mutex = NULL;

		ut_a(buf_page_in_file(bpage));
		ut_ad(bpage->in_LRU_list);

		prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		/* bpage->space and bpage->io_fix are protected by
		buf_pool->mutex and the block_mutex. It is safe to check
		them while holding buf_pool->mutex only. */

		if (buf_page_get_space(bpage) != id) {
			/* Skip this block, as it does not belong to
			the space that is being invalidated. */
			goto next_page;
		} else if (buf_page_get_io_fix(bpage) != BUF_IO_NONE) {
			/* We cannot remove this page during this scan
			yet; maybe the system is currently reading it
			in, or flushing the modifications to the file */

			all_freed = FALSE;
			goto next_page;
		} else {
			block_mutex = buf_page_get_mutex(bpage);
			mutex_enter(block_mutex);

			if (bpage->buf_fix_count > 0) {

				mutex_exit(block_mutex);
				/* We cannot remove this page during
				this scan yet; maybe the system is
				currently reading it in, or flushing
				the modifications to the file */

				all_freed = FALSE;

				goto next_page;
			}
		}

		ut_ad(mutex_own(block_mutex));

#ifdef UNIV_DEBUG
		if (buf_debug_prints) {
			fprintf(stderr,
				"Dropping space %lu page %lu\n",
				(ulong) buf_page_get_space(bpage),
				(ulong) buf_page_get_page_no(bpage));
		}
#endif
		if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
			/* Do nothing, because the adaptive hash index
			covers uncompressed pages only. */
		} else if (((buf_block_t*) bpage)->index) {
			ulint	page_no;
			ulint	zip_size;

			buf_pool_mutex_exit();

			zip_size = buf_page_get_zip_size(bpage);
			page_no = buf_page_get_page_no(bpage);

			mutex_exit(block_mutex);

			/* Note that the following call will acquire
			and release an X-latch on the page. */

			btr_search_drop_page_hash_when_freed(
				id, zip_size, page_no);

			goto scan_again;
		}

		if (bpage->oldest_modification != 0) {
			buf_flush_remove(bpage);
		}

		ut_ad(!bpage->in_flush_list);

		/* Remove from the LRU list. */

		if (buf_LRU_block_remove_hashed_page(bpage, TRUE)
		    != BUF_BLOCK_ZIP_FREE) {
			buf_LRU_block_free_hashed_page((buf_block_t*) bpage);
			mutex_exit(block_mutex);
		} else {
			/* The block_mutex should have been released
			by buf_LRU_block_remove_hashed_page() when it
			returns BUF_BLOCK_ZIP_FREE. */
			ut_ad(block_mutex == &buf_pool_zip_mutex);
		}

		ut_ad(!mutex_own(block_mutex));
next_page:
		bpage = prev_bpage;
	}

	buf_pool_mutex_exit();

	if (!all_freed) {
		os_thread_sleep(20000);

		goto scan_again;
	}
}

/******************************************************************//**
Removes all pages belonging to a given tablespace. */
UNIV_INTERN
void
buf_LRU_flush_or_remove_pages(
/*==========================*/
	ulint			id,	/*!< in: space id */
	enum buf_remove_t	buf_remove)/*!< in: remove or flush
					strategy */
{
	switch (buf_remove) {
	case BUF_REMOVE_ALL_NO_WRITE:
		/* A DISCARD tablespace case. Remove AHI entries
		and evict all pages from LRU. */

		/* Before we attempt to drop pages hash entries
		one by one we first attempt to drop page hash
		index entries in batches to make it more
		efficient. The batching attempt is a best effort
		attempt and does not guarantee that all pages
		hash entries will be dropped. We get rid of
		remaining page hash entries one by one below. */
		buf_LRU_drop_page_hash_for_tablespace(id);
		buf_LRU_remove_all_pages(id);
		break;

	case BUF_REMOVE_FLUSH_NO_WRITE:
		/* Be paranoid and confirm other code removed the AHI entries.
		Doing this in non-debug builds would make DROP TABLE slow. */
		ut_ad(buf_LRU_drop_page_hash_for_tablespace(id) == 0);

		/* A DROP table case. AHI entries are already
		removed. No need to evict all pages from LRU
		list. Just evict pages from flush list without
		writing. */
		buf_flush_dirty_pages(id);
		break;
	}
}

/* zip_clean has been made debug only. See the field declaration. */

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/********************************************************************//**
Insert a compressed block into buf_pool->zip_clean in the LRU order. */
UNIV_INTERN
void
buf_LRU_insert_zip_clean(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_page_t*	b;

	ut_ad(buf_pool_mutex_own());
	ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_PAGE);

	/* Find the first successor of bpage in the LRU list
	that is in the zip_clean list. */
	b = bpage;
	do {
		b = UT_LIST_GET_NEXT(LRU, b);
	} while (b && buf_page_get_state(b) != BUF_BLOCK_ZIP_PAGE);

	/* Insert bpage before b, i.e., after the predecessor of b. */
	if (b) {
		b = UT_LIST_GET_PREV(list, b);
	}

	if (b) {
		UT_LIST_INSERT_AFTER(list, buf_pool->zip_clean, b, bpage);
	} else {
		UT_LIST_ADD_FIRST(list, buf_pool->zip_clean, bpage);
	}
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/******************************************************************//**
Try to free an uncompressed page of a compressed block from the unzip
LRU list.  The compressed page is preserved, and it need not be clean.
@return	TRUE if freed */
UNIV_INLINE
ibool
buf_LRU_free_from_unzip_LRU_list(
/*=============================*/
	ulint	n_iterations)	/*!< in: how many times this has been called
				repeatedly without result: a high value means
				that we should search farther; we will search
				n_iterations / 5 of the unzip_LRU list,
				or nothing if n_iterations >= 5 */
{
	buf_block_t*	block;
	ulint		distance;

	ut_ad(buf_pool_mutex_own());

	/* Theoratically it should be much easier to find a victim
	from unzip_LRU as we can choose even a dirty block (as we'll
	be evicting only the uncompressed frame).  In a very unlikely
	eventuality that we are unable to find a victim from
	unzip_LRU, we fall back to the regular LRU list.  We do this
	if we have done five iterations so far. */

	if (UNIV_UNLIKELY(n_iterations >= 5)
	    || !buf_LRU_evict_from_unzip_LRU()) {

		return(FALSE);
	}

	distance = 100 + (n_iterations
			  * UT_LIST_GET_LEN(buf_pool->unzip_LRU)) / 5;

	for (block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);
	     UNIV_LIKELY(block != NULL) && UNIV_LIKELY(distance > 0);
	     block = UT_LIST_GET_PREV(unzip_LRU, block), distance--) {

		ibool freed;
		ibool removed;

		ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);

		mutex_enter(&block->mutex);
		freed = buf_LRU_free_block(&block->page, FALSE, &removed);

		/* With zip=FALSE in call to buf_LRU_free_block the compressed
		page must remain on the LRU */
		ut_ad(!removed);

		mutex_exit(&block->mutex);

		if (freed) {
			return(TRUE);
		}
	}

	return(FALSE);
}

/******************************************************************//**
Try to free a clean page from the common LRU list.
@return	TRUE if freed */
UNIV_INLINE
ibool
buf_LRU_free_from_common_LRU_list(
/*==============================*/
	ulint	n_iterations,	/*!< in: how many times this has been called
				repeatedly without result: a high value means
				that we should search farther; if
				n_iterations < 10, then we search
				n_iterations / 10 * buf_pool->curr_size
				pages from the end of the LRU list */
	ulint*	space_id,	/*!<: out: space_id for freed page */
	ulint*	nsearched,	/*!< out: #blocks checked */
	ulint	limit)		/*!< in: when not 0 search at most this number
				of pages */
{
	buf_page_t*	bpage;
	ulint		distance;
	ulint		init_distance;

	*space_id = ULINT_UNDEFINED;
	ut_ad(buf_pool_mutex_own());

	if (!limit)
		distance = 100 + (n_iterations * buf_pool->curr_size) / 10;
	else
		distance = limit;

	init_distance = distance;

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     UNIV_LIKELY(bpage != NULL) && UNIV_LIKELY(distance > 0);
	     bpage = UT_LIST_GET_PREV(LRU, bpage), distance--) {

		ibool		freed;
		my_fast_timer_t	accessed;
		mutex_t*	block_mutex = buf_page_get_mutex(bpage);
		ibool		removed;

		ut_ad(buf_page_in_file(bpage));
		ut_ad(bpage->in_LRU_list);

		mutex_enter(block_mutex);
		buf_page_is_accessed(bpage, &accessed);
		*space_id = bpage->space;
		freed = buf_LRU_free_block(bpage, TRUE, &removed);
		mutex_exit(block_mutex);

		if (!removed)
			*space_id = ULINT_UNDEFINED;

		if (freed) {
			/* Keep track of pages that are evicted without
			ever being accessed. This gives us a measure of
			the effectiveness of readahead */
			if (!my_fast_timer_is_valid(&accessed)) {
				++buf_pool->stat.n_ra_pages_evicted;
			}
			*nsearched = init_distance - distance + 1;
			return(TRUE);
		}
	}

	*nsearched = init_distance - distance + 1;
	return(FALSE);
}

/******************************************************************//**
Try to free a replaceable block.
@return	TRUE if found and freed */
UNIV_INTERN
ibool
buf_LRU_search_and_free_block(
/*==========================*/
	ulint	n_iterations,	/*!< in: how many times this has been called
				repeatedly without result: a high value means
				that we should search farther; if
				n_iterations < 10, then we search
				n_iterations / 10 * buf_pool->curr_size
				pages from the end of the LRU list; if
				n_iterations < 5, then we will also search
				n_iterations / 5 of the unzip_LRU list. */
	buf_block_t**	block,	/*!< in/out: if block != NULL then this
				can return a pointer to a free block. */
	ibool		locked,	/*!< in: when TRUE the buffer pool mutex
				is locked by the caller. Buffer pool mutex
				is always unlocked when this returns. */
	ulint*		nsearched)/*!< out: #blocks checked in the common LRU */
{
	ibool	freed = FALSE;
	ulint	space_id = ULINT_UNDEFINED;

	ut_ad(*nsearched == 0);

	if (!locked)
		buf_pool_mutex_enter();

	freed = buf_LRU_free_from_unzip_LRU_list(n_iterations);

	if (!freed) {
		/* Limit how far back from the LRU a search will be done when
		innodb_fast_free_list is ON and this was called by
		buf_LRU_get_free_block. Without a limit this can search too far
		into the LRU. This is not needed when innodb_fast_free_list is
		OFF because buf_flush_free_margin is always called after a free
		page was allocated during a read and the reading thread will get
		stuck in buf_flush_free_margin waiting for a flush to finish.
		*/
		ulint limit	= 0;
		if (block && srv_fast_free_list && n_iterations == 1)
			limit = BUF_LRU_FREE_SEARCH_LEN;

		freed = buf_LRU_free_from_common_LRU_list(n_iterations, &space_id,
				 			  nsearched, limit);
	}

	if (!freed) {
		buf_pool->LRU_flush_ended = 0;
	} else {
		if (buf_pool->LRU_flush_ended > 0) {
			buf_pool->LRU_flush_ended--;
		}

		if (block) {
			/* Get a free block before releasing the buffer pool mutex */
			*block = buf_LRU_get_free_only();
		}
	}

	buf_pool_mutex_exit();

	if (space_id != ULINT_UNDEFINED)
		fil_change_lru_count(space_id, -1);

	return(freed);
}

/******************************************************************//**
Tries to remove LRU flushed blocks from the end of the LRU list and put them
to the free list. This is beneficial for the efficiency of the insert buffer
operation, as flushed pages from non-unique non-clustered indexes are here
taken out of the buffer pool, and their inserts redirected to the insert
buffer. Otherwise, the flushed blocks could get modified again before read
operations need new buffer blocks, and the i/o work done in flushing would be
wasted. */
UNIV_INTERN
void
buf_LRU_try_free_flushed_blocks(void)
/*=================================*/
{
	buf_pool_mutex_enter();

	while (buf_pool->LRU_flush_ended > 0) {
		ulint	unused	= 0;

		buf_pool_mutex_exit();

		buf_LRU_search_and_free_block(1, NULL, FALSE, &unused);

		buf_pool_mutex_enter();
	}

	buf_pool_mutex_exit();
}

/******************************************************************//**
Returns TRUE if less than 25 % of the buffer pool is available. This can be
used in heuristics to prevent huge transactions eating up the whole buffer
pool for their locks.
@return	TRUE if less than 25 % of buffer pool left */
UNIV_INTERN
ibool
buf_LRU_buf_pool_running_out(void)
/*==============================*/
{
	ibool	ret	= FALSE;

	buf_pool_mutex_enter();

	if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free)
	    + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->curr_size / 4) {

		ret = TRUE;
	}

	buf_pool_mutex_exit();

	return(ret);
}

/******************************************************************//**
Returns a free block from the buf_pool.  The block is taken off the
free list.  If it is empty, returns NULL.
@return	a free control block, or NULL if the buf_block->free list is empty */
UNIV_INTERN
buf_block_t*
buf_LRU_get_free_only(void)
/*=======================*/
{
	buf_block_t*	block;

	ut_ad(buf_pool_mutex_own());

	block = (buf_block_t*) UT_LIST_GET_FIRST(buf_pool->free);

	if (block) {
		ut_ad(block->page.in_free_list);
		ut_d(block->page.in_free_list = FALSE);
		ut_ad(!block->page.in_flush_list);
		ut_ad(!block->page.in_LRU_list);
		ut_a(!buf_page_in_file(&block->page));
		UT_LIST_REMOVE(list, buf_pool->free, (&block->page));

		mutex_enter(&block->mutex);

		buf_block_set_state(block, BUF_BLOCK_READY_FOR_USE);
		UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);

		mutex_exit(&block->mutex);
	}

	return(block);
}


/******************************************************************//**
Prepares a free block to be used. */
static void
buf_LRU_prepare_free_block(
/*===================================*/
	buf_block_t*	block,		/*!< in: free block to be used */
	ibool		started_monitor,/*!< in: see caller */
	ibool		mon_value_was)	/*!< in: see caller */
{
	memset(&block->page.zip, 0, sizeof block->page.zip);

	if (started_monitor) {
		srv_print_innodb_monitor = mon_value_was;
	}
}


/******************************************************************//**
Returns a free block from the buf_pool. The block is taken off the
free list. If it is empty, blocks are moved from the end of the
LRU list to the free list.
@return	the free control block, in state BUF_BLOCK_READY_FOR_USE */
UNIV_INTERN
buf_block_t*
buf_LRU_get_free_block(
/*========================*/
	ulint*	nsearched)	/*!< out: #blocks checked on the LRU to find
				a free one */
{
	buf_block_t*	block		= NULL;
	ibool		freed;
	ulint		n_iterations	= 1;
	ibool		mon_value_was	= FALSE;
	ibool		started_monitor	= FALSE;
loop:
	buf_pool_mutex_enter();

	if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free)
	    + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->curr_size / 20) {
		ut_print_timestamp(stderr);

		fprintf(stderr,
			"  InnoDB: ERROR: over 95 percent of the buffer pool"
			" is occupied by\n"
			"InnoDB: lock heaps or the adaptive hash index!"
			" Check that your\n"
			"InnoDB: transactions do not set too many row locks.\n"
			"InnoDB: Your buffer pool size is %lu MB."
			" Maybe you should make\n"
			"InnoDB: the buffer pool bigger?\n"
			"InnoDB: We intentionally generate a seg fault"
			" to print a stack trace\n"
			"InnoDB: on Linux!\n",
			(ulong) (buf_pool->curr_size
				 / (1024 * 1024 / UNIV_PAGE_SIZE)));

		ut_error;

	} else if (!recv_recovery_on
		   && (UT_LIST_GET_LEN(buf_pool->free)
		       + UT_LIST_GET_LEN(buf_pool->LRU))
		   < buf_pool->curr_size / 3) {

		if (!buf_lru_switched_on_innodb_mon) {

			/* Over 67 % of the buffer pool is occupied by lock
			heaps or the adaptive hash index. This may be a memory
			leak! */

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: WARNING: over 67 percent of"
				" the buffer pool is occupied by\n"
				"InnoDB: lock heaps or the adaptive"
				" hash index! Check that your\n"
				"InnoDB: transactions do not set too many"
				" row locks.\n"
				"InnoDB: Your buffer pool size is %lu MB."
				" Maybe you should make\n"
				"InnoDB: the buffer pool bigger?\n"
				"InnoDB: Starting the InnoDB Monitor to print"
				" diagnostics, including\n"
				"InnoDB: lock heap and hash index sizes.\n",
				(ulong) (buf_pool->curr_size
					 / (1024 * 1024 / UNIV_PAGE_SIZE)));

			buf_lru_switched_on_innodb_mon = TRUE;
			srv_print_innodb_monitor = TRUE;
			os_event_set(srv_lock_timeout_thread_event);
		}
	} else if (buf_lru_switched_on_innodb_mon) {

		/* Switch off the InnoDB Monitor; this is a simple way
		to stop the monitor if the situation becomes less urgent,
		but may also surprise users if the user also switched on the
		monitor! */

		buf_lru_switched_on_innodb_mon = FALSE;
		srv_print_innodb_monitor = FALSE;
	}

	/* If there is a block in the free list, take it */
	block = buf_LRU_get_free_only();

	if (block) {
		buf_pool_mutex_exit();
		buf_LRU_prepare_free_block(block, started_monitor, mon_value_was);
		return(block);
	}

	/* If no block was in the free list, search from the end of the LRU
	list and try to free a block there. This function calls buf_pool_mutex_exit */

	*nsearched = 0;
	freed = buf_LRU_search_and_free_block(n_iterations, &block, TRUE, nsearched);

	if (block) {
		ut_a(freed);
		buf_LRU_prepare_free_block(block, started_monitor, mon_value_was);
		return block;
	}

	if (freed > 0) {
		goto loop;
	}

	if (n_iterations > 30) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: Warning: difficult to find free blocks in\n"
			"InnoDB: the buffer pool (%lu search iterations)!"
			" Consider\n"
			"InnoDB: increasing the buffer pool size.\n"
			"InnoDB: It is also possible that"
			" in your Unix version\n"
			"InnoDB: fsync is very slow, or"
			" completely frozen inside\n"
			"InnoDB: the OS kernel. Then upgrading to"
			" a newer version\n"
			"InnoDB: of your operating system may help."
			" Look at the\n"
			"InnoDB: number of fsyncs in diagnostic info below.\n"
			"InnoDB: Pending flushes (fsync) log: %lu;"
			" buffer pool: %lu\n"
			"InnoDB: %lu OS file reads, %lu OS file writes,"
			" %lu OS fsyncs\n"
			"InnoDB: Starting InnoDB Monitor to print further\n"
			"InnoDB: diagnostics to the standard output.\n",
			(ulong) n_iterations,
			(ulong) fil_n_pending_log_flushes,
			(ulong) fil_n_pending_tablespace_flushes,
			(ulong) os_n_file_reads, (ulong) os_n_file_writes,
			(ulong) os_n_fsyncs);

		mon_value_was = srv_print_innodb_monitor;
		started_monitor = TRUE;
		srv_print_innodb_monitor = TRUE;
		os_event_set(srv_lock_timeout_thread_event);
	}

	/* No free block was found: try to flush the LRU list */

	buf_flush_free_margin(TRUE, *nsearched);

	/* Caller won't need to do work in buf_flush_free_margin because
	it was just called above. */
	*nsearched = 0;

	++srv_buf_pool_wait_free;

	os_aio_simulated_wake_handler_threads();

	buf_pool_mutex_enter();

	if (buf_pool->LRU_flush_ended > 0) {
		/* We have written pages in an LRU flush. To make the insert
		buffer more efficient, we try to move these pages to the free
		list. */

		buf_pool_mutex_exit();

		buf_LRU_try_free_flushed_blocks();
	} else {
		buf_pool_mutex_exit();
	}

	if (n_iterations > 10) {

		os_thread_sleep(500000);
	}

	n_iterations++;

	goto loop;
}

/*******************************************************************//**
Moves the LRU_old pointer so that the length of the old blocks list
is inside the allowed limits. */
UNIV_INLINE
void
buf_LRU_old_adjust_len(void)
/*========================*/
{
	ulint	old_len;
	ulint	new_len;

	ut_a(buf_pool->LRU_old);
	ut_ad(buf_pool_mutex_own());
	ut_ad(buf_LRU_old_ratio >= BUF_LRU_OLD_RATIO_MIN);
	ut_ad(buf_LRU_old_ratio <= BUF_LRU_OLD_RATIO_MAX);
#if BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN <= BUF_LRU_OLD_RATIO_DIV * (BUF_LRU_OLD_TOLERANCE + 5)
# error "BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN <= BUF_LRU_OLD_RATIO_DIV * (BUF_LRU_OLD_TOLERANCE + 5)"
#endif
#ifdef UNIV_LRU_DEBUG
	/* buf_pool->LRU_old must be the first item in the LRU list
	whose "old" flag is set. */
	ut_a(buf_pool->LRU_old->old);
	ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)
	     || !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
	ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)
	     || UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */

	old_len = buf_pool->LRU_old_len;
	new_len = ut_min(UT_LIST_GET_LEN(buf_pool->LRU)
			 * buf_LRU_old_ratio / BUF_LRU_OLD_RATIO_DIV,
			 UT_LIST_GET_LEN(buf_pool->LRU)
			 - (BUF_LRU_OLD_TOLERANCE
			    + BUF_LRU_NON_OLD_MIN_LEN));

	for (;;) {
		buf_page_t*	LRU_old = buf_pool->LRU_old;

		ut_a(LRU_old);
		ut_ad(LRU_old->in_LRU_list);
#ifdef UNIV_LRU_DEBUG
		ut_a(LRU_old->old);
#endif /* UNIV_LRU_DEBUG */

		/* Update the LRU_old pointer if necessary */

		if (old_len + BUF_LRU_OLD_TOLERANCE < new_len) {

			buf_pool->LRU_old = LRU_old = UT_LIST_GET_PREV(
				LRU, LRU_old);
#ifdef UNIV_LRU_DEBUG
			ut_a(!LRU_old->old);
#endif /* UNIV_LRU_DEBUG */
			old_len = ++buf_pool->LRU_old_len;
			buf_page_set_old(LRU_old, TRUE);

		} else if (old_len > new_len + BUF_LRU_OLD_TOLERANCE) {

			buf_pool->LRU_old = UT_LIST_GET_NEXT(LRU, LRU_old);
			old_len = --buf_pool->LRU_old_len;
			buf_page_set_old(LRU_old, FALSE);
		} else {
			return;
		}
	}
}

/*******************************************************************//**
Initializes the old blocks pointer in the LRU list. This function should be
called when the LRU list grows to BUF_LRU_OLD_MIN_LEN length. */
static
void
buf_LRU_old_init(void)
/*==================*/
{
	buf_page_t*	bpage;

	ut_ad(buf_pool_mutex_own());
	ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN);

	/* We first initialize all blocks in the LRU list as old and then use
	the adjust function to move the LRU_old pointer to the right
	position */

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU); bpage != NULL;
	     bpage = UT_LIST_GET_PREV(LRU, bpage)) {
		ut_ad(bpage->in_LRU_list);
		ut_ad(buf_page_in_file(bpage));
		/* This loop temporarily violates the
		assertions of buf_page_set_old(). */
		bpage->old = TRUE;
	}

	buf_pool->LRU_old = UT_LIST_GET_FIRST(buf_pool->LRU);
	buf_pool->LRU_old_len = UT_LIST_GET_LEN(buf_pool->LRU);

	buf_LRU_old_adjust_len();
}

/******************************************************************//**
Remove a block from the unzip_LRU list if it belonged to the list. */
static
void
buf_unzip_LRU_remove_block_if_needed(
/*=================================*/
	buf_page_t*	bpage)	/*!< in/out: control block */
{
	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_page_in_file(bpage));
	ut_ad(buf_pool_mutex_own());

	if (buf_page_belongs_to_unzip_LRU(bpage)) {
		buf_block_t*	block = (buf_block_t*) bpage;

		ut_ad(block->in_unzip_LRU_list);
		ut_d(block->in_unzip_LRU_list = FALSE);

		UT_LIST_REMOVE(unzip_LRU, buf_pool->unzip_LRU, block);
	}
}

/******************************************************************//**
Removes a block from the LRU list. */
UNIV_INLINE
void
buf_LRU_remove_block(
/*=================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_in_file(bpage));

	ut_ad(bpage->in_LRU_list);

	/* If the LRU_old pointer is defined and points to just this block,
	move it backward one step */

	if (UNIV_UNLIKELY(bpage == buf_pool->LRU_old)) {

		/* Below: the previous block is guaranteed to exist,
		because the LRU_old pointer is only allowed to differ
		by BUF_LRU_OLD_TOLERANCE from strict
		buf_LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV of the LRU
		list length. */
		buf_page_t*	prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		ut_a(prev_bpage);
#ifdef UNIV_LRU_DEBUG
		ut_a(!prev_bpage->old);
#endif /* UNIV_LRU_DEBUG */
		buf_pool->LRU_old = prev_bpage;
		buf_page_set_old(prev_bpage, TRUE);

		buf_pool->LRU_old_len++;
	}

	/* Remove the block from the LRU list */
	UT_LIST_REMOVE(LRU, buf_pool->LRU, bpage);
	ut_d(bpage->in_LRU_list = FALSE);

	buf_unzip_LRU_remove_block_if_needed(bpage);

	/* If the LRU list is so short that LRU_old is not defined,
	clear the "old" flags and return */
	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN) {

		for (bpage = UT_LIST_GET_FIRST(buf_pool->LRU); bpage != NULL;
		     bpage = UT_LIST_GET_NEXT(LRU, bpage)) {
			/* This loop temporarily violates the
			assertions of buf_page_set_old(). */
			bpage->old = FALSE;
		}

		buf_pool->LRU_old = NULL;
		buf_pool->LRU_old_len = 0;

		return;
	}

	ut_ad(buf_pool->LRU_old);

	/* Update the LRU_old_len field if necessary */
	if (buf_page_is_old(bpage)) {

		buf_pool->LRU_old_len--;
	}

	/* Adjust the length of the old block list if necessary */
	buf_LRU_old_adjust_len();
}

/******************************************************************//**
Adds a block to the LRU list of decompressed zip pages. */
UNIV_INTERN
void
buf_unzip_LRU_add_block(
/*====================*/
	buf_block_t*	block,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the end
				of the list, else put to the start */
{
	ut_ad(buf_pool);
	ut_ad(block);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_belongs_to_unzip_LRU(&block->page));

	ut_ad(!block->in_unzip_LRU_list);
	ut_d(block->in_unzip_LRU_list = TRUE);

	if (old) {
		UT_LIST_ADD_LAST(unzip_LRU, buf_pool->unzip_LRU, block);
	} else {
		UT_LIST_ADD_FIRST(unzip_LRU, buf_pool->unzip_LRU, block);
	}
}

/******************************************************************//**
Adds a block to the LRU list end. */
UNIV_INLINE
void
buf_LRU_add_block_to_end_low(
/*=========================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_in_file(bpage));

	ut_ad(!bpage->in_LRU_list);
	UT_LIST_ADD_LAST(LRU, buf_pool->LRU, bpage);
	ut_d(bpage->in_LRU_list = TRUE);

	if (UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool->LRU_old);

		/* Adjust the length of the old block list if necessary */

		buf_page_set_old(bpage, TRUE);
		buf_pool->LRU_old_len++;
		buf_LRU_old_adjust_len();

	} else if (UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init();
	} else {
		buf_page_set_old(bpage, buf_pool->LRU_old != NULL);
	}

	/* If this is a zipped block with decompressed frame as well
	then put it on the unzip_LRU list */
	if (buf_page_belongs_to_unzip_LRU(bpage)) {
		buf_unzip_LRU_add_block((buf_block_t*) bpage, TRUE);
	}
}

/******************************************************************//**
Adds a block to the LRU list. */
UNIV_INLINE
void
buf_LRU_add_block_low(
/*==================*/
	buf_page_t*	bpage,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the old blocks
				in the LRU list, else put to the start; if the
				LRU list is very short, the block is added to
				the start, regardless of this parameter */
{
	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());

	ut_a(buf_page_in_file(bpage));
	ut_ad(!bpage->in_LRU_list);

	if (!old || (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN)) {

		UT_LIST_ADD_FIRST(LRU, buf_pool->LRU, bpage);

		bpage->freed_page_clock = buf_pool->freed_page_clock;
	} else {
#ifdef UNIV_LRU_DEBUG
		/* buf_pool->LRU_old must be the first item in the LRU list
		whose "old" flag is set. */
		ut_a(buf_pool->LRU_old->old);
		ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)
		     || !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
		ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)
		     || UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */
		UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU, buf_pool->LRU_old,
				     bpage);
		buf_pool->LRU_old_len++;
	}

	ut_d(bpage->in_LRU_list = TRUE);

	if (UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool->LRU_old);

		/* Adjust the length of the old block list if necessary */

		buf_page_set_old(bpage, old);
		buf_LRU_old_adjust_len();

	} else if (UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init();
	} else {
		buf_page_set_old(bpage, buf_pool->LRU_old != NULL);
	}

	/* If this is a zipped block with decompressed frame as well
	then put it on the unzip_LRU list */
	if (buf_page_belongs_to_unzip_LRU(bpage)) {
		buf_unzip_LRU_add_block((buf_block_t*) bpage, old);
	}
}

/******************************************************************//**
Adds a block to the LRU list. */
UNIV_INTERN
void
buf_LRU_add_block(
/*==============*/
	buf_page_t*	bpage,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the old
				blocks in the LRU list, else put to the start;
				if the LRU list is very short, the block is
				added to the start, regardless of this
				parameter */
{
	buf_LRU_add_block_low(bpage, old);
}

/******************************************************************//**
Moves a block to the start of the LRU list. */
UNIV_INTERN
void
buf_LRU_make_block_young(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	ut_ad(buf_pool_mutex_own());

	if (bpage->old) {
		buf_pool->stat.n_pages_made_young++;
	}

	buf_LRU_remove_block(bpage);
	buf_LRU_add_block_low(bpage, FALSE);
}

/******************************************************************//**
Moves a block to the end of the LRU list. */
UNIV_INTERN
void
buf_LRU_make_block_old(
/*===================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	buf_LRU_remove_block(bpage);
	buf_LRU_add_block_to_end_low(bpage);
}

/******************************************************************//**
Try to free a block.  If bpage is a descriptor of a compressed-only
page, the descriptor object will be freed as well.

NOTE: If this function returns TRUE, it will temporarily
release buf_pool_mutex.  Furthermore, the page frame will no longer be
accessible via bpage.

The caller must hold buf_pool_mutex and buf_page_get_mutex(bpage) and
release these two mutexes after the call.  No other
buf_page_get_mutex() may be held when calling this function.
@return TRUE if freed, FALSE otherwise. */
UNIV_INTERN
ibool
buf_LRU_free_block(
/*===============*/
	buf_page_t*	bpage,	/*!< in: block to be freed */
	ibool		zip,	/*!< in: TRUE if should remove also the
				compressed page of an uncompressed page */
	ibool*		removed)/*!< out: return TRUE if removed from LRU */
{
	buf_page_t*	b = NULL;
	mutex_t*	block_mutex = buf_page_get_mutex(bpage);

	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(block_mutex));
	ut_ad(buf_page_in_file(bpage));
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_flush_list == !bpage->oldest_modification);
#if UNIV_WORD_SIZE == 4
	/* On 32-bit systems, there is no padding in buf_page_t.  On
	other systems, Valgrind could complain about uninitialized pad
	bytes. */
	UNIV_MEM_ASSERT_RW(bpage, sizeof *bpage);
#endif

	*removed = FALSE;

	if (!buf_page_can_relocate(bpage)) {

		/* Do not free buffer-fixed or I/O-fixed blocks. */
		return(FALSE);
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(bpage->space, bpage->offset) == 0);
#endif /* UNIV_IBUF_COUNT_DEBUG */

	if (zip || !bpage->zip.data) {
		/* This would completely free the block. */
		/* Do not completely free dirty blocks. */

		if (bpage->oldest_modification) {
			return(FALSE);
		}
	} else if (bpage->oldest_modification) {
		/* Do not completely free dirty blocks. */

		if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
			ut_ad(buf_page_get_state(bpage)
			      == BUF_BLOCK_ZIP_DIRTY);
			return(FALSE);
		}

		goto alloc;
	} else if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {
		/* Allocate the control block for the compressed page.
		If it cannot be allocated (without freeing a block
		from the LRU list), refuse to free bpage. */
alloc:
		b = buf_page_alloc_descriptor(TRUE);
		ut_a(b);
		memcpy(b, bpage, sizeof *b);
	}

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Putting space %lu page %lu to free list\n",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));
	}
#endif /* UNIV_DEBUG */

	*removed = TRUE;

	if (buf_LRU_block_remove_hashed_page(bpage, zip)
	    != BUF_BLOCK_ZIP_FREE) {
		ut_a(bpage->buf_fix_count == 0);

		if (b) {
			buf_page_t*	prev_b	= UT_LIST_GET_PREV(LRU, b);
			const ulint	fold	= buf_page_address_fold(
				bpage->space, bpage->offset);

			ut_a(!buf_page_hash_get(bpage->space, bpage->offset));

			b->state = b->oldest_modification
				? BUF_BLOCK_ZIP_DIRTY
				: BUF_BLOCK_ZIP_PAGE;
			UNIV_MEM_DESC(b->zip.data,
				      page_zip_get_size(&b->zip), b);

			/* The fields in_page_hash and in_LRU_list of
			the to-be-freed block descriptor should have
			been cleared in
			buf_LRU_block_remove_hashed_page(), which
			invokes buf_LRU_remove_block(). */
			ut_ad(!bpage->in_page_hash);
			ut_ad(!bpage->in_LRU_list);
			/* bpage->state was BUF_BLOCK_FILE_PAGE because
			b != NULL. The type cast below is thus valid. */
			ut_ad(!((buf_block_t*) bpage)->in_unzip_LRU_list);

			/* The fields of bpage were copied to b before
			buf_LRU_block_remove_hashed_page() was invoked. */
			ut_ad(!b->in_zip_hash);
			ut_ad(b->in_page_hash);
			ut_ad(b->in_LRU_list);

			HASH_INSERT(buf_page_t, hash,
				    buf_pool->page_hash, fold, b);

			*removed = FALSE;

			/* Insert b where bpage was in the LRU list. */
			if (UNIV_LIKELY(prev_b != NULL)) {
				ulint	lru_len;

				ut_ad(prev_b->in_LRU_list);
				ut_ad(buf_page_in_file(prev_b));
#if UNIV_WORD_SIZE == 4
				/* On 32-bit systems, there is no
				padding in buf_page_t.  On other
				systems, Valgrind could complain about
				uninitialized pad bytes. */
				UNIV_MEM_ASSERT_RW(prev_b, sizeof *prev_b);
#endif
				UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU,
						     prev_b, b);

				if (buf_page_is_old(b)) {
					buf_pool->LRU_old_len++;
					if (UNIV_UNLIKELY
					    (buf_pool->LRU_old
					     == UT_LIST_GET_NEXT(LRU, b))) {

						buf_pool->LRU_old = b;
					}
				}

				lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

				if (lru_len > BUF_LRU_OLD_MIN_LEN) {
					ut_ad(buf_pool->LRU_old);
					/* Adjust the length of the
					old block list if necessary */
					buf_LRU_old_adjust_len();
				} else if (lru_len == BUF_LRU_OLD_MIN_LEN) {
					/* The LRU list is now long
					enough for LRU_old to become
					defined: init it */
					buf_LRU_old_init();
				}
#ifdef UNIV_LRU_DEBUG
				/* Check that the "old" flag is consistent
				in the block and its neighbours. */
				buf_page_set_old(b, buf_page_is_old(b));
#endif /* UNIV_LRU_DEBUG */
			} else {
				ut_d(b->in_LRU_list = FALSE);
				buf_LRU_add_block_low(b, buf_page_is_old(b));
			}

			if (b->state == BUF_BLOCK_ZIP_PAGE) {
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
				buf_LRU_insert_zip_clean(b);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
			} else {
				/* Relocate on buf_pool->flush_list. */
				buf_flush_relocate_on_flush_list(bpage, b);
			}

			bpage->zip.data = NULL;
			page_zip_set_size(&bpage->zip, 0);

			/* Prevent buf_page_get_gen() from
			decompressing the block while we release
			buf_pool_mutex and block_mutex. */
			mutex_enter(&buf_pool_zip_mutex);
			buf_page_set_sticky(b);
			mutex_exit(&buf_pool_zip_mutex);
		}

		buf_pool_mutex_exit();
		mutex_exit(block_mutex);

		/* Remove possible adaptive hash index on the page.
		The page was declared uninitialized by
		buf_LRU_block_remove_hashed_page().  We need to flag
		the contents of the page valid (which it still is) in
		order to avoid bogus Valgrind warnings.*/

		UNIV_MEM_VALID(((buf_block_t*) bpage)->frame,
			       UNIV_PAGE_SIZE);
		btr_search_drop_page_hash_index((buf_block_t*) bpage);
		UNIV_MEM_INVALID(((buf_block_t*) bpage)->frame,
				 UNIV_PAGE_SIZE);

		if (b && (srv_extra_checksums_unzip_lru ||
		          buf_page_get_state(b) == BUF_BLOCK_ZIP_DIRTY)) {
			/* Compute and stamp the compressed page
			checksum while not holding any mutex.  The
			block is already half-freed
			(BUF_BLOCK_REMOVE_HASH) and removed from
			buf_pool->page_hash, thus inaccessible by any
			other thread. */

			mach_write_to_4(
				b->zip.data + FIL_PAGE_SPACE_OR_CHKSUM,
				page_zip_calc_checksum(b->zip.data,
				                       page_zip_get_size(&b->zip)));
		}

		buf_pool_mutex_enter();
		mutex_enter(block_mutex);

		if (b) {
			mutex_enter(&buf_pool_zip_mutex);
			buf_page_unset_sticky(b);
			mutex_exit(&buf_pool_zip_mutex);
		}

		buf_LRU_block_free_hashed_page((buf_block_t*) bpage);
	} else {
		/* The block_mutex should have been released by
		buf_LRU_block_remove_hashed_page() when it returns
		BUF_BLOCK_ZIP_FREE. */
		ut_ad(block_mutex == &buf_pool_zip_mutex);
		mutex_enter(block_mutex);
	}

	return(TRUE);
}

/******************************************************************//**
Puts a block back to the free list. */
UNIV_INTERN
void
buf_LRU_block_free_non_file_page(
/*=============================*/
	buf_block_t*	block)	/*!< in: block, must not contain a file page */
{
	void*	data;

	ut_ad(block);
	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&block->mutex));

	switch (buf_block_get_state(block)) {
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_READY_FOR_USE:
		break;
	default:
		ut_error;
	}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ut_a(block->n_pointers == 0);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	ut_ad(!block->page.in_free_list);
	ut_ad(!block->page.in_flush_list);
	ut_ad(!block->page.in_LRU_list);

	buf_block_set_state(block, BUF_BLOCK_NOT_USED);

	UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);
#ifdef UNIV_DEBUG
	/* Wipe contents of page to reveal possible stale pointers to it */
	memset(block->frame, '\0', UNIV_PAGE_SIZE);
#else
	/* Wipe page_no and space_id */
	memset(block->frame + FIL_PAGE_OFFSET, 0xfe, 4);
	memset(block->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xfe, 4);
#endif
	data = block->page.zip.data;

	if (data) {
		block->page.zip.data = NULL;
		mutex_exit(&block->mutex);
		buf_pool_mutex_exit_forbid();
		buf_buddy_free(data, page_zip_get_size(&block->page.zip));
		buf_pool_mutex_exit_allow();
		mutex_enter(&block->mutex);
		page_zip_set_size(&block->page.zip, 0);
	}

	UT_LIST_ADD_FIRST(list, buf_pool->free, (&block->page));
	ut_d(block->page.in_free_list = TRUE);

	UNIV_MEM_ASSERT_AND_FREE(block->frame, UNIV_PAGE_SIZE);
}

/******************************************************************//**
Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed and buf_pool_zip_mutex will be released.

If a compressed page or a compressed-only block descriptor is freed,
other compressed pages or compressed-only block descriptors may be
relocated.
@return the new state of the block (BUF_BLOCK_ZIP_FREE if the state
was BUF_BLOCK_ZIP_PAGE, or BUF_BLOCK_REMOVE_HASH otherwise) */
static
enum buf_page_state
buf_LRU_block_remove_hashed_page(
/*=============================*/
	buf_page_t*	bpage,	/*!< in: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
	ibool		zip)	/*!< in: TRUE if should remove also the
				compressed page of an uncompressed page */
{
	const buf_page_t*	hashed_bpage;
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);

#if UNIV_WORD_SIZE == 4
	/* On 32-bit systems, there is no padding in
	buf_page_t.  On other systems, Valgrind could complain
	about uninitialized pad bytes. */
	UNIV_MEM_ASSERT_RW(bpage, sizeof *bpage);
#endif

	buf_LRU_remove_block(bpage);

	buf_pool->freed_page_clock += 1;

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_FILE_PAGE:
		UNIV_MEM_ASSERT_W(bpage, sizeof(buf_block_t));
		UNIV_MEM_ASSERT_W(((buf_block_t*) bpage)->frame,
				  UNIV_PAGE_SIZE);
		buf_block_modify_clock_inc((buf_block_t*) bpage);
		if (bpage->zip.data) {
			const page_t*	page = ((buf_block_t*) bpage)->frame;
			const ulint	zip_size
				= page_zip_get_size(&bpage->zip);

			ut_a(!zip || bpage->oldest_modification == 0);

			switch (UNIV_EXPECT(fil_page_get_type(page),
					    FIL_PAGE_INDEX)) {
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_BITMAP:
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
				/* These are essentially uncompressed pages. */
				if (!zip) {
					/* InnoDB writes the data to the
					uncompressed page frame.  Copy it
					to the compressed page, which will
					be preserved. */
					memcpy(bpage->zip.data, page,
					       zip_size);
				}
				break;
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_INDEX:
#ifdef UNIV_ZIP_DEBUG
				ut_a(page_zip_validate(&bpage->zip, page));
#endif /* UNIV_ZIP_DEBUG */
				break;
			default:
				ut_print_timestamp(stderr);
				fputs("  InnoDB: ERROR: The compressed page"
				      " to be evicted seems corrupt:", stderr);
				ut_print_buf(stderr, page, zip_size);
				fputs("\nInnoDB: Possibly older version"
				      " of the page:", stderr);
				ut_print_buf(stderr, bpage->zip.data,
					     zip_size);
				putc('\n', stderr);
				ut_error;
			}

			break;
		}
		/* fall through */
	case BUF_BLOCK_ZIP_PAGE:
		ut_a(bpage->oldest_modification == 0);
		UNIV_MEM_ASSERT_W(bpage->zip.data,
				  page_zip_get_size(&bpage->zip));
		break;
	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	}

	hashed_bpage = buf_page_hash_get(bpage->space, bpage->offset);

	if (UNIV_UNLIKELY(bpage != hashed_bpage)) {
		fprintf(stderr,
			"InnoDB: Error: page %lu %lu not found"
			" in the hash table\n",
			(ulong) bpage->space,
			(ulong) bpage->offset);
		if (hashed_bpage) {
			fprintf(stderr,
				"InnoDB: In hash table we find block"
				" %p of %lu %lu which is not %p\n",
				(const void*) hashed_bpage,
				(ulong) hashed_bpage->space,
				(ulong) hashed_bpage->offset,
				(const void*) bpage);
		}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		mutex_exit(buf_page_get_mutex(bpage));
		buf_pool_mutex_exit();
		buf_print();
		buf_LRU_print();
		buf_validate();
		buf_LRU_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		ut_error;
	}

	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_d(bpage->in_page_hash = FALSE);
	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash,
		    buf_page_address_fold(bpage->space, bpage->offset),
		    bpage);
	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_PAGE:
		ut_ad(!bpage->in_free_list);
		ut_ad(!bpage->in_flush_list);
		ut_ad(!bpage->in_LRU_list);
		ut_a(bpage->zip.data);
		ut_a(buf_page_get_zip_size(bpage));

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		UT_LIST_REMOVE(list, buf_pool->zip_clean, bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

		mutex_exit(&buf_pool_zip_mutex);
		buf_pool_mutex_exit_forbid();
		buf_buddy_free(bpage->zip.data,
			       page_zip_get_size(&bpage->zip));
		bpage->state = BUF_BLOCK_ZIP_FREE;
		buf_page_free_descriptor(bpage, TRUE);
		buf_pool_mutex_exit_allow();
		return(BUF_BLOCK_ZIP_FREE);

	case BUF_BLOCK_FILE_PAGE:
		memset(((buf_block_t*) bpage)->frame
		       + FIL_PAGE_OFFSET, 0xff, 4);
		memset(((buf_block_t*) bpage)->frame
		       + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		UNIV_MEM_INVALID(((buf_block_t*) bpage)->frame,
				 UNIV_PAGE_SIZE);
		buf_page_set_state(bpage, BUF_BLOCK_REMOVE_HASH);

		if (zip && bpage->zip.data) {
			/* Free the compressed page. */
			void*	data = bpage->zip.data;
			bpage->zip.data = NULL;

			ut_ad(!bpage->in_free_list);
			ut_ad(!bpage->in_flush_list);
			ut_ad(!bpage->in_LRU_list);
			mutex_exit(&((buf_block_t*) bpage)->mutex);
			buf_pool_mutex_exit_forbid();
			buf_buddy_free(data, page_zip_get_size(&bpage->zip));
			buf_pool_mutex_exit_allow();
			mutex_enter(&((buf_block_t*) bpage)->mutex);
			page_zip_set_size(&bpage->zip, 0);
		}

		return(BUF_BLOCK_REMOVE_HASH);

	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		break;
	}

	ut_error;
	return(BUF_BLOCK_ZIP_FREE);
}

/******************************************************************//**
Puts a file page whose has no hash index to the free list. */
static
void
buf_LRU_block_free_hashed_page(
/*===========================*/
	buf_block_t*	block)	/*!< in: block, must contain a file page and
				be in a state where it can be freed */
{
	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(&block->mutex));

	buf_block_set_state(block, BUF_BLOCK_MEMORY);

	buf_LRU_block_free_non_file_page(block);
}

/******************************************************************//**
Remove one page from LRU list and put it to free list */
UNIV_INTERN
void
buf_LRU_free_one_page(
/*==================*/
	buf_page_t*	bpage)	/*!< in/out: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
{
	mutex_t*	block_mutex = buf_page_get_mutex(bpage);

	ut_ad(buf_pool_mutex_own());
	ut_ad(mutex_own(block_mutex));

	if (buf_LRU_block_remove_hashed_page(bpage, TRUE)
	    != BUF_BLOCK_ZIP_FREE) {
		buf_LRU_block_free_hashed_page((buf_block_t*) bpage);
	} else {
		/* The block_mutex should have been released by
		buf_LRU_block_remove_hashed_page() when it returns
		BUF_BLOCK_ZIP_FREE. */
		ut_ad(block_mutex == &buf_pool_zip_mutex);
		mutex_enter(block_mutex);
	}
}

/**********************************************************************//**
Updates buf_LRU_old_ratio.
@return	updated old_pct */
UNIV_INTERN
uint
buf_LRU_old_ratio_update(
/*=====================*/
	uint	old_pct,/*!< in: Reserve this percentage of
			the buffer pool for "old" blocks. */
	ibool	adjust)	/*!< in: TRUE=adjust the LRU list;
			FALSE=just assign buf_LRU_old_ratio
			during the initialization of InnoDB */
{
	uint	ratio;

	ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100;
	if (ratio < BUF_LRU_OLD_RATIO_MIN) {
		ratio = BUF_LRU_OLD_RATIO_MIN;
	} else if (ratio > BUF_LRU_OLD_RATIO_MAX) {
		ratio = BUF_LRU_OLD_RATIO_MAX;
	}

	if (adjust) {
		buf_pool_mutex_enter();

		if (ratio != buf_LRU_old_ratio) {
			buf_LRU_old_ratio = ratio;

			if (UT_LIST_GET_LEN(buf_pool->LRU)
			    >= BUF_LRU_OLD_MIN_LEN) {
				buf_LRU_old_adjust_len();
			}
		}

		buf_pool_mutex_exit();
	} else {
		buf_LRU_old_ratio = ratio;
	}

	/* the reverse of 
	ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100 */
	return((uint) (ratio * 100 / (double) BUF_LRU_OLD_RATIO_DIV + 0.5));
}

/********************************************************************//**
Update the historical stats that we are collecting for LRU eviction
policy at the end of each interval. */
UNIV_INTERN
void
buf_LRU_stat_update(void)
/*=====================*/
{
	buf_LRU_stat_t*	item;
	buf_LRU_stat_t	cur_stat;

	/* If we haven't started eviction yet then don't update stats. */
	if (buf_pool->freed_page_clock == 0) {
		goto func_exit;
	}

	buf_pool_mutex_enter();

	/* Update the index. */
	item = &buf_LRU_stat_arr[buf_LRU_stat_arr_ind];
	buf_LRU_stat_arr_ind++;
	buf_LRU_stat_arr_ind %= BUF_LRU_STAT_N_INTERVAL;

	/* Add the current value and subtract the obsolete entry.
	Since buf_LRU_stat_cur is not protected by any mutex,
	it can be changing between adding to buf_LRU_stat_sum
	and copying to item. Assign it to local variables to make
	sure the same value assign to the buf_LRU_stat_sum
	and item */
	cur_stat = buf_LRU_stat_cur;

	buf_LRU_stat_sum.io += cur_stat.io - item->io;
	buf_LRU_stat_sum.unzip += cur_stat.unzip - item->unzip;

	/* Put current entry in the array. */
	memcpy(item, &cur_stat, sizeof *item);

	buf_pool_mutex_exit();

func_exit:
	/* Clear the current entry. */
	memset(&buf_LRU_stat_cur, 0, sizeof buf_LRU_stat_cur);
}

/********************************************************************//**
Dump the LRU page list to the specific file.

The format of the file is a list of (space id, page id) pairs, written in
big-endian format, followed by the pair (0xFFFFFFFF, 0xFFFFFFFF).  The order of
the pages is the order in which they appear in the LRU, from most recent access
to oldest access.  */
#define LRU_DUMP_FILE "ib_lru_dump"
#define LRU_DUMP_TEMP_FILE "ib_lru_dump.tmp"

UNIV_INTERN
ibool
buf_LRU_file_dump(void)
/*===================*/
{
	os_file_t	dump_file = -1;
	ibool		success;
	byte*		buffer_base = NULL;
	byte*		buffer = NULL;
	buf_page_t*	bpage;
	buf_page_t*	first_bpage;
	ulint		buffers;
	ulint		offset;
	ulint		pages_written;
	ulint		i;
	ulint		total_pages;

	for (i = 0; i < srv_n_data_files; i++) {
		if (strstr(srv_data_file_names[i], LRU_DUMP_FILE) != NULL) {
			fprintf(stderr,
				" InnoDB: The name '%s' seems to be used for"
				" innodb_data_file_path. Dumping LRU list is not"
				" done for safeness.\n", LRU_DUMP_FILE);
			goto end;
		}
	}

	buffer_base = ut_malloc(2 * UNIV_PAGE_SIZE);
	buffer = ut_align(buffer_base, UNIV_PAGE_SIZE);
	if (!buffer) {
		fprintf(stderr,
			" InnoDB: cannot allocate buffer.\n");
		goto end;
	}

	dump_file = os_file_create(LRU_DUMP_TEMP_FILE, OS_FILE_OVERWRITE,
				OS_FILE_NORMAL, OS_DATA_FILE, &success);
	if (!success) {
		os_file_get_last_error(TRUE);
		fprintf(stderr,
			" InnoDB: cannot open %s\n", LRU_DUMP_FILE);
		goto end;
	}

	memset(buffer, 0, UNIV_PAGE_SIZE);

	/* walk the buffer pool from most recent to oldest */
	buf_pool_mutex_enter();
	bpage = first_bpage = UT_LIST_GET_FIRST(buf_pool->LRU);
	total_pages = UT_LIST_GET_LEN(buf_pool->LRU);

	buffers = offset = pages_written = 0;
	while (bpage != NULL &&
		(srv_lru_dump_old_pages || !buf_page_is_old(bpage)) &&
		(pages_written++ < total_pages)) {

		buf_page_t*	next_bpage = UT_LIST_GET_NEXT(LRU, bpage);

		if (next_bpage == first_bpage) {
			buf_pool_mutex_exit();
			fprintf(stderr,
				" InnoDB: detected cycle in LRU, skipping dump\n");
			success = FALSE;
			goto end;
		}

		mach_write_to_4(buffer + offset * 4, bpage->space);
		offset++;
		mach_write_to_4(buffer + offset * 4, bpage->offset);
		offset++;

		/* write out one page of data at a time */
		if (offset == UNIV_PAGE_SIZE/4) {
			mutex_t* next_block_mutex = NULL;

			/* while writing file, release buffer pool mutex but
			 * keep the next page fixed so we don't worry about
			 * our list iterator becoming invalid */
			if (next_bpage) {
				next_block_mutex = buf_page_get_mutex(next_bpage);

				mutex_enter(next_block_mutex);
				next_bpage->buf_fix_count++;
				mutex_exit(next_block_mutex);
			}
			buf_pool_mutex_exit();

			success = os_file_write(LRU_DUMP_TEMP_FILE, dump_file, buffer,
					(buffers << UNIV_PAGE_SIZE_SHIFT) & 0xFFFFFFFFUL,
					(buffers >> (32 - UNIV_PAGE_SIZE_SHIFT)),
					UNIV_PAGE_SIZE);
			buffers++;
			offset = 0;
			memset(buffer, 0, UNIV_PAGE_SIZE);

			buf_pool_mutex_enter();
			if (next_bpage) {
				mutex_enter(next_block_mutex);
				next_bpage->buf_fix_count--;
				mutex_exit(next_block_mutex);
			}
			if (!success) {
				buf_pool_mutex_exit();
				fprintf(stderr,
					" InnoDB: cannot write page %lu of %s\n",
					buffers, LRU_DUMP_FILE);
				goto end;
			}
		}

		bpage = next_bpage;
	}
	buf_pool_mutex_exit();

	/* mark end of file with 0xFFFFFFFF */
	mach_write_to_4(buffer + offset * 4, 0xFFFFFFFFUL);
	offset++;
	mach_write_to_4(buffer + offset * 4, 0xFFFFFFFFUL);
	offset++;

	success = os_file_write(LRU_DUMP_TEMP_FILE, dump_file, buffer,
			(buffers << UNIV_PAGE_SIZE_SHIFT) & 0xFFFFFFFFUL,
			(buffers >> (32 - UNIV_PAGE_SIZE_SHIFT)),
			UNIV_PAGE_SIZE);
	if (!success) {
		goto end;
	}

end:
	if (dump_file != -1) {
		if (success) {
			success = os_file_flush(dump_file);
		}
		os_file_close(dump_file);
	}
	if (success) {
		success = os_file_rename(LRU_DUMP_TEMP_FILE,
					LRU_DUMP_FILE);
	}
	if (buffer_base) {
		ut_free(buffer_base);
	}

	return(success);
}

typedef struct {
	ib_uint32_t space_id;
	ib_uint32_t page_no;
} dump_record_t;

static int dump_record_cmp(const void *a, const void *b)
{
	const dump_record_t *rec1 = (dump_record_t *) a;
	const dump_record_t *rec2 = (dump_record_t *) b;

	if (rec1->space_id < rec2->space_id)
		return -1;
	if (rec1->space_id > rec2->space_id)
		return 1;
	if (rec1->page_no < rec2->page_no)
		return -1;
	return rec1->page_no > rec2->page_no;
}

/********************************************************************//**
Read the pages based on the specific file.

Pre-warms the buffer pool by loading the buffer pool pages recorded in
LRU_DUMP_FILE by automatic or manual invocation of buf_LRU_file_dump.

The pages are loaded in LRU priority order to ensure the most frequently
accessed pages are loaded first.  While loading in LRU priority order, any
lower priority pages that are logically adjacent to higher priority pages are
loaded along with the higher priority page.  The goal is to maximize the size
of the data reads without introducing many additional seeks.
*/
UNIV_INTERN
ibool
buf_LRU_file_restore(void)
/*======================*/
{
	os_file_t	dump_file = -1;
	ibool		success;
	byte*		buffer_base = NULL;
	byte*		buffer = NULL;
	ulint		buffers;
	ulint		offset;
	ulint		reads = 0;
	ulint		req = 0;
	ibool		terminated = FALSE;
	ibool		ret = FALSE;
	dump_record_t*	records = NULL;
	dump_record_t*	sorted_records = NULL;
	dump_record_t*	current_record;
	dump_record_t*	prev_record;
	dump_record_t*	next_record;
	unsigned char*	records_loaded = NULL;
	ulint		size;
	ulint		size_high;
	ulint		length;
	my_fast_timer_t	loop_timer;

	dump_file = os_file_create_simple_no_error_handling(
		LRU_DUMP_FILE, OS_FILE_OPEN, OS_FILE_READ_ONLY, &success);
	if (!success || !os_file_get_size(dump_file, &size, &size_high)) {
		os_file_get_last_error(TRUE);
		fprintf(stderr,
			" InnoDB: cannot open %s\n", LRU_DUMP_FILE);
		goto end;
	}
	if (size == 0 || size_high > 0 || size % 8) {
		fprintf(stderr, " InnoDB: broken LRU dump file\n");
		goto end;
	}
	buffer_base = ut_malloc(2 * UNIV_PAGE_SIZE);
	buffer = ut_align(buffer_base, UNIV_PAGE_SIZE);
	records = (dump_record_t*) ut_malloc((size/8) * sizeof(dump_record_t));
	sorted_records = (dump_record_t*) ut_malloc((size/8) * sizeof(dump_record_t));
	records_loaded = (unsigned char*) ut_malloc((size/8) * sizeof(unsigned char));
	if (!buffer || !records || !sorted_records || !records_loaded) {
		fprintf(stderr,
			" InnoDB: cannot allocate buffer.\n");
		goto end;
	}

	buffers = 0;
	length = 0;
	while (!terminated) {
		success = os_file_read(dump_file, buffer,
				(buffers << UNIV_PAGE_SIZE_SHIFT) & 0xFFFFFFFFUL,
				(buffers >> (32 - UNIV_PAGE_SIZE_SHIFT)),
				UNIV_PAGE_SIZE);
		if (!success) {
			fprintf(stderr,
				" InnoDB: cannot read page %lu of %s,"
				" or meet unexpected terminal.\n",
				buffers, LRU_DUMP_FILE);
			goto end;
		}

		for (offset = 0; offset < UNIV_PAGE_SIZE/4; offset += 2) {
			ulint	space_id;
			ulint	page_no;

			space_id = mach_read_from_4(buffer + offset * 4);
			page_no = mach_read_from_4(buffer + (offset + 1) * 4);
			/* found list terminator value 0xFFFFFFFF */
			if (space_id == 0xFFFFFFFFUL
			    || page_no == 0xFFFFFFFFUL) {
				terminated = TRUE;
				break;
			}

			records[length].space_id = space_id;
			records[length].page_no = page_no;
			length++;
			if (length * 8 >= size) {
				fprintf(stderr,
					" InnoDB: could not find the "
					"end-of-file marker after reading "
					"the expected %lu bytes from the "
					"LRU dump file.\n"
					" InnoDB: this could be caused by a "
					"broken or incomplete file.\n"
					" InnoDB: trying to process what has "
					"been read so far.\n",
					size);
				terminated= TRUE;
				break;
			}
		}

		buffers++;
	}

	srv_lru_restore_total_pages = length;
	srv_lru_restore_loaded_pages = 0;

	/* Copy the records into a second array and sort them, this will
	 * allow us to identify sequential records so we can load contiguous
	 * data while still prioritizing based on LRU order in the original */
	memcpy(sorted_records, records, length * sizeof(dump_record_t));
	qsort(sorted_records, length, sizeof(dump_record_t), dump_record_cmp);

	/* As we will be loading data in a new order, we use this array to
	 * track which records have already been loaded */
	memset(records_loaded, 0, length * sizeof(char));

	/* start time */
	my_get_fast_timer(&loop_timer);

	/* iterate over the LRU in priority order */
	for (offset = 0;
	     offset < ut_min(length, srv_lru_load_max_entries);
	     offset++) {
		ulint		space_id;
		ulint		page_no;
		ulint		zip_size;
		ulint		err;
		ib_int64_t	tablespace_version;

		space_id = records[offset].space_id;
		zip_size = fil_space_get_zip_size(space_id);
		if (UNIV_UNLIKELY(zip_size == ULINT_UNDEFINED)) {
			continue;
		}

		/* we iterate over the LRU in priority order, but want to find
		 * the record's position in the sorted array so we can look for
		 * consecutive runs */
		current_record = bsearch(records + offset, sorted_records, length,
					sizeof(dump_record_t), dump_record_cmp);
		ut_ad(current_record);

		/* check if we already loaded this record as part of another
		 * consecutive run */
		if (records_loaded[current_record - sorted_records]) {
			continue;
		}

		/* step backwards in the sorted array until we find the start
		 * of this run of consecutive pages */
		while (current_record > sorted_records) {
			prev_record = current_record - 1;

			if (prev_record->space_id != current_record->space_id ||
				prev_record->page_no + 1 != current_record->page_no) {
				break;
			}

			current_record = prev_record;
		}

		/* now step forwards requesting consecutive pages */
		while (current_record < sorted_records + length) {
			ulint	unused	= 0;

			if (srv_shutdown_state >= SRV_SHUTDOWN_CLEANUP) {
				os_aio_simulated_wake_handler_threads();
				goto end;
			}

			records_loaded[current_record - sorted_records] = TRUE;

			page_no = current_record->page_no;

			if (!fil_area_is_exist(space_id, zip_size, page_no, 0,
					      zip_size ? zip_size : UNIV_PAGE_SIZE)) {
				break;
			}

			tablespace_version = fil_space_get_version(space_id);

			req++;

			/* do not issue more than srv_io_capacity requests per second */
			if (req % srv_io_capacity == 0) {
				ulint loop_usecs;

				os_aio_simulated_wake_handler_threads();
				buf_flush_free_margin(FALSE, 0);

				loop_usecs = my_fast_timer_diff_now(&loop_timer, NULL) * 1000000.0;

				if (loop_usecs < 1000000) {
					os_thread_sleep(1000000 - loop_usecs);
				}

				my_get_fast_timer(&loop_timer);
			}

			reads += buf_read_page_low(&err, FALSE, BUF_READ_ANY_PAGE
						   | OS_AIO_SIMULATED_WAKE_LATER,
						   space_id, zip_size, TRUE,
						   tablespace_version, page_no, NULL,
						   &unused);
			buf_LRU_stat_inc_io();

			srv_lru_restore_loaded_pages++;

			next_record = current_record + 1;

			if (next_record >= sorted_records + length ||
				current_record->space_id != next_record->space_id ||
				current_record->page_no + 1 != next_record->page_no) {
				break;
			}

			current_record = next_record;
		}
	}

	os_aio_simulated_wake_handler_threads();
	buf_flush_free_margin(FALSE, 0);

	ut_print_timestamp(stderr);
	fprintf(stderr,
		" InnoDB: reading pages based on the dumped LRU list was done."
		" (requested: %lu, read: %lu)\n", req, reads);
	ret = TRUE;
end:
	if (dump_file != -1)
		os_file_close(dump_file);
	if (buffer_base)
		ut_free(buffer_base);
	if (records)
		ut_free(records);
	if (sorted_records)
		ut_free(sorted_records);
	if (records_loaded)
		ut_free(records_loaded);

	return(ret);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**********************************************************************//**
Validates the LRU list.
@return	TRUE */
UNIV_INTERN
ibool
buf_LRU_validate(void)
/*==================*/
{
	buf_page_t*	bpage;
	buf_block_t*	block;
	ulint		old_len;
	ulint		new_len;

	ut_ad(buf_pool);
	buf_pool_mutex_enter();

	if (UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN) {

		ut_a(buf_pool->LRU_old);
		old_len = buf_pool->LRU_old_len;
		new_len = ut_min(UT_LIST_GET_LEN(buf_pool->LRU)
				 * buf_LRU_old_ratio / BUF_LRU_OLD_RATIO_DIV,
				 UT_LIST_GET_LEN(buf_pool->LRU)
				 - (BUF_LRU_OLD_TOLERANCE
				    + BUF_LRU_NON_OLD_MIN_LEN));
		ut_a(old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
		ut_a(old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
	}

	UT_LIST_VALIDATE(LRU, buf_page_t, buf_pool->LRU,
			 ut_ad(ut_list_node_313->in_LRU_list));

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	old_len = 0;

	while (bpage != NULL) {

		switch (buf_page_get_state(bpage)) {
		case BUF_BLOCK_ZIP_FREE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		case BUF_BLOCK_FILE_PAGE:
			ut_ad(((buf_block_t*) bpage)->in_unzip_LRU_list
			      == buf_page_belongs_to_unzip_LRU(bpage));
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_ZIP_DIRTY:
			break;
		}

		if (buf_page_is_old(bpage)) {
			const buf_page_t*	prev
				= UT_LIST_GET_PREV(LRU, bpage);
			const buf_page_t*	next
				= UT_LIST_GET_NEXT(LRU, bpage);

			if (!old_len++) {
				ut_a(buf_pool->LRU_old == bpage);
			} else {
				ut_a(!prev || buf_page_is_old(prev));
			}

			ut_a(!next || buf_page_is_old(next));
		}

		bpage = UT_LIST_GET_NEXT(LRU, bpage);
	}

	ut_a(buf_pool->LRU_old_len == old_len);

	UT_LIST_VALIDATE(list, buf_page_t, buf_pool->free,
			 ut_ad(ut_list_node_313->in_free_list));

	for (bpage = UT_LIST_GET_FIRST(buf_pool->free);
	     bpage != NULL;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_NOT_USED);
	}

	UT_LIST_VALIDATE(unzip_LRU, buf_block_t, buf_pool->unzip_LRU,
			 ut_ad(ut_list_node_313->in_unzip_LRU_list
			       && ut_list_node_313->page.in_LRU_list));

	for (block = UT_LIST_GET_FIRST(buf_pool->unzip_LRU);
	     block;
	     block = UT_LIST_GET_NEXT(unzip_LRU, block)) {

		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);
		ut_a(buf_page_belongs_to_unzip_LRU(&block->page));
	}

	buf_pool_mutex_exit();
	return(TRUE);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**********************************************************************//**
Prints the LRU list. */
UNIV_INTERN
void
buf_LRU_print(void)
/*===============*/
{
	const buf_page_t*	bpage;

	ut_ad(buf_pool);
	buf_pool_mutex_enter();

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	while (bpage != NULL) {

		fprintf(stderr, "BLOCK space %lu page %lu ",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));

		if (buf_page_is_old(bpage)) {
			fputs("old ", stderr);
		}

		if (bpage->buf_fix_count) {
			fprintf(stderr, "buffix count %lu ",
				(ulong) bpage->buf_fix_count);
		}

		if (buf_page_get_io_fix(bpage)) {
			fprintf(stderr, "io_fix %lu ",
				(ulong) buf_page_get_io_fix(bpage));
		}

		if (bpage->oldest_modification) {
			fputs("modif. ", stderr);
		}

		switch (buf_page_get_state(bpage)) {
			const byte*	frame;
		case BUF_BLOCK_FILE_PAGE:
			frame = buf_block_get_frame((buf_block_t*) bpage);
			fprintf(stderr, "\ntype %lu"
				" index id %lu\n",
				(ulong) fil_page_get_type(frame),
				(ulong) ut_dulint_get_low(
					btr_page_get_index_id(frame)));
			break;
		case BUF_BLOCK_ZIP_PAGE:
			frame = bpage->zip.data;
			fprintf(stderr, "\ntype %lu size %lu"
				" index id %lu\n",
				(ulong) fil_page_get_type(frame),
				(ulong) buf_page_get_zip_size(bpage),
				(ulong) ut_dulint_get_low(
					btr_page_get_index_id(frame)));
			break;

		default:
			fprintf(stderr, "\n!state %lu!\n",
				(ulong) buf_page_get_state(bpage));
			break;
		}

		bpage = UT_LIST_GET_NEXT(LRU, bpage);
	}

	buf_pool_mutex_exit();
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */
