/*
 *******************************************************************************
 *
 *	STORAGE INTERACTION
 *
 * This module is responsible for interaction with the storage of AQO data.
 * It does not provide information protection from concurrent updates.
 *
 *******************************************************************************
 *
 * Copyright (c) 2016-2022, Postgres Professional
 *
 * IDENTIFICATION
 *	  aqo/storage.c
 *
 */

#include "postgres.h"

#include <unistd.h>

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "miscadmin.h"

#include "aqo.h"
#include "aqo_shared.h"
#include "machine_learning.h"
#include "preprocessing.h"
#include "learn_cache.h"
#include "storage.h"

#define AQO_DATA_COLUMNS	(7)
HTAB *deactivated_queries = NULL;

static ArrayType *form_matrix(double *matrix, int nrows, int ncols);

#define FormVectorSz(v_name)			(form_vector((v_name), (v_name ## _size)))

static bool my_simple_heap_update(Relation relation,
								  ItemPointer otid,
								  HeapTuple tup,
								  bool *update_indexes);

/*
 * Open an AQO-related relation.
 * It should be done carefully because of a possible concurrent DROP EXTENSION
 * command. In such case AQO must be disabled in this backend.
 */
static bool
open_aqo_relation(char *heaprelnspname, char *heaprelname,
				  char *indrelname, LOCKMODE lockmode,
				  Relation *hrel, Relation *irel)
{
	Oid			reloid;
	RangeVar   *rv;

	reloid = RelnameGetRelid(indrelname);
	if (!OidIsValid(reloid))
		goto cleanup;

	rv = makeRangeVar(heaprelnspname, heaprelname, -1);
	*hrel = table_openrv_extended(rv,  lockmode, true);
	if (*hrel == NULL)
		goto cleanup;

	/* Try to open index relation carefully. */
	*irel = try_relation_open(reloid,  lockmode);
	if (*irel == NULL)
	{
		relation_close(*hrel, lockmode);
		goto cleanup;
	}

	return true;

cleanup:
	/*
	 * Absence of any AQO-related table tell us that someone executed
	 * a 'DROP EXTENSION aqo' command. We disable AQO for all future queries
	 * in this backend. For performance reasons we do it locally.
	 * Clear profiling hash table.
	 * Also, we gently disable AQO for the rest of the current query
	 * execution process.
	 */
	aqo_enabled = false;
	disable_aqo_for_query();
	return false;

}

/*
 * Returns whether the query with given hash is in aqo_queries.
 * If yes, returns the content of the first line with given hash.
 *
 * Use dirty snapshot to see all (include in-progess) data. We want to prevent
 * wait in the XactLockTableWait routine.
 * If query is found in the knowledge base, fill the query context struct.
 */
bool
find_query(uint64 qhash, QueryContextData *ctx)
{
	Relation		hrel;
	Relation		irel;
	HeapTuple		tuple;
	TupleTableSlot *slot;
	bool			shouldFree = true;
	IndexScanDesc	scan;
	ScanKeyData		key;
	SnapshotData	snap;
	bool			find_ok = false;
	Datum			values[5];
	bool			nulls[5] = {false, false, false, false, false};

	if (!open_aqo_relation(NULL, "aqo_queries", "aqo_queries_query_hash_idx",
		AccessShareLock, &hrel, &irel))
		return false;

	InitDirtySnapshot(snap);
	scan = index_beginscan(hrel, irel, &snap, 1, 0);
	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(qhash));

	index_rescan(scan, &key, 1, NULL, 0);
	slot = MakeSingleTupleTableSlot(hrel->rd_att, &TTSOpsBufferHeapTuple);
	find_ok = index_getnext_slot(scan, ForwardScanDirection, slot);

	if (find_ok)
	{
		tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
		Assert(shouldFree != true);
		heap_deform_tuple(tuple, hrel->rd_att, values, nulls);

		/* Fill query context data */
		ctx->learn_aqo = DatumGetBool(values[1]);
		ctx->use_aqo = DatumGetBool(values[2]);
		ctx->fspace_hash = DatumGetInt64(values[3]);
		ctx->auto_tuning = DatumGetBool(values[4]);
		ctx->collect_stat = query_context.auto_tuning;
	}

	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(irel,  AccessShareLock);
	table_close(hrel,  AccessShareLock);
	return find_ok;
}

/*
 * Update query status in intelligent mode.
 *
 * Do it gently: to prevent possible deadlocks, revert this update if any
 * concurrent transaction is doing it.
 *
 * Such logic is possible, because this update is performed by AQO itself. It is
 * not break any learning logic besides possible additional learning iterations.
 * Pass NIL as a value of the relations field to avoid updating it.
 */
bool
update_query(uint64 qhash, uint64 fhash,
			 bool learn_aqo, bool use_aqo, bool auto_tuning)
{
	Relation	hrel;
	Relation	irel;
	TupleTableSlot *slot;
	HeapTuple	tuple,
				nw_tuple;
	Datum		values[5];
	bool		isnull[5] = { false, false, false, false, false };
	bool		replace[5] = { false, true, true, true, true };
	bool		shouldFree;
	bool		result = true;
	bool		update_indexes;
	IndexScanDesc scan;
	ScanKeyData key;
	SnapshotData snap;

	/* Couldn't allow to write if xact must be read-only. */
	if (XactReadOnly)
		return false;

	if (!open_aqo_relation(NULL, "aqo_queries", "aqo_queries_query_hash_idx",
		RowExclusiveLock, &hrel, &irel))
		return false;

	/*
	 * Start an index scan. Use dirty snapshot to check concurrent updates that
	 * can be made before, but still not visible.
	 */
	InitDirtySnapshot(snap);
	scan = index_beginscan(hrel, irel, &snap, 1, 0);
	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(qhash));

	index_rescan(scan, &key, 1, NULL, 0);
	slot = MakeSingleTupleTableSlot(hrel->rd_att, &TTSOpsBufferHeapTuple);

	values[0] = Int64GetDatum(qhash);
	values[1] = BoolGetDatum(learn_aqo);
	values[2] = BoolGetDatum(use_aqo);
	values[3] = Int64GetDatum(fhash);
	values[4] = BoolGetDatum(auto_tuning);

	if (!index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		/* New tuple for the ML knowledge base */
		tuple = heap_form_tuple(RelationGetDescr(hrel), values, isnull);
		simple_heap_insert(hrel, tuple);
		my_index_insert(irel, values, isnull, &(tuple->t_self),
														hrel, UNIQUE_CHECK_YES);
	}
	else if (!TransactionIdIsValid(snap.xmin) &&
			 !TransactionIdIsValid(snap.xmax))
	{
		/*
		 * Update existed data. No one concurrent transaction doesn't update this
		 * right now.
		 */
		tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
		Assert(shouldFree != true);
		nw_tuple = heap_modify_tuple(tuple, hrel->rd_att, values, isnull, replace);

		if (my_simple_heap_update(hrel, &(nw_tuple->t_self), nw_tuple,
															&update_indexes))
		{
			if (update_indexes)
				my_index_insert(irel, values, isnull,
								&(nw_tuple->t_self),
								hrel, UNIQUE_CHECK_YES);
			result = true;
		}
		else
		{
			/*
			 * Ooops, somebody concurrently updated the tuple. It is possible
			 * only in the case of changes made by third-party code.
			 */
			elog(ERROR, "AQO feature space data for signature ("UINT64_FORMAT \
						", "UINT64_FORMAT") concurrently"
						" updated by a stranger backend.",
						qhash, fhash);
			result = false;
		}
	}
	else
	{
		/*
		 * Concurrent update was made. To prevent deadlocks refuse to update.
		 */
		result = false;
	}

	ExecDropSingleTupleTableSlot(slot);
	index_endscan(scan);
	index_close(irel, RowExclusiveLock);
	table_close(hrel, RowExclusiveLock);

	CommandCounterIncrement();
	return result;
}

