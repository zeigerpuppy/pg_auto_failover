/*-------------------------------------------------------------------------
 *
 * src/monitor/archiver_metadata.c
 *
 * Implementation of functions related to archiver metadata.
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "health_check.h"
#include "metadata.h"
#include "archiver_metadata.h"
#include "notifications.h"

#include "access/htup_details.h"
#include "access/xlogdefs.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "storage/lockdefs.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


Datum AutoFailoverArchiverGetDatum(FunctionCallInfo fcinfo,
								   AutoFailoverArchiver *archiver);

/*
 * GetFormation returns an AutoFailoverFormation structure with the formationId
 * and its kind, when the formation has already been created, or NULL
 * otherwise.
 */
AutoFailoverArchiver *
GetArchiver(int nodeId)
{
	AutoFailoverArchiver *archiver = NULL;
	MemoryContext callerContext = CurrentMemoryContext;

	Oid argTypes[] = {
		INT8OID /* nodeid */
	};

	Datum argValues[] = {
		Int32GetDatum(nodeId), /* nodeId */
	};
	const int argCount = sizeof(argValues) / sizeof(argValues[0]);

	const char *selectQuery =
		"SELECT * FROM " AUTO_FAILOVER_ARCHIVER_TABLE
		" WHERE nodeId = $1";

	SPI_connect();

	int spiStatus = SPI_execute_with_args(selectQuery,
										  argCount, argTypes, argValues,
										  NULL, false, 1);
	if (spiStatus != SPI_OK_SELECT)
	{
		elog(ERROR, "could not select from " AUTO_FAILOVER_ARCHIVER_TABLE);
	}

	if (SPI_processed > 0)
	{
		MemoryContext spiContext = MemoryContextSwitchTo(callerContext);
		TupleDesc tupleDescriptor = SPI_tuptable->tupdesc;
		HeapTuple heapTuple = SPI_tuptable->vals[0];
		bool isNull = false;

		Datum nodeId =
			heap_getattr(heapTuple, Anum_pgautofailover_archiver_nodeid,
						 tupleDescriptor, &isNull);
		Datum nodeName =
			heap_getattr(heapTuple, Anum_pgautofailover_archiver_nodename,
						 tupleDescriptor, &isNull);
		Datum nodeHost =
			heap_getattr(heapTuple, Anum_pgautofailover_archiver_nodehost,
						 tupleDescriptor, &isNull);

		archiver =
			(AutoFailoverArchiver *) palloc0(sizeof(AutoFailoverArchiver));

		archiver->nodeId = DatumGetInt64(nodeId);
		archiver->nodeName = TextDatumGetCString(nodeName);
		archiver->nodeHost = TextDatumGetCString(nodeHost);

		MemoryContextSwitchTo(spiContext);
	}
	else
	{
		archiver = NULL;
	}

	SPI_finish();

	return archiver;
}


/*
 * AddArchiver adds given archiver to the pgautofailover.archiver table.
 *
 * It returns nothing: either the INSERT happened and we have the exact same
 * information as given in the table, or it failed and we raise an exception
 * here.
 */
int
AddArchiver(const char *nodeName, const char *nodeHost)
{
	Oid argTypes[] = {
		TEXTOID, /* nodename */
		TEXTOID  /* nodehost */
	};

	Datum argValues[] = {
		nodeName == NULL
		? (Datum) 0
		: CStringGetTextDatum(nodeName), /* nodename */
		CStringGetTextDatum(nodeHost)    /* nodehost */
	};

	const char argNulls[] = {
		nodeName == NULL ? 'n' : ' ', /* nodename */
		' '                           /* nodehost */
	};

	const int argCount = sizeof(argValues) / sizeof(argValues[0]);

	int nodeId = 0;

	const char *insertQuery =
		"WITH seq(nodeid) AS "
		"(SELECT nextval('pgautofailover.archiver_nodeid_seq'::regclass)) "
		"INSERT INTO " AUTO_FAILOVER_ARCHIVER_TABLE
		" (nodename, nodehost) "
		"SELECT "
		"case when $1 is null then format('archiver_%s', seq.nodeid) else $1 end"
		", $2 "
		"FROM seq "
		"RETURNING nodeid";

	SPI_connect();

	int spiStatus = SPI_execute_with_args(insertQuery, argCount,
										  argTypes, argValues, argNulls,
										  false, 0);

	if (spiStatus == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
	{
		bool isNull = false;

		Datum nodeIdDatum = SPI_getbinval(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc,
										  1,
										  &isNull);

		nodeId = DatumGetInt32(nodeIdDatum);
	}
	else
	{
		elog(ERROR, "could not insert into " AUTO_FAILOVER_ARCHIVER_TABLE);
	}

	SPI_finish();

	return nodeId;
}


/*
 * RemoveArchiver removes an archiver node from the monitor.
 *
 * We use SPI to automatically handle triggers, function calls, etc.
 */
void
RemoveArchiver(AutoFailoverArchiver *archiver)
{
	Oid argTypes[] = {
		INT4OID  /* nodeId */
	};

	Datum argValues[] = {
		Int32GetDatum(archiver->nodeId) /* nodeId */
	};
	const int argCount = sizeof(argValues) / sizeof(argValues[0]);

	const char *deleteQuery =
		"DELETE FROM " AUTO_FAILOVER_ARCHIVER_TABLE
		" WHERE nodeid = $1";

	SPI_connect();

	int spiStatus = SPI_execute_with_args(deleteQuery, argCount,
										  argTypes, argValues, NULL,
										  false, 0);

	if (spiStatus != SPI_OK_DELETE)
	{
		elog(ERROR, "could not delete from " AUTO_FAILOVER_ARCHIVER_TABLE);
	}

	SPI_finish();
}


/*
 * AutoFailoverFormationGetDatum prepares a Datum from given formation.
 * Caller is expected to provide fcinfo structure that contains compatible
 * call result type.
 */
Datum
AutoFailoverArchiverGetDatum(FunctionCallInfo fcinfo,
							 AutoFailoverArchiver *archiver)
{
	TupleDesc resultDescriptor = NULL;

	Datum values[3];
	bool isNulls[3];

	if (archiver == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the given archiver must not be NULL")));
	}

	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[0] = Int32GetDatum(archiver->nodeId);
	values[1] = CStringGetTextDatum(archiver->nodeName);
	values[2] = CStringGetTextDatum(archiver->nodeHost);

	TypeFuncClass resultTypeClass =
		get_call_result_type(fcinfo, NULL, &resultDescriptor);

	if (resultTypeClass != TYPEFUNC_COMPOSITE)
	{
		ereport(ERROR, (errmsg("return type must be a row type")));
	}

	HeapTuple resultTuple = heap_form_tuple(resultDescriptor, values, isNulls);
	Datum resultDatum = HeapTupleGetDatum(resultTuple);

	PG_RETURN_DATUM(resultDatum);
}