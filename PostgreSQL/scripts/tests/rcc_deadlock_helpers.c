/* Test-only dependency injection; loaded through session-local SQL functions. */
#include "postgres.h"
#include "fmgr.h"
#include "access/rcc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(rcc_test_depend);

Datum
rcc_test_depend(PG_FUNCTION_ARGS)
{
	RecordReadDependency(PG_GETARG_TRANSACTIONID(0), GetTopTransactionId());
	PG_RETURN_VOID();
}