/*
static ArrayType *
form_strings_vector(List *reloids)
{
	Datum	   *rels;
	ArrayType  *array;
	ListCell   *lc;
	int			i = 0;

	if (reloids == NIL)
		return NULL;

	rels = (Datum *) palloc(list_length(reloids) * sizeof(Datum));

	foreach(lc, reloids)
	{
		char *relname = strVal(lfirst(lc));

		rels[i++] = CStringGetTextDatum(relname);
	}

	array = construct_array(rels, i, TEXTOID, -1, false, TYPALIGN_INT);
	pfree(rels);
	return array;
}

static List *
deform_strings_vector(Datum datum)
{
	ArrayType  *array = DatumGetArrayTypePCopy(PG_DETOAST_DATUM(datum));
	Datum	   *values;
	int			i;
	int			nelems = 0;
	List	   *reloids = NIL;

	deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT,
					  &values, NULL, &nelems);
	for (i = 0; i < nelems; ++i)
	{
		Value *s;

		s = makeString(pstrdup(TextDatumGetCString(values[i])));
		reloids = lappend(reloids, s);
	}

	pfree(values);
	pfree(array);
	return reloids;
}
*/

bool
load_fss_ext(uint64 fs, int fss, OkNNrdata *data, List **reloids, bool isSafe)
{
	if (isSafe && (!aqo_learn_statement_timeout || !lc_has_fss(fs, fss)))
		return load_aqo_data(fs, fss, data, reloids, false);
	else
	{
		Assert(aqo_learn_statement_timeout);
		return lc_load_fss(fs, fss, data, reloids);
	}
}

bool
update_fss_ext(uint64 fs, int fss, OkNNrdata *data, List *reloids,
			   bool isTimedOut)
{
	if (!isTimedOut)
		return aqo_data_store(fs, fss, data, reloids);
	else
		return lc_update_fss(fs, fss, data, reloids);
}

/*
 * Forms ArrayType object for storage from simple C-array matrix.
 */
ArrayType *
form_matrix(double *matrix, int nrows, int ncols)
{
	Datum	   *elems;
	ArrayType  *array;
	int			dims[2] = {nrows, ncols};
	int			lbs[2];
	int			i,
				j;

	lbs[0] = lbs[1] = 1;
	elems = palloc(sizeof(*elems) * nrows * ncols);
	for (i = 0; i < nrows; ++i)
		for (j = 0; j < ncols; ++j)
			elems[i * ncols + j] = Float8GetDatum(matrix[i * ncols + j]);

	array = construct_md_array(elems, NULL, 2, dims, lbs,
							   FLOAT8OID, 8, FLOAT8PASSBYVAL, 'd');
	pfree(elems);
	return array;
}

/*
 * Forms ArrayType object for storage from simple C-array vector.
 */
ArrayType *
form_vector(double *vector, int nrows)
{
	Datum	   *elems;
	ArrayType  *array;
	int			dims[1];
	int			lbs[1];
	int			i;

	dims[0] = nrows;
	lbs[0] = 1;
	elems = palloc(sizeof(*elems) * nrows);
	for (i = 0; i < nrows; ++i)
		elems[i] = Float8GetDatum(vector[i]);
	array = construct_md_array(elems, NULL, 1, dims, lbs,
							   FLOAT8OID, 8, FLOAT8PASSBYVAL, 'd');
	pfree(elems);
	return array;
}

/*
 * Returns true if updated successfully, false if updated concurrently by
 * another session, error otherwise.
 */
static bool
my_simple_heap_update(Relation relation, ItemPointer otid, HeapTuple tup,
					  bool *update_indexes)
{
	TM_Result result;
	TM_FailureData hufd;
	LockTupleMode lockmode;

	Assert(update_indexes != NULL);
	result = heap_update(relation, otid, tup,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* wait for commit */ ,
						 &hufd, &lockmode);
	switch (result)
	{
		case TM_SelfModified:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* done successfully */
			if (!HeapTupleIsHeapOnly(tup))
				*update_indexes = true;
			else
				*update_indexes = false;
			return true;

		case TM_Updated:
			return false;
			break;

		case TM_BeingModified:
			return false;
			break;

		default:
			elog(ERROR, "unrecognized heap_update status: %u", result);
			break;
	}
	return false;
}


/* Provides correct insert in both PostgreQL 9.6.X and 10.X.X */
bool
my_index_insert(Relation indexRelation,
				Datum *values, bool *isnull,
				ItemPointer heap_t_ctid,
				Relation heapRelation,
				IndexUniqueCheck checkUnique)
{
	/* Index must be UNIQUE to support uniqueness checks */
	Assert(checkUnique == UNIQUE_CHECK_NO ||
		   indexRelation->rd_index->indisunique);

#if PG_VERSION_NUM < 100000
	return index_insert(indexRelation, values, isnull, heap_t_ctid,
						heapRelation, checkUnique);
#elif PG_VERSION_NUM < 140000
	return index_insert(indexRelation, values, isnull, heap_t_ctid,
						heapRelation, checkUnique,
						BuildIndexInfo(indexRelation));
#else
	return index_insert(indexRelation, values, isnull, heap_t_ctid,
						heapRelation, checkUnique, false,
						BuildIndexInfo(indexRelation));
#endif
}

