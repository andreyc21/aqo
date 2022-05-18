/* contrib/aqo/aqo--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION aqo UPDATE TO '1.5'" to load this file. \quit

--
-- Re-create the aqo_data table. Do so to keep the columns order.
-- The oids array contains oids of permanent tables only. It is used for cleanup
-- ML knowledge base from queries that refer to removed tables.
--
DROP TABLE public.aqo_data CASCADE;
CREATE TABLE public.aqo_data (
	fspace_hash	 bigint NOT NULL REFERENCES public.aqo_queries ON DELETE CASCADE,
	fsspace_hash int NOT NULL,
	nfeatures	 int NOT NULL,
	features	 double precision[][],
	targets		 double precision[],
	oids		 oid [] DEFAULT NULL,
	reliability	 double precision []
);
CREATE UNIQUE INDEX aqo_fss_access_idx ON public.aqo_data (fspace_hash, fsspace_hash);


--
-- Remove rows from the AQO ML knowledge base, related to previously dropped
-- tables of the database.
--
CREATE OR REPLACE FUNCTION public.clean_aqo_data() RETURNS void AS $$
DECLARE
    aqo_data_row aqo_data%ROWTYPE;
    aqo_queries_row aqo_queries%ROWTYPE;
    aqo_query_texts_row aqo_query_texts%ROWTYPE;
    aqo_query_stat_row aqo_query_stat%ROWTYPE;
    oid_var oid;
    fspace_hash_var bigint;
    delete_row boolean DEFAULT false;
BEGIN
  FOR aqo_data_row IN (SELECT * FROM aqo_data)
  LOOP
    delete_row = false;
    SELECT aqo_data_row.fspace_hash INTO fspace_hash_var FROM aqo_data;

    IF (aqo_data_row.oids IS NOT NULL) THEN
      FOREACH oid_var IN ARRAY aqo_data_row.oids
      LOOP
        IF NOT EXISTS (SELECT relname FROM pg_class WHERE oid = oid_var) THEN
          delete_row = true;
        END IF;
      END LOOP;
    END IF;

    FOR aqo_queries_row IN (SELECT * FROM public.aqo_queries)
    LOOP
      IF (delete_row = true AND fspace_hash_var <> 0 AND
          fspace_hash_var = aqo_queries_row.fspace_hash AND
          aqo_queries_row.fspace_hash = aqo_queries_row.query_hash) THEN
        DELETE FROM aqo_data WHERE aqo_data = aqo_data_row;
        DELETE FROM aqo_queries WHERE aqo_queries = aqo_queries_row;

        FOR aqo_query_texts_row IN (SELECT * FROM aqo_query_texts)
        LOOP
          DELETE FROM aqo_query_texts
          WHERE aqo_query_texts_row.query_hash = fspace_hash_var AND
				aqo_query_texts = aqo_query_texts_row;
        END LOOP;

        FOR aqo_query_stat_row IN (SELECT * FROM aqo_query_stat)
        LOOP
          DELETE FROM aqo_query_stat
          WHERE aqo_query_stat_row.query_hash = fspace_hash_var AND
				aqo_query_stat = aqo_query_stat_row;
        END LOOP;
      END IF;
    END LOOP;
  END LOOP;
END;
$$ LANGUAGE plpgsql;

DROP FUNCTION public.top_time_queries;
DROP FUNCTION public.aqo_drop;
DROP FUNCTION public.clean_aqo_data;
DROP FUNCTION public.show_cardinality_errors;
DROP FUNCTION array_mse;
DROP FUNCTION array_avg;
DROP FUNCTION public.aqo_ne_queries; -- Not needed anymore due to changing in the logic
DROP FUNCTION public.aqo_clear_hist; -- Should be renamed and reworked

--
-- Show execution time of queries, for which AQO has statistics.
-- controlled - show stat on executions where AQO was used for cardinality
-- estimations, or not used (controlled = false).
-- Last case is possible in disabled mode with aqo.force_collect_stat = 'on'.
--
CREATE OR REPLACE FUNCTION public.aqo_execution_time(controlled boolean)
RETURNS TABLE(num bigint, id bigint, fshash bigint, exec_time float, nexecs bigint)
AS $$
BEGIN
IF (controlled) THEN
  -- Show a query execution time made with AQO support for the planner
  -- cardinality estimations. Here we return result of last execution.
  RETURN QUERY
    SELECT
      row_number() OVER (ORDER BY (exectime, queryid, fs_hash) DESC) AS nn,
       queryid, fs_hash, exectime, execs
    FROM (
    SELECT
      aq.query_hash AS queryid,
      aq.fspace_hash AS fs_hash,
      execution_time_with_aqo[array_length(execution_time_with_aqo, 1)] AS exectime,
      executions_with_aqo AS execs
    FROM public.aqo_queries aq JOIN public.aqo_query_stat aqs
    ON aq.query_hash = aqs.query_hash
    WHERE TRUE = ANY (SELECT unnest(execution_time_with_aqo) IS NOT NULL)
    ) AS q1
    ORDER BY nn ASC;

ELSE
  -- Show a query execution time made without any AQO advise.
  -- Return an average value across all executions.
  RETURN QUERY
    SELECT
      row_number() OVER (ORDER BY (exectime, queryid, fs_hash) DESC) AS nn,
      queryid, fs_hash, exectime, execs
    FROM (
      SELECT
        aq.query_hash AS queryid,
        aq.fspace_hash AS fs_hash,
        (SELECT AVG(t) FROM unnest(execution_time_without_aqo) t) AS exectime,
        executions_without_aqo AS execs
      FROM public.aqo_queries aq JOIN public.aqo_query_stat aqs
      ON aq.query_hash = aqs.query_hash
      WHERE TRUE = ANY (SELECT unnest(execution_time_without_aqo) IS NOT NULL)
      ) AS q1
    ORDER BY (nn) ASC;
END IF;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION public.aqo_execution_time(boolean) IS
'Get execution time of queries. If controlled = true (AQO could advise cardinality estimations), show time of last execution attempt. Another case (AQO not used), return an average value of execution time across all known executions.';

--
-- Remove all information about a query class from AQO storage.
--
CREATE OR REPLACE FUNCTION public.aqo_drop_class(queryid bigint)
RETURNS integer AS $$
DECLARE
  fs bigint;
  num integer;
BEGIN
  IF (queryid = 0) THEN
    raise EXCEPTION '[AQO] Cannot remove basic class %.', queryid;
  END IF;

  SELECT fspace_hash FROM public.aqo_queries WHERE (query_hash = queryid) INTO fs;

  IF (fs IS NULL) THEN
    raise WARNING '[AQO] Nothing to remove for the class %.', queryid;
    RETURN 0;
  END IF;

  IF (fs <> queryid) THEN
    raise WARNING '[AQO] Removing query class has non-generic feature space value: id = %, fs = %.', queryid, fs;
  END IF;

  SELECT count(*) FROM public.aqo_data WHERE fspace_hash = fs INTO num;

  /*
   * Remove the only from aqo_queries table. All other data will be removed by
   * CASCADE deletion.
   */
  DELETE FROM public.aqo_queries WHERE query_hash = queryid;
  RETURN num;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION public.aqo_drop_class(bigint) IS
