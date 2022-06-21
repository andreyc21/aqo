/* contrib/aqo/aqo--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION aqo UPDATE TO '1.5'" to load this file. \quit

DROP FUNCTION array_mse;
DROP FUNCTION array_avg;
DROP FUNCTION public.aqo_clear_hist; -- Should be renamed and reworked
DROP FUNCTION public.aqo_disable_query;
DROP FUNCTION public.aqo_drop;
DROP FUNCTION public.aqo_enable_query;
DROP FUNCTION public.aqo_ne_queries; -- Not needed anymore due to changing in the logic
DROP FUNCTION public.aqo_status;
DROP FUNCTION public.clean_aqo_data;
DROP FUNCTION public.show_cardinality_errors;
DROP FUNCTION public.top_time_queries;

DROP TABLE public.aqo_data CASCADE;
DROP TABLE public.aqo_queries CASCADE;
DROP TABLE public.aqo_query_texts CASCADE;
DROP TABLE public.aqo_query_stat CASCADE;

CREATE FUNCTION aqo_queries (
	OUT queryid		bigint,
  OUT fspace_hash		bigint,
	OUT learn_aqo		boolean,
	OUT use_aqo			boolean,
	OUT auto_tuning		boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'aqo_queries'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
CREATE FUNCTION aqo_queries_remove(queryid bigint) RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION aqo_query_texts(OUT queryid bigint, OUT query_text text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'aqo_query_texts'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
CREATE FUNCTION aqo_qtexts_remove(queryid bigint) RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- Remove all records in the AQO storage.
-- Return number of rows removed.
--
CREATE FUNCTION aqo_reset() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C PARALLEL SAFE;
COMMENT ON FUNCTION aqo_reset() IS
'Reset all data gathered by AQO';

CREATE FUNCTION aqo_query_stat(
  OUT queryid						bigint,
  OUT execution_time_with_aqo		double precision[],
  OUT execution_time_without_aqo	double precision[],
  OUT planning_time_with_aqo		double precision[],
  OUT planning_time_without_aqo		double precision[],
  OUT cardinality_error_with_aqo	double precision[],
  OUT cardinality_error_without_aqo	double precision[],
  OUT executions_with_aqo			bigint,
  OUT executions_without_aqo		bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'aqo_query_stat'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE FUNCTION aqo_data(
  OUT fs			bigint,
  OUT fss			integer,
  OUT nfeatures		integer,
  OUT features		double precision[][],
  OUT targets		double precision[],
  OUT reliability	double precision[],
  OUT oids			integer[]
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'aqo_data'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
CREATE FUNCTION aqo_data_remove(fs bigint, fss int) RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW aqo_query_stat AS SELECT * FROM aqo_query_stat();
CREATE VIEW aqo_query_texts AS SELECT * FROM aqo_query_texts();
CREATE VIEW aqo_data AS SELECT * FROM aqo_data();
CREATE VIEW aqo_queries AS SELECT * FROM aqo_queries();

CREATE FUNCTION aqo_stat_remove(fs bigint) RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT PARALLEL SAFE;

-- INSERT INTO aqo_queries VALUES (0, false, false, 0, false);
-- a virtual query for COMMON feature space

--CREATE TRIGGER aqo_queries_invalidate AFTER UPDATE OR DELETE OR TRUNCATE
--	ON aqo_queries FOR EACH STATEMENT
--	EXECUTE PROCEDURE invalidate_deactivated_queries_cache();

--
-- Show execution time of queries, for which AQO has statistics.
-- controlled - show stat on executions where AQO was used for cardinality
-- estimations, or not used (controlled = false).
-- Last case is possible in disabled mode with aqo.force_collect_stat = 'on'.
--
CREATE OR REPLACE FUNCTION aqo_execution_time(controlled boolean)
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
      aq.queryid AS queryid,
      aq.fspace_hash AS fs_hash,
      execution_time_with_aqo[array_length(execution_time_with_aqo, 1)] AS exectime,
      executions_with_aqo AS execs
    FROM aqo_queries aq JOIN aqo_query_stat aqs
    ON aq.queryid = aqs.queryid
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
        aq.queryid AS queryid,
        aq.fspace_hash AS fs_hash,
        (SELECT AVG(t) FROM unnest(execution_time_without_aqo) t) AS exectime,
        executions_without_aqo AS execs
      FROM aqo_queries aq JOIN aqo_query_stat aqs
      ON aq.queryid = aqs.queryid
      WHERE TRUE = ANY (SELECT unnest(execution_time_without_aqo) IS NOT NULL)
      ) AS q1
    ORDER BY (nn) ASC;
END IF;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION aqo_execution_time(boolean) IS
'Get execution time of queries. If controlled = true (AQO could advise cardinality estimations), show time of last execution attempt. Another case (AQO not used), return an average value of execution time across all known executions.';

--
-- Remove all information about a query class from AQO storage.
--
CREATE OR REPLACE FUNCTION aqo_drop_class(queryid_rm bigint)
RETURNS integer AS $$
DECLARE
  lfs bigint;
  num integer;
BEGIN
  IF (queryid_rm = 0) THEN
    raise EXCEPTION '[AQO] Cannot remove basic class %.', queryid_rm;
  END IF;

  SELECT fspace_hash FROM aqo_queries WHERE (queryid = queryid_rm) INTO lfs;

  IF (lfs IS NULL) THEN
    raise WARNING '[AQO] Nothing to remove for the class %.', queryid_rm;
    RETURN 0;
  END IF;

  IF (lfs <> queryid_rm) THEN
    raise WARNING '[AQO] Removing query class has non-generic feature space value: id = %, fs = %.', queryid_rm, fs;
  END IF;

  SELECT count(*) FROM aqo_data WHERE fs = lfs INTO num;

  PERFORM aqo_queries_remove(queryid_rm);
  PERFORM aqo_stat_remove(queryid_rm);
  PERFORM aqo_qtexts_remove(queryid_rm);
  PERFORM aqo_data_remove(lfs, NULL);
  RETURN num;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION aqo_drop_class(bigint) IS
'Remove info about an query class from AQO ML knowledge base.';

--
-- Remove unneeded rows from the AQO ML storage.
-- For common feature space, remove rows from aqo_data only.
-- For custom feature space - remove all rows related to the space from all AQO
-- tables even if only one oid for one feature subspace of the space is illegal.
-- Returns number of deleted rows from aqo_queries and aqo_data tables.
--
CREATE OR REPLACE FUNCTION aqo_cleanup(OUT nfs integer, OUT nfss integer)
AS $$
DECLARE
  lfs bigint;
  lfss integer;
BEGIN
  -- Save current number of rows
  SELECT count(*) FROM aqo_queries INTO nfs;
  SELECT count(*) FROM aqo_data INTO nfss;

  FOR lfs,lfss IN SELECT q1.fs,q1.fss FROM (
                     SELECT fs, fss, unnest(oids) AS reloid
                     FROM aqo_data) AS q1
                     WHERE q1.reloid NOT IN (SELECT oid FROM pg_class)
                     GROUP BY (q1.fs,q1.fss)
  LOOP
--    IF (fs = 0) THEN
--      DELETE FROM aqo_data WHERE fsspace_hash = fss;
--      continue;
--    END IF;

    -- Remove ALL feature space if one of oids isn't exists
    PERFORM aqo_queries_remove(lfs);
    PERFORM aqo_stat_remove(lfs);
    PERFORM aqo_qtexts_remove(lfs);
	PERFORM aqo_data_remove(lfs, NULL);
  END LOOP;

  -- Calculate difference with previous state of knowledge base
  nfs := nfs - (SELECT count(*) FROM aqo_queries);
  nfss := nfss - (SELECT count(*) FROM aqo_data);
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION aqo_cleanup() IS
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
CREATE OR REPLACE FUNCTION aqo_cardinality_error(controlled boolean)
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
      aq.queryid AS query_id,
      aq.fspace_hash AS fs_hash,
      cardinality_error_with_aqo[array_length(cardinality_error_with_aqo, 1)] AS cerror,
      executions_with_aqo AS execs
    FROM aqo_queries aq JOIN aqo_query_stat aqs
    ON aq.queryid = aqs.queryid
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
        aq.queryid AS query_id,
        aq.fspace_hash AS fs_hash,
		(SELECT AVG(t) FROM unnest(cardinality_error_without_aqo) t) AS cerror,
        executions_without_aqo AS execs
      FROM aqo_queries aq JOIN aqo_query_stat aqs
      ON aq.queryid = aqs.queryid
      WHERE TRUE = ANY (SELECT unnest(cardinality_error_without_aqo) IS NOT NULL)
      ) AS q1
    ORDER BY (nn) ASC;
END IF;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION aqo_cardinality_error(boolean) IS
'Get cardinality error of queries the last time they were executed. Order queries according to an error value.';

--
-- Remove all learning data for query with given ID.
-- Can be used in the case when user don't want to drop preferences and
-- accumulated statistics on a query class, but tries to re-learn AQO on this
-- class.
-- Returns a number of deleted rows in the aqo_data table.
--
CREATE OR REPLACE FUNCTION aqo_reset_query(queryid_res bigint)
RETURNS integer AS $$
DECLARE
  num integer;
  fs  bigint;
BEGIN
  IF (queryid_res = 0) THEN
    raise WARNING '[AQO] Reset common feature space.'
  END IF;

  SELECT fspace_hash FROM aqo_queries WHERE queryid = queryid_res INTO fs;
  SELECT count(*) FROM aqo_data WHERE fspace_hash = fs INTO num;
  DELETE FROM aqo_data WHERE fspace_hash = fs;
  RETURN num;
END;
$$ LANGUAGE plpgsql;

COMMENT ON FUNCTION aqo_reset_query(bigint) IS
'Remove from AQO storage only learning data for given QueryId.';

CREATE FUNCTION aqo_status(hash bigint)
RETURNS TABLE (
	"learn"			BOOL,
	"use aqo"		BOOL,
	"auto tune"		BOOL,
	"fspace hash"	bigINT,
	"t_naqo"		TEXT,
	"err_naqo"		TEXT,
	"iters"			BIGINT,
	"t_aqo"			TEXT,
	"err_aqo"		TEXT,
	"iters_aqo"		BIGINT
) AS $$
SELECT	learn_aqo,use_aqo,auto_tuning,fspace_hash,
		to_char(execution_time_without_aqo[n4],'9.99EEEE'),
		to_char(cardinality_error_without_aqo[n2],'9.99EEEE'),
		executions_without_aqo,
		to_char(execution_time_with_aqo[n3],'9.99EEEE'),
		to_char(cardinality_error_with_aqo[n1],'9.99EEEE'),
		executions_with_aqo
FROM aqo_queries aq, aqo_query_stat aqs,
	(SELECT array_length(n1,1) AS n1, array_length(n2,1) AS n2,
		array_length(n3,1) AS n3, array_length(n4,1) AS n4
	FROM
		(SELECT cardinality_error_with_aqo		AS n1,
				cardinality_error_without_aqo	AS n2,
				execution_time_with_aqo			AS n3,
				execution_time_without_aqo		AS n4
		FROM aqo_query_stat aqs WHERE
			aqs.queryid = $1) AS al) AS q
WHERE (aqs.queryid = aq.queryid) AND
	aqs.queryid = $1;
$$ LANGUAGE SQL;

/* CREATE FUNCTION aqo_enable_query(hash bigint)
RETURNS VOID AS $$
UPDATE aqo_queries SET
	learn_aqo = 'true',
	use_aqo = 'true'
	WHERE queryid = $1;
$$ LANGUAGE SQL; 

CREATE FUNCTION aqo_disable_query(hash bigint)
RETURNS VOID AS $$
UPDATE aqo_queries SET
	learn_aqo = 'false',
	use_aqo = 'false',
	auto_tuning = 'false'
	WHERE queryid = $1;
$$ LANGUAGE SQL;
*/

CREATE FUNCTION aqo_enable_query(hash bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'aqo_enable_query'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION aqo_disable_query(hash bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'aqo_enable_query'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION aqo_queries_update(learn_aqo int, use_aqo int, auto_tuning int)
RETURNS void
AS 'MODULE_PATHNAME', 'aqo_queries_update'
LANGUAGE C STRICT VOLATILE;