/* Creates a storage for hashes of deactivated queries */
void
init_deactivated_queries_storage(void)
{
	HASHCTL		hash_ctl;

	/* Create the hashtable proper */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(uint64);
	hash_ctl.entrysize = sizeof(uint64);
	deactivated_queries = hash_create("aqo_deactivated_queries",
									  128,		/* start small and extend */
									  &hash_ctl,
									  HASH_ELEM | HASH_BLOBS);
}

/* Destroys the storage for hash of deactivated queries */
void
fini_deactivated_queries_storage(void)
{
	hash_destroy(deactivated_queries);
	deactivated_queries = NULL;
}

/* Checks whether the query with given hash is deactivated */
bool
query_is_deactivated(uint64 query_hash)
{
	bool		found;

	hash_search(deactivated_queries, &query_hash, HASH_FIND, &found);
	return found;
}

/* Adds given query hash into the set of hashes of deactivated queries*/
void
add_deactivated_query(uint64 query_hash)
{
	hash_search(deactivated_queries, &query_hash, HASH_ENTER, NULL);
}

/* *****************************************************************************
 *
 * Implementation of the AQO file storage
 *
 **************************************************************************** */

#include "funcapi.h"
#include "pgstat.h"

#define PGAQO_STAT_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pgaqo_statistics.stat"
#define PGAQO_TEXT_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pgaqo_query_texts.stat"
#define PGAQO_DATA_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pgaqo_data.stat"

PG_FUNCTION_INFO_V1(aqo_query_stat);
PG_FUNCTION_INFO_V1(aqo_query_texts);
PG_FUNCTION_INFO_V1(aqo_data);
PG_FUNCTION_INFO_V1(aqo_stat_remove);
PG_FUNCTION_INFO_V1(aqo_qtexts_remove);
PG_FUNCTION_INFO_V1(aqo_data_remove);
PG_FUNCTION_INFO_V1(aqo_reset);

typedef enum {
	QUERYID = 0, EXEC_TIME_AQO, EXEC_TIME, PLAN_TIME_AQO, PLAN_TIME,
	EST_ERROR_AQO, EST_ERROR, NEXECS_AQO, NEXECS, TOTAL_NCOLS
} aqo_stat_cols;

typedef enum {
	QT_QUERYID = 0, QT_QUERY_STRING, QT_TOTAL_NCOLS
} aqo_qtexts_cols;

typedef enum {
	AD_FS = 0, AD_FSS, AD_NFEATURES, AD_FEATURES, AD_TARGETS, AD_RELIABILITY,
	AD_OIDS, AD_TOTAL_NCOLS
} aqo_data_cols;

typedef void* (*form_record_t) (void *ctx, size_t *size);
typedef void (*deform_record_t) (void *data, size_t size);

bool aqo_use_file_storage;

HTAB *stat_htab = NULL;
HTAB *queries_htab = NULL; /* TODO */

HTAB *qtexts_htab = NULL;
dsa_area *qtext_dsa = NULL;

HTAB *data_htab = NULL;
dsa_area *data_dsa = NULL;

/* Used to check data file consistency */
static const uint32 PGAQO_FILE_HEADER = 123467589;
static const uint32 PGAQO_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

static void dsa_init(void);
static int data_store(const char *filename, form_record_t callback,
					  long nrecs, void *ctx);
static void data_load(const char *filename, deform_record_t callback, void *ctx);
static size_t _compute_data_dsa(const DataEntry *entry);
/*
 * Update AQO statistics.
 *
 * Add a record (and replace old, if all stat slots is full) to stat slot for
 * a query class.
 * Returns a copy of stat entry, allocated in current memory context. Caller is
 * in charge to free this struct after usage.
 */
StatEntry *
aqo_stat_store(uint64 queryid, bool use_aqo,
			   double plan_time, double exec_time, double est_error)
{
	StatEntry  *entry;
	bool		found;
	int			pos;

	Assert(stat_htab);

	LWLockAcquire(&aqo_state->stat_lock, LW_EXCLUSIVE);
	entry = (StatEntry *) hash_search(stat_htab, &queryid, HASH_ENTER, &found);

	/* Initialize entry on first usage */
	if (!found)
	{
		uint64 qid = entry->queryid;
		memset(entry, 0, sizeof(StatEntry));
		entry->queryid = qid;
	}

	/* Update the entry data */

	if (use_aqo)
	{
		Assert(entry->cur_stat_slot_aqo >= 0);
		pos = entry->cur_stat_slot_aqo;
		if (entry->cur_stat_slot_aqo < STAT_SAMPLE_SIZE - 1)
			entry->cur_stat_slot_aqo++;
		else
		{
			size_t sz = (STAT_SAMPLE_SIZE - 1) * sizeof(entry->est_error_aqo[0]);

			Assert(entry->cur_stat_slot_aqo = STAT_SAMPLE_SIZE - 1);
			memmove(entry->plan_time_aqo, &entry->plan_time_aqo[1], sz);
			memmove(entry->exec_time_aqo, &entry->exec_time_aqo[1], sz);
			memmove(entry->est_error_aqo, &entry->est_error_aqo[1], sz);
		}

		entry->execs_with_aqo++;
		entry->plan_time_aqo[pos] = plan_time;
		entry->exec_time_aqo[pos] = exec_time;
		entry->est_error_aqo[pos] = est_error;
	}
	else
	{
		Assert(entry->cur_stat_slot >= 0);
		pos = entry->cur_stat_slot;
		if (entry->cur_stat_slot < STAT_SAMPLE_SIZE - 1)
			entry->cur_stat_slot++;
		else
		{
			size_t sz = (STAT_SAMPLE_SIZE - 1) * sizeof(entry->est_error[0]);

			Assert(entry->cur_stat_slot = STAT_SAMPLE_SIZE - 1);
			memmove(entry->plan_time, &entry->plan_time[1], sz);
			memmove(entry->exec_time, &entry->exec_time[1], sz);
			memmove(entry->est_error, &entry->est_error[1], sz);
		}

		entry->execs_without_aqo++;
		entry->plan_time[pos] = plan_time;
		entry->exec_time[pos] = exec_time;
		entry->est_error[pos] = est_error;
	}
	entry = memcpy(palloc(sizeof(StatEntry)), entry, sizeof(StatEntry));
	LWLockRelease(&aqo_state->stat_lock);
	return entry;
}

/*
 * Returns AQO statistics on controlled query classes.
 */