'Remove info about an query class from AQO ML knowledge base.';

--
-- Remove unneeded rows from the AQO ML storage.
-- For common feature space, remove rows from aqo_data only.
-- For custom feature space - remove all rows related to the space from all AQO
-- tables even if only one oid for one feature subspace of the space is illegal.
-- Returns number of deleted rows from aqo_queries and aqo_data tables.
--
CREATE OR REPLACE FUNCTION public.aqo_cleanup(OUT nfs integer, OUT nfss integer)
AS $$
DECLARE
  fs bigint;
  fss integer;
BEGIN
  -- Save current number of rows
  SELECT count(*) FROM aqo_queries INTO nfs;
  SELECT count(*) FROM aqo_data INTO nfss;

  FOR fs,fss IN SELECT q1.fs,q1.fss FROM (
                     SELECT fspace_hash fs, fsspace_hash fss, unnest(oids) AS reloid
                     FROM aqo_data) AS q1
                     WHERE q1.reloid NOT IN (SELECT oid FROM pg_class)
                     GROUP BY (q1.fs,q1.fss)
  LOOP
    IF (fs = 0) THEN
      DELETE FROM aqo_data WHERE fsspace_hash = fss;
      continue;
    END IF;

    -- Remove ALL feature space if one of oids isn't exists
    DELETE FROM aqo_queries WHERE fspace_hash = fs;
  END LOOP;

  -- Calculate difference with previous state of knowledge base
  nfs := nfs - (SELECT count(*) FROM aqo_queries);
  nfss := nfss - (SELECT count(*) FROM aqo_data);
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION public.aqo_cleanup() IS
'Remove unneeded rows from the AQO ML storage';

