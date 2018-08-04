#include <postgres.h>
#include <access/xact.h>
#include <catalog/namespace.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <access/relscan.h>
#include <access/xact.h>
#include <storage/lmgr.h>
#include <storage/bufmgr.h>
#include <storage/dsm.h>
#include <storage/spin.h>
#include <utils/rel.h>
#include <utils/tqual.h>

#include "params.h"
#include "timer_mock.h"
#include "log.h"
#include "scanner.h"
#include "catalog.h"
#include "compat.h"

typedef struct FormData_bgw_dsm_handle
{
	/* handle is actually a uint32 */
	int64		handle;
} FormData_bgw_dsm_handle;

typedef struct TestParamsWrapper
{
	TestParams	params;
	slock_t		mutex;
} TestParamsWrapper;

static Oid
get_dsm_handle_table_oid()
{
	return get_relname_relid("bgw_dsm_handle_store", get_namespace_oid("public", false));
}

static void
params_register_dsm_handle(dsm_handle handle)
{
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	FormData_bgw_dsm_handle *fd;

	rel = heap_open(get_dsm_handle_table_oid(), RowExclusiveLock);
	scan = heap_beginscan(rel, SnapshotSelf, 0, NULL);
	tuple = heap_copytuple(heap_getnext(scan, ForwardScanDirection));
	fd = (FormData_bgw_dsm_handle *) GETSTRUCT(tuple);
	fd->handle = handle;
	catalog_update(rel, tuple);
	heap_freetuple(tuple);
	heap_endscan(scan);
	heap_close(rel, RowExclusiveLock);
}

static dsm_handle
params_load_dsm_handle()
{
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	FormData_bgw_dsm_handle *fd;
	dsm_handle	handle;

	rel = heap_open(get_dsm_handle_table_oid(), RowExclusiveLock);
	scan = heap_beginscan(rel, SnapshotSelf, 0, NULL);
	tuple = heap_getnext(scan, ForwardScanDirection);
	Assert(tuple != NULL);
	tuple = heap_copytuple(tuple);
	fd = (FormData_bgw_dsm_handle *) GETSTRUCT(tuple);
	handle = fd->handle;
	heap_freetuple(tuple);
	heap_endscan(scan);
	heap_close(rel, RowExclusiveLock);

	return handle;
}

static dsm_handle
params_get_dsm_handle()
{
	static dsm_handle handle = 0;

	if (handle == 0)
		handle = params_load_dsm_handle();

	return handle;
}

static TestParamsWrapper *
params_open_wrapper()
{
	dsm_segment *seg;
	dsm_handle	handle = params_get_dsm_handle();
	TestParamsWrapper *wrapper;

	seg = dsm_find_mapping(handle);
	if (seg == NULL)
	{
#if PG96
		bool		started = IsTransactionState();

		if (!started)
			StartTransactionCommand();
#endif
		seg = dsm_attach(handle);
#if PG96
		dsm_pin_mapping(seg);
		if (!started)
			CommitTransactionCommand();
#endif
	}

	Assert(seg != NULL);

	wrapper = dsm_segment_address(seg);

	Assert(wrapper != NULL);

	return wrapper;
};

static void
params_close_wrapper(TestParamsWrapper *wrapper)
{
	dsm_segment *seg = dsm_find_mapping(params_get_dsm_handle());

	Assert(seg != NULL);
	dsm_detach(seg);
}

TestParams *
params_get()
{
	TestParamsWrapper *wrapper = params_open_wrapper();
	TestParams *res;

	Assert(wrapper != NULL);

	res = palloc(sizeof(TestParams));

	SpinLockAcquire(&wrapper->mutex);

	memcpy(res, &wrapper->params, sizeof(TestParams));

	SpinLockRelease(&wrapper->mutex);

	params_close_wrapper(wrapper);

	return res;
};

void
params_set_time(int64 new_val)
{
	TestParamsWrapper *wrapper = params_open_wrapper();

	Assert(wrapper != NULL);

	SpinLockAcquire(&wrapper->mutex);

	wrapper->params.current_time = new_val;

	SpinLockRelease(&wrapper->mutex);

	params_close_wrapper(wrapper);
}

static void
params_set_mock_wait_returns_immediately(bool new_val)
{
	TestParamsWrapper *wrapper = params_open_wrapper();

	Assert(wrapper != NULL);

	SpinLockAcquire(&wrapper->mutex);

	wrapper->params.mock_wait_returns_immediately = new_val;

	SpinLockRelease(&wrapper->mutex);

	params_close_wrapper(wrapper);
}

TS_FUNCTION_INFO_V1(ts_bgw_params_reset_time);
Datum
ts_bgw_params_reset_time(PG_FUNCTION_ARGS)
{
	params_set_time(0);

	PG_RETURN_VOID();
}

TS_FUNCTION_INFO_V1(ts_bgw_params_mock_wait_returns_immediately);
Datum
ts_bgw_params_mock_wait_returns_immediately(PG_FUNCTION_ARGS)
{
	params_set_mock_wait_returns_immediately(PG_GETARG_BOOL(0));

	PG_RETURN_VOID();
}

TS_FUNCTION_INFO_V1(ts_bgw_params_create);
Datum
ts_bgw_params_create(PG_FUNCTION_ARGS)
{
	dsm_segment *seg = dsm_create(sizeof(TestParamsWrapper), 0);
	TestParamsWrapper *params;

	Assert(seg != NULL);

	params = dsm_segment_address(seg);
	*params = (TestParamsWrapper)
	{
		.params =
		{
			.current_time = 0,
		},
	};
	SpinLockInit(&params->mutex);

	params_register_dsm_handle(dsm_segment_handle(seg));

	dsm_pin_mapping(seg);
	dsm_pin_segment(seg);

	PG_RETURN_VOID();
}

TS_FUNCTION_INFO_V1(ts_bgw_params_destroy);
Datum
ts_bgw_params_destroy(PG_FUNCTION_ARGS)
{
/* no way to unpin in 9.6 so forget it, should only affect tests anyway */
#if PG10
	dsm_unpin_segment(params_get_dsm_handle());
#endif
	PG_RETURN_VOID();
}