Datum
aqo_query_stat(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupDesc;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	Tuplestorestate	   *tupstore;
	Datum				values[TOTAL_NCOLS + 1];
	bool				nulls[TOTAL_NCOLS + 1];
	HASH_SEQ_STATUS		hash_seq;
	StatEntry	   *entry;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	Assert(tupDesc->natts == TOTAL_NCOLS);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupDesc;

	MemoryContextSwitchTo(oldcontext);

	memset(nulls, 0, TOTAL_NCOLS + 1);
	LWLockAcquire(&aqo_state->stat_lock, LW_SHARED);
	hash_seq_init(&hash_seq, stat_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		values[QUERYID] = Int64GetDatum(entry->queryid);
		values[NEXECS] = Int64GetDatum(entry->execs_without_aqo);
		values[NEXECS_AQO] = Int64GetDatum(entry->execs_with_aqo);
		values[EXEC_TIME_AQO] = PointerGetDatum(form_vector(entry->exec_time_aqo, entry->cur_stat_slot_aqo));
		values[EXEC_TIME] = PointerGetDatum(form_vector(entry->exec_time, entry->cur_stat_slot));
		values[PLAN_TIME_AQO] = PointerGetDatum(form_vector(entry->plan_time_aqo, entry->cur_stat_slot_aqo));
		values[PLAN_TIME] = PointerGetDatum(form_vector(entry->plan_time, entry->cur_stat_slot));
		values[EST_ERROR_AQO] = PointerGetDatum(form_vector(entry->est_error_aqo, entry->cur_stat_slot_aqo));
		values[EST_ERROR] = PointerGetDatum(form_vector(entry->est_error, entry->cur_stat_slot));
		tuplestore_putvalues(tupstore, tupDesc, values, nulls);
	}

	LWLockRelease(&aqo_state->stat_lock);
	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}

static long
aqo_stat_reset(void)
{
	HASH_SEQ_STATUS	hash_seq;
	StatEntry	   *entry;
	long			num_remove = 0;
	long			num_entries;

	LWLockAcquire(&aqo_state->stat_lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(stat_htab);
	hash_seq_init(&hash_seq, stat_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (hash_search(stat_htab, &entry->queryid, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "[AQO] hash table corrupted");
		num_remove++;
	}
	LWLockRelease(&aqo_state->stat_lock);
	Assert(num_remove == num_entries); /* Is it really impossible? */

	aqo_stat_flush();

	return num_remove;
}

Datum
aqo_stat_remove(PG_FUNCTION_ARGS)
{
	uint64		queryid = (uint64) PG_GETARG_INT64(0);
	StatEntry   *entry;
	bool		removed;

	LWLockAcquire(&aqo_state->stat_lock, LW_EXCLUSIVE);
	entry = (StatEntry *) hash_search(stat_htab, &queryid, HASH_REMOVE, NULL);
	removed = (entry) ? true : false;
	LWLockRelease(&aqo_state->stat_lock);
	PG_RETURN_BOOL(removed);
}

static void *
_form_stat_record_cb(void *ctx, size_t *size)
{
	HASH_SEQ_STATUS *hash_seq = (HASH_SEQ_STATUS *) ctx;
	StatEntry		*entry;

	*size = sizeof(StatEntry);
	entry = hash_seq_search(hash_seq);
	if (entry == NULL)
		return NULL;

	return memcpy(palloc(*size), entry, *size);
}

/* Implement data flushing according to pgss_shmem_shutdown() */

void
aqo_stat_flush(void)
{
	HASH_SEQ_STATUS	hash_seq;
	int				ret;
	long			entries;

	LWLockAcquire(&aqo_state->stat_lock, LW_SHARED);
	entries = hash_get_num_entries(stat_htab);
	hash_seq_init(&hash_seq, stat_htab);
	ret = data_store(PGAQO_STAT_FILE, _form_stat_record_cb, entries,
					 (void *) &hash_seq);
	if (ret != 0)
		hash_seq_term(&hash_seq);

	LWLockRelease(&aqo_state->stat_lock);
}

static void *
_form_qtext_record_cb(void *ctx, size_t *size)
{
	HASH_SEQ_STATUS *hash_seq = (HASH_SEQ_STATUS *) ctx;
	QueryTextEntry	*entry;
	void		    *data;
	char			*query_string;
	char			*ptr;

	entry = hash_seq_search(hash_seq);
	if (entry == NULL)
		return NULL;

	Assert(DsaPointerIsValid(entry->qtext_dp));
	query_string = dsa_get_address(qtext_dsa, entry->qtext_dp);
	*size = sizeof(entry->queryid) + strlen(query_string) + 1;
	data = palloc(*size);
	ptr = data;
	memcpy(ptr, &entry->queryid, sizeof(entry->queryid));
	ptr += sizeof(entry->queryid);
	memcpy(ptr, query_string, strlen(query_string) + 1);
	return data;
}

void
aqo_qtexts_flush(void)
{
	HASH_SEQ_STATUS	hash_seq;
	int				ret;
	long			entries;

	dsa_init();
	LWLockAcquire(&aqo_state->qtexts_lock, LW_SHARED);

	if (!aqo_state->qtexts_changed)
		/* XXX: mull over forced mode. */
		goto end;

	entries = hash_get_num_entries(qtexts_htab);
	hash_seq_init(&hash_seq, qtexts_htab);
	ret = data_store(PGAQO_TEXT_FILE, _form_qtext_record_cb, entries,
					 (void *) &hash_seq);
	if (ret != 0)
		hash_seq_term(&hash_seq);
	aqo_state->qtexts_changed = false;

end:
	LWLockRelease(&aqo_state->qtexts_lock);
}

/*
 * Getting a hash table iterator, return a newly allocated memory chunk and its
 * size for subsequent writing into storage.
 */
static void *
_form_data_record_cb(void *ctx, size_t *size)
{
	HASH_SEQ_STATUS	   *hash_seq = (HASH_SEQ_STATUS *) ctx;
	DataEntry		   *entry;
	char			   *data;
	char			   *ptr,
					   *dsa_ptr;
	size_t				sz;

	entry = hash_seq_search(hash_seq);
	if (entry == NULL)
		return NULL;

	/* Size of data is DataEntry (without DSA pointer) plus size of DSA chunk */
	sz = offsetof(DataEntry, data_dp) + _compute_data_dsa(entry);
	ptr = data = palloc(sz);

	/* Put the data into the chunk */

	/* Plane copy of all bytes of hash table entry */
	memcpy(ptr, entry, offsetof(DataEntry, data_dp));
	ptr += offsetof(DataEntry, data_dp);

	Assert(DsaPointerIsValid(entry->data_dp));
	dsa_ptr = (char *) dsa_get_address(data_dsa, entry->data_dp);
	Assert((sz - (ptr - data)) == _compute_data_dsa(entry));
	memcpy(ptr, dsa_ptr, sz - (ptr - data));
	*size = sz;
	return data;
}

void
aqo_data_flush(void)
{
	HASH_SEQ_STATUS	hash_seq;
	int				ret;
	long			entries;

	dsa_init();
	LWLockAcquire(&aqo_state->data_lock, LW_SHARED);

	if (!aqo_state->data_changed)
		/* XXX: mull over forced mode. */
		goto end;

	entries = hash_get_num_entries(data_htab);
	hash_seq_init(&hash_seq, data_htab);
	ret = data_store(PGAQO_DATA_FILE, _form_data_record_cb, entries,
					 (void *) &hash_seq);
	if (ret != 0)
		/*
		 * Something happened and storing procedure hasn't finished walking
		 * along all records of the hash table.
		 */
		hash_seq_term(&hash_seq);
	else
		aqo_state->data_changed = false;
end:
	LWLockRelease(&aqo_state->data_lock);
}

static int
data_store(const char *filename, form_record_t callback,
		   long nrecs, void *ctx)
{
	FILE   *file;
	size_t	size;
	uint	counter = 0;
	void   *data;
	char   *tmpfile;

	tmpfile = psprintf("%s.tmp", filename);
	file = AllocateFile(tmpfile, PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGAQO_FILE_HEADER, sizeof(uint32), 1, file) != 1 ||
		fwrite(&PGAQO_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1 ||
		fwrite(&nrecs, sizeof(long), 1, file) != 1)
		goto error;

	while ((data = callback(ctx, &size)) != NULL)
	{
		/* TODO: Add CRC code ? */
		if (fwrite(&size, sizeof(size), 1, file) != 1 ||
			fwrite(data, size, 1, file) != 1)
			goto error;
		pfree(data);
		counter++;
	}

	Assert(counter == nrecs);
	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	(void) durable_rename(tmpfile, filename, LOG);
	pfree(tmpfile);
	elog(LOG, "[AQO] %d records stored in file %s.", counter, filename);
	return 0;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write AQO file \"%s\": %m", tmpfile)));

	if (file)
		FreeFile(file);
	unlink(tmpfile);
	pfree(tmpfile);
	return -1;
}