--
-- Get cardinality error of queries the last time they were executed.
-- IN:
-- controlled - show queries executed under a control of AQO (true);
-- executed without an AQO control, but AQO has a stat on the query (false).
--
-- OUT:
-- num - sequental number. Smaller number corresponds to higher error.
-- id - ID of a query.
-- fshash - feature space. Usually equal to zero or ID.
-- error - AQO error that calculated on plan nodes of the query.
-- nexecs - number of executions of queries associated with this ID.
--
CREATE OR REPLACE FUNCTION public.aqo_cardinality_error(controlled boolean)
RETURNS TABLE(num bigint, id bigint, fshash bigint, error float, nexecs bigint)
AS $$
BEGIN
IF (controlled) THEN
  RETURN QUERY
    SELECT
      row_number() OVER (ORDER BY (cerror, query_id, fs_hash) DESC) AS nn,
      query_id, fs_hash, cerror, execs
    FROM (
    SELECT
      aq.query_hash AS query_id,
      aq.fspace_hash AS fs_hash,
      cardinality_error_with_aqo[array_length(cardinality_error_with_aqo, 1)] AS cerror,
      executions_with_aqo AS execs
    FROM public.aqo_queries aq JOIN public.aqo_query_stat aqs
    ON aq.query_hash = aqs.query_hash
    WHERE TRUE = ANY (SELECT unnest(cardinality_error_with_aqo) IS NOT NULL)
    ) AS q1
    ORDER BY nn ASC;
ELSE
  RETURN QUERY
    SELECT
      row_number() OVER (ORDER BY (cerror, query_id, fs_hash) DESC) AS nn,
      query_id, fs_hash, cerror, execs
    FROM (
      SELECT
        aq.query_hash AS query_id,
        aq.fspace_hash AS fs_hash,
		(SELECT AVG(t) FROM unnest(cardinality_error_without_aqo) t) AS cerror,
        executions_without_aqo AS execs
      FROM public.aqo_queries aq JOIN public.aqo_query_stat aqs
      ON aq.query_hash = aqs.query_hash
      WHERE TRUE = ANY (SELECT unnest(cardinality_error_without_aqo) IS NOT NULL)
      ) AS q1
    ORDER BY (nn) ASC;
END IF;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION public.aqo_cardinality_error(boolean) IS
'Get cardinality error of queries the last time they were executed. Order queries according to an error value.';

--
-- Remove all learning data for query with given ID.
-- Can be used in the case when user don't want to drop preferences and
-- accumulated statistics on a query class, but tries to re-learn AQO on this
-- class.
-- Returns a number of deleted rows in the aqo_data table.
--
CREATE OR REPLACE FUNCTION public.aqo_reset_query(queryid bigint)
RETURNS integer AS $$
DECLARE
  num integer;
  fs  bigint;
BEGIN
  IF (queryid = 0) THEN
    raise WARNING '[AQO] Reset common feature space.'
  END IF;

  SELECT fspace_hash FROM public.aqo_queries WHERE query_hash = queryid INTO fs;
  SELECT count(*) FROM public.aqo_data WHERE fspace_hash = fs INTO num;
  DELETE FROM public.aqo_data WHERE fspace_hash = fs;
  RETURN num;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION public.aqo_reset_query(bigint) IS
'Remove from AQO storage only learning data for given QueryId.';