static void
_deform_stat_record_cb(void *data, size_t size)
{
	bool		found;
	StatEntry  *entry;
	uint64		queryid;

	Assert(LWLockHeldByMeInMode(&aqo_state->stat_lock, LW_EXCLUSIVE));
	Assert(size == sizeof(StatEntry));

	queryid = ((StatEntry *) data)->queryid;
	entry = (StatEntry *) hash_search(stat_htab, &queryid, HASH_ENTER, &found);
	Assert(!found);
	memcpy(entry, data, sizeof(StatEntry));
}

void
aqo_stat_load(void)
{
	long	entries;

	Assert(!LWLockHeldByMe(&aqo_state->stat_lock));

	LWLockAcquire(&aqo_state->stat_lock, LW_EXCLUSIVE);
	entries = hash_get_num_entries(stat_htab);
	Assert(entries == 0);
	data_load(PGAQO_STAT_FILE, _deform_stat_record_cb, NULL);

	LWLockRelease(&aqo_state->stat_lock);
}

static void
_deform_qtexts_record_cb(void *data, size_t size)
{
	bool			found;
	QueryTextEntry *entry;
	uint64			queryid = *(uint64 *) data;
	char		   *query_string = (char *) data + sizeof(queryid);
	size_t			len = size - sizeof(queryid);
	char		   *strptr;

	Assert(LWLockHeldByMeInMode(&aqo_state->qtexts_lock, LW_EXCLUSIVE));
	Assert(strlen(query_string) + 1 == len);
	entry = (QueryTextEntry *) hash_search(qtexts_htab, &queryid,
										   HASH_ENTER, &found);
	Assert(!found);

	entry->qtext_dp = dsa_allocate(qtext_dsa, len);
	Assert(DsaPointerIsValid(entry->qtext_dp));
	strptr = (char *) dsa_get_address(qtext_dsa, entry->qtext_dp);
	strlcpy(strptr, query_string, len);
}

void
aqo_qtexts_load(void)
{
	uint64	queryid = 0;
	bool	found;

	Assert(!LWLockHeldByMe(&aqo_state->qtexts_lock));
	Assert(qtext_dsa != NULL);

	LWLockAcquire(&aqo_state->qtexts_lock, LW_EXCLUSIVE);
	Assert(hash_get_num_entries(qtexts_htab) == 0);
	data_load(PGAQO_TEXT_FILE, _deform_qtexts_record_cb, NULL);

	/* Check existence of default feature space */
	(void) hash_search(qtexts_htab, &queryid, HASH_FIND, &found);

	aqo_state->qtexts_changed = false; /* mem data consistent with disk */
	LWLockRelease(&aqo_state->qtexts_lock);

	if (!found)
	{
		if (!aqo_qtext_store(0, "COMMON feature space (do not delete!)"))
			elog(PANIC, "[AQO] DSA Initialization was unsuccessful");
	}
}

/*
 * Getting a data chunk from a caller, add a record into the 'ML data'
 * shmem hash table. Allocate and fill DSA chunk for variadic part of the data.
 */
static void
_deform_data_record_cb(void *data, size_t size)
{
	bool		found;
	DataEntry  *fentry = (DataEntry *) data; /*Depends on a platform? */
	DataEntry  *entry;
	size_t		sz;
	char	   *ptr = (char *) data,
			   *dsa_ptr;

	Assert(LWLockHeldByMeInMode(&aqo_state->data_lock, LW_EXCLUSIVE));
	entry = (DataEntry *) hash_search(data_htab, &fentry->key,
										   HASH_ENTER, &found);
	Assert(!found);

	/* Copy fixed-size part of entry byte-by-byte even with caves */
	memcpy(entry, fentry, offsetof(DataEntry, data_dp));
	ptr += offsetof(DataEntry, data_dp);

	sz = _compute_data_dsa(entry);
	Assert(sz + offsetof(DataEntry, data_dp) == size);
	entry->data_dp = dsa_allocate(data_dsa, sz);
	Assert(DsaPointerIsValid(entry->data_dp));
	dsa_ptr = (char *) dsa_get_address(data_dsa, entry->data_dp);
	memcpy(dsa_ptr, ptr, sz);
}

void
aqo_data_load(void)
{
	Assert(!LWLockHeldByMe(&aqo_state->data_lock));
	Assert(data_dsa != NULL);

	LWLockAcquire(&aqo_state->data_lock, LW_EXCLUSIVE);
	Assert(hash_get_num_entries(data_htab) == 0);
	data_load(PGAQO_DATA_FILE, _deform_data_record_cb, NULL);

	aqo_state->data_changed = false; /* mem data is consistent with disk */
	LWLockRelease(&aqo_state->data_lock);
}

static void
data_load(const char *filename, deform_record_t callback, void *ctx)
{
	FILE   *file;
	long	i;
	uint32	header;
	int32	pgver;
	long	num;

	file = AllocateFile(filename, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		return;
	}

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1 ||
		fread(&num, sizeof(long), 1, file) != 1)
		goto read_error;

	if (header != PGAQO_FILE_HEADER || pgver != PGAQO_PG_MAJOR_VERSION)
		goto data_error;

	for (i = 0; i < num; i++)
	{
		void   *data;
		size_t	size;

		if (fread(&size, sizeof(size), 1, file) != 1)
			goto read_error;
		data = palloc(size);
		if (fread(data, size, 1, file) != 1)
			goto read_error;
		callback(data, size);
		pfree(data);
	}

	FreeFile(file);
	unlink(filename);

	elog(LOG, "[AQO] %ld records loaded from file %s.", num, filename);
	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m", filename)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"", filename)));
fail:
	if (file)
		FreeFile(file);
	unlink(filename);
}

static void
on_shmem_shutdown(int code, Datum arg)
{
	aqo_qtexts_flush();
	aqo_data_flush();
}

/*
 * Initialize DSA memory for AQO shared data with variable length.
 * On first call, create DSA segments and load data into hash table and DSA
 * from disk.
 */
static void
dsa_init()
{
	MemoryContext	old_context;

	if (qtext_dsa)
		return;

	Assert(data_dsa == NULL && data_dsa == NULL);
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	LWLockAcquire(&aqo_state->lock, LW_EXCLUSIVE);

	if (aqo_state->qtexts_dsa_handler == DSM_HANDLE_INVALID)
	{
		Assert(aqo_state->data_dsa_handler == DSM_HANDLE_INVALID);

		qtext_dsa = dsa_create(aqo_state->qtext_trancheid);
		dsa_pin(qtext_dsa);
		aqo_state->qtexts_dsa_handler = dsa_get_handle(qtext_dsa);

		data_dsa = dsa_create(aqo_state->data_trancheid);
		dsa_pin(data_dsa);
		aqo_state->data_dsa_handler = dsa_get_handle(data_dsa);

		/* Load and initialize quuery texts hash table */
		aqo_qtexts_load();
		aqo_data_load();
	}
	else
	{
		qtext_dsa = dsa_attach(aqo_state->qtexts_dsa_handler);
		data_dsa = dsa_attach(aqo_state->data_dsa_handler);
	}

	dsa_pin_mapping(qtext_dsa);
	dsa_pin_mapping(data_dsa);
	MemoryContextSwitchTo(old_context);
	LWLockRelease(&aqo_state->lock);

	before_shmem_exit(on_shmem_shutdown, (Datum) 0);
}

/* ************************************************************************** */

/*
 * XXX: Maybe merge with aqo_queries ?
 */
bool
aqo_qtext_store(uint64 queryid, const char *query_string)
{
	QueryTextEntry *entry;
	bool			found;

	Assert(!LWLockHeldByMe(&aqo_state->qtexts_lock));

	if (query_string == NULL)
		return false;

	dsa_init();

	LWLockAcquire(&aqo_state->qtexts_lock, LW_EXCLUSIVE);
	entry = (QueryTextEntry *) hash_search(qtexts_htab, &queryid, HASH_ENTER,
										   &found);

	/* Initialize entry on first usage */
	if (!found)
	{
		size_t size = strlen(query_string) + 1;
		char *strptr;

		entry->queryid = queryid;
		entry->qtext_dp = dsa_allocate(qtext_dsa, size);
		Assert(DsaPointerIsValid(entry->qtext_dp));
		strptr = (char *) dsa_get_address(qtext_dsa, entry->qtext_dp);
		strlcpy(strptr, query_string, size);
		aqo_state->qtexts_changed = true;
	}
	LWLockRelease(&aqo_state->qtexts_lock);
	return !found;
}

Datum
aqo_query_texts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupDesc;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	Tuplestorestate	   *tupstore;
	Datum				values[QT_TOTAL_NCOLS];
	bool				nulls[QT_TOTAL_NCOLS];
	HASH_SEQ_STATUS		hash_seq;
	QueryTextEntry	   *entry;

	Assert(!LWLockHeldByMe(&aqo_state->qtexts_lock));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	Assert(tupDesc->natts == QT_TOTAL_NCOLS);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupDesc;

	MemoryContextSwitchTo(oldcontext);

	dsa_init();
	memset(nulls, 0, QT_TOTAL_NCOLS);
	LWLockAcquire(&aqo_state->qtexts_lock, LW_SHARED);
	hash_seq_init(&hash_seq, qtexts_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Assert(DsaPointerIsValid(entry->qtext_dp));
		char *ptr = dsa_get_address(qtext_dsa, entry->qtext_dp);
		values[QT_QUERYID] = Int64GetDatum(entry->queryid);
		values[QT_QUERY_STRING] = CStringGetTextDatum(ptr);
		tuplestore_putvalues(tupstore, tupDesc, values, nulls);
	}

	LWLockRelease(&aqo_state->qtexts_lock);
	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}

Datum
aqo_qtexts_remove(PG_FUNCTION_ARGS)
{
	uint64			queryid = (uint64) PG_GETARG_INT64(0);
	bool			found = false;
	QueryTextEntry *entry;

	dsa_init();

	Assert(!LWLockHeldByMe(&aqo_state->qtexts_lock));
	LWLockAcquire(&aqo_state->qtexts_lock, LW_EXCLUSIVE);

	/*
	 * Look for a record with this queryid. DSA fields must be freed before
	 * deletion of the record.
	 */
	entry = (QueryTextEntry *) hash_search(qtexts_htab, &queryid, HASH_FIND, &found);
	if (!found)
		goto end;

	/* Free DSA memory, allocated foro this record */
	Assert(DsaPointerIsValid(entry->qtext_dp));
	dsa_free(qtext_dsa, entry->qtext_dp);

	(void) hash_search(qtexts_htab, &queryid, HASH_REMOVE, &found);
	Assert(found);
end:
	LWLockRelease(&aqo_state->qtexts_lock);
	PG_RETURN_BOOL(found);
}

static long
aqo_qtexts_reset(void)
{
	HASH_SEQ_STATUS	hash_seq;
	QueryTextEntry *entry;
	long			num_remove = 0;
	long			num_entries;

	dsa_init();

	Assert(!LWLockHeldByMe(&aqo_state->qtexts_lock));
	LWLockAcquire(&aqo_state->qtexts_lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(qtexts_htab);
	hash_seq_init(&hash_seq, qtexts_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (entry->queryid == 0)
			continue;

		Assert(DsaPointerIsValid(entry->qtext_dp));
		dsa_free(qtext_dsa, entry->qtext_dp);
		if (hash_search(qtexts_htab, &entry->queryid, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "[AQO] hash table corrupted");
		num_remove++;
	}
	aqo_state->qtexts_changed = true;
	LWLockRelease(&aqo_state->qtexts_lock);
	Assert(num_remove == num_entries - 1); /* Is it really impossible? */

	/* TODO: clean disk storage */

	return num_remove;
}

static size_t
_compute_data_dsa(const DataEntry *entry)
{
	size_t	size = sizeof(data_key); /* header's size */

	size += sizeof(double) * entry->rows * entry->cols; /* matrix */
	size += 2 * sizeof(double) * entry->rows; /* targets, rfactors */

	/* Calculate memory size needed to store relation names */
	size += entry->nrels * sizeof(Oid);
	return size;
}

/*
 * Insert new record or update existed in the AQO data storage.
 * Return true if data was changed.
 */
bool
aqo_data_store(uint64 fs, int fss, OkNNrdata *data, List *reloids)
{
	DataEntry  *entry;
	bool		found;
	data_key	key = {.fs = fs, .fss = fss};
	int			i;
	char	   *ptr;
	ListCell   *lc;
	size_t		size;

	Assert(!LWLockHeldByMe(&aqo_state->data_lock));

	dsa_init();

	LWLockAcquire(&aqo_state->data_lock, LW_EXCLUSIVE);
	entry = (DataEntry *) hash_search(data_htab, &key, HASH_ENTER, &found);

	/* Initialize entry on first usage */
	if (!found)
	{
		entry->cols = data->cols;
		entry->rows = data->rows;
		entry->nrels = list_length(reloids);

		size = _compute_data_dsa(entry);
		entry->data_dp = dsa_allocate0(data_dsa, size);
		Assert(DsaPointerIsValid(entry->data_dp));
	}

	Assert(DsaPointerIsValid(entry->data_dp));
	Assert(entry->rows <= data->rows); /* Reserved for the future features */

	if (entry->cols != data->cols || entry->nrels != list_length(reloids))
	{
		/* Collision happened? */
		elog(LOG, "[AQO] Does a collision happened? Check it if possible (fs: %lu, fss: %d).",
			 fs, fss);
		goto end;
	}

	if (entry->rows < data->rows)
	{
		entry->rows = data->rows;
		size = _compute_data_dsa(entry);

		/* Need to re-allocate DSA chunk */
		dsa_free(data_dsa, entry->data_dp);
		entry->data_dp = dsa_allocate0(data_dsa, size);
		Assert(DsaPointerIsValid(entry->data_dp));
	}
	ptr = (char *) dsa_get_address(data_dsa, entry->data_dp);

	/*
	 * Copy AQO data into allocated DSA segment
	 */

	memcpy(ptr, &key, sizeof(data_key)); /* Just for debug */
	ptr += sizeof(data_key);
	if (entry->cols > 0)
	{
		for (i = 0; i < entry->rows; i++)
		{
			memcpy(ptr, data->matrix[i], sizeof(double) * data->cols);
			ptr += sizeof(double) * data->cols;
		}
	}
	/* copy targets into DSM storage */
	memcpy(ptr, data->targets, sizeof(double) * entry->rows);
	ptr += sizeof(double) * entry->rows;
	/* copy rfactors into DSM storage */
	memcpy(ptr, data->rfactors, sizeof(double) * entry->rows);
	ptr += sizeof(double) * entry->rows;
	/* store list of relations. XXX: optimize ? */
	foreach(lc, reloids)
	{
		Oid reloid = lfirst_oid(lc);

		memcpy(ptr, &reloid, sizeof(Oid));
		ptr += sizeof(Oid);
	}

	aqo_state->data_changed = true;
end:
	LWLockRelease(&aqo_state->data_lock);
	return aqo_state->data_changed;
}

static void
build_knn_matrix(OkNNrdata *data, const OkNNrdata *temp_data)
{
	Assert(data->cols == temp_data->cols);

	if (data->rows >= 0)
		/* trivial strategy - use first suitable record and ignore others */
		return;

	memcpy(data, temp_data, sizeof(OkNNrdata));
	if (data->cols > 0)
	{
		int i;

		for (i = 0; i < data->rows; i++)
			memcpy(data->matrix[i], temp_data->matrix[i], data->cols * sizeof(double));
	}
}

static OkNNrdata *
_fill_knn_data(const DataEntry *entry, List **reloids)
{
	OkNNrdata *data;
	char	   *ptr;
	int			i;
	size_t		offset;
	size_t		sz = _compute_data_dsa(entry);

	data = OkNNr_allocate(entry->cols);
	data->rows = entry->rows;

	ptr = (char *) dsa_get_address(data_dsa, entry->data_dp);

	/* Check invariants */
	Assert(entry->rows < aqo_K);
	Assert(ptr != NULL);
	Assert(entry->key.fs == ((data_key *)ptr)->fs &&
		   entry->key.fss == ((data_key *)ptr)->fss);

	ptr += sizeof(data_key);

	if (entry->cols > 0)
	{
		for (i = 0; i < entry->rows; i++)
		{
			memcpy(data->matrix[i], ptr, sizeof(double) * data->cols);
			ptr += sizeof(double) * data->cols;
		}
	}
	/* copy targets from DSM storage */
	memcpy(data->targets, ptr, sizeof(double) * entry->rows);
	ptr += sizeof(double) * entry->rows;
	offset = ptr - (char *) dsa_get_address(data_dsa, entry->data_dp);
	Assert(offset < sz);

	/* copy rfactors from DSM storage */
	memcpy(data->rfactors, ptr, sizeof(double) * entry->rows);
	ptr += sizeof(double) * entry->rows;
	offset = ptr - (char *) dsa_get_address(data_dsa, entry->data_dp);
	Assert(offset <= sz);

	if (reloids == NULL)
		return data;

	/* store list of relations. XXX: optimize ? */
	for (i = 0; i < entry->nrels; i++)
	{
		*reloids = lappend_oid(*reloids, ObjectIdGetDatum(*(Oid*)ptr));
		ptr += sizeof(Oid);
	}
	Assert(ptr - (char *) dsa_get_address(data_dsa, entry->data_dp) == sz);
	return data;
}

/*
 * Return on feature subspace, unique defined by its class (fs) and hash value
 * (fss).
 * If reloids is NULL, skip loading of this list.
 * If wideSearch is true - make seqscan on the hash table to see for relevant
 * data across neighbours.
 */
bool
load_aqo_data(uint64 fs, int fss, OkNNrdata *data, List **reloids,
			  bool wideSearch)
{
	DataEntry  *entry;
	bool		found;
	data_key	key = {.fs = fs, .fss = fss};
	OkNNrdata  *temp_data;

	Assert(!LWLockHeldByMe(&aqo_state->data_lock));

	dsa_init();

	LWLockAcquire(&aqo_state->data_lock, LW_EXCLUSIVE);
	entry = (DataEntry *) hash_search(data_htab, &key, HASH_FIND, &found);

	if (!found)
		goto end;

	/* One entry with all correctly filled fields is found */
	Assert(entry);
	Assert(DsaPointerIsValid(entry->data_dp));

	if (entry->cols != data->cols)
	{
		/* Collision happened? */
		elog(LOG, "[AQO] Does a collision happened? Check it if possible (fs: %lu, fss: %d).",
			 fs, fss);
		found = false;
		goto end;
	}

	temp_data = _fill_knn_data(entry, reloids);
	build_knn_matrix(data, temp_data);
end:
	LWLockRelease(&aqo_state->data_lock);

	return found;
}

Datum
aqo_data(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupDesc;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	Tuplestorestate	   *tupstore;
	Datum				values[AD_TOTAL_NCOLS];
	bool				nulls[AD_TOTAL_NCOLS];
	HASH_SEQ_STATUS		hash_seq;
	DataEntry		   *entry;

	Assert(!LWLockHeldByMe(&aqo_state->data_lock));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	Assert(tupDesc->natts == AD_TOTAL_NCOLS);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupDesc;

	MemoryContextSwitchTo(oldcontext);

	dsa_init();
	memset(nulls, 0, AD_TOTAL_NCOLS);
	LWLockAcquire(&aqo_state->data_lock, LW_SHARED);
	hash_seq_init(&hash_seq, data_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		char *ptr;

		values[AD_FS] = Int64GetDatum(entry->key.fs);
		values[AD_FSS] = Int64GetDatum(entry->key.fss);
		values[AD_NFEATURES] = Int32GetDatum(entry->cols);

		/* Fill values from the DSA data chunk */
		Assert(DsaPointerIsValid(entry->data_dp));
		ptr = dsa_get_address(data_dsa, entry->data_dp);
		Assert(entry->key.fs == ((data_key*)ptr)->fs && entry->key.fss == ((data_key*)ptr)->fss);
		ptr += sizeof(data_key);

		if (entry->cols > 0)
		values[AD_FEATURES] = PointerGetDatum(form_matrix((double *)ptr, entry->rows, entry->cols));
		else
			nulls[AD_FEATURES] = true;

		ptr += sizeof(double) * entry->rows * entry->cols;
		values[AD_TARGETS] = PointerGetDatum(form_vector((double *)ptr, entry->rows));
		ptr += sizeof(double) * entry->rows;
		values[AD_RELIABILITY] = PointerGetDatum(form_vector((double *)ptr, entry->rows));
		ptr += sizeof(double) * entry->rows;

		if (entry->nrels > 0)
		{
			Datum	   *elems;
			ArrayType  *array;
			int			i;

			elems = palloc(sizeof(*elems) * entry->nrels);
			for(i = 0; i < entry->nrels; i++)
				elems[i] = ObjectIdGetDatum(*(Oid *)ptr);

			array = construct_array(elems, entry->nrels, OIDOID,
									sizeof(Oid), true, TYPALIGN_INT);
			values[AD_OIDS] = PointerGetDatum(array);
			pfree(elems);
		}
		else
			nulls[AD_OIDS] = true;

		tuplestore_putvalues(tupstore, tupDesc, values, nulls);
	}

	LWLockRelease(&aqo_state->data_lock);
	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}

static long
_aqo_data_clean(uint64 fs)
{
	HASH_SEQ_STATUS	hash_seq;
	DataEntry	   *entry;
	long			removed = 0;

	Assert(LWLockHeldByMe(&aqo_state->data_lock));
	hash_seq_init(&hash_seq, data_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (entry->key.fs != fs)
			continue;

		Assert(DsaPointerIsValid(entry->data_dp));
		dsa_free(data_dsa, entry->data_dp);
		if (hash_search(data_htab, &entry->key, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "[AQO] hash table corrupted");
		removed++;
	}

	return removed;
}

Datum
aqo_data_remove(PG_FUNCTION_ARGS)
{
	data_key	key;
	bool		found;
	DataEntry  *entry;

	dsa_init();

	Assert(!LWLockHeldByMe(&aqo_state->data_lock));
	LWLockAcquire(&aqo_state->data_lock, LW_EXCLUSIVE);

	if (PG_ARGISNULL(1))
	{
		/* Remove all feature subspaces from the space */
		found = (_aqo_data_clean((uint64) PG_GETARG_INT64(0)) > 0);
		goto end;
	}

	key.fs = (uint64) PG_GETARG_INT64(0);
	key.fss = PG_GETARG_INT32(1);

	/*
	 * Look for a record with this queryid. DSA fields must be freed before
	 * deletion of the record.
	 */
	entry = (DataEntry *) hash_search(qtexts_htab, &key, HASH_FIND, &found);
	if (!found)
		goto end;

	/* Free DSA memory, allocated foro this record */
	Assert(DsaPointerIsValid(entry->data_dp));
	dsa_free(data_dsa, entry->data_dp);

	(void) hash_search(data_htab, &key, HASH_REMOVE, &found);
	Assert(found);
end:
	if (found)
		aqo_state->data_changed = true;
	LWLockRelease(&aqo_state->data_lock);
	PG_RETURN_BOOL(found);
}

static long
aqo_data_reset(void)
{
	HASH_SEQ_STATUS	hash_seq;
	DataEntry	   *entry;
	long			num_remove = 0;
	long			num_entries;

	dsa_init();

	Assert(!LWLockHeldByMe(&aqo_state->data_lock));
	LWLockAcquire(&aqo_state->data_lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(data_htab);
	hash_seq_init(&hash_seq, data_htab);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Assert(DsaPointerIsValid(entry->data_dp));
		dsa_free(data_dsa, entry->data_dp);
		if (hash_search(data_htab, &entry->key, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "[AQO] hash table corrupted");
		num_remove++;
	}
	aqo_state->data_changed = true;
	LWLockRelease(&aqo_state->data_lock);
	Assert(num_remove == num_entries);

	/* TODO: clean disk storage */

	return num_remove;
}

Datum
aqo_reset(PG_FUNCTION_ARGS)
{
	long counter = 0;

	counter += aqo_stat_reset();
	counter += aqo_qtexts_reset();
	counter += aqo_data_reset();
	PG_RETURN_INT64(counter);
}
