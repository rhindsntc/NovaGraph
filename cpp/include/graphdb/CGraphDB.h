#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GraphDBHandle GraphDBHandle;
typedef struct GraphDBRequest GraphDBRequest;
typedef struct GraphDBSnapshot GraphDBSnapshot;

typedef struct GraphDBString {
  char *data;
  unsigned long long len;
} GraphDBString;

// On failure, caller frees error with graphdb_string_free. On success error is empty.
typedef struct GraphDBOpenResult {
  GraphDBHandle *handle;
  GraphDBString error;
} GraphDBOpenResult;
GraphDBOpenResult graphdb_open_result(const char *database_dir, long long hot_ttl_ms,
                                      long long sweep_interval_ms);

// Zero depth/results/expanded edges are valid restrictions; byte/timeout/batch limits must be positive.
typedef struct GraphDBQueryOptions {
  unsigned long long max_depth, max_results, max_expanded_edges;
  unsigned long long working_bytes, result_bytes, max_statements, batch_bytes, timeout_ms;
} GraphDBQueryOptions;
GraphDBQueryOptions graphdb_default_query_options(void);
GraphDBOpenResult graphdb_open_with_options_result(const char *database_dir, long long hot_ttl_ms,
    long long sweep_interval_ms, const GraphDBQueryOptions *options);

// v2 buffers are UTF-8 with explicit byte lengths (no terminator required).
// Path NULs are rejected; query/property NULs are preserved. NULL + 0 is allowed
// only for absent parameters. Options may lower the database safety ceilings.
// Responses: {schemaVersion:2,ok:true,data:...,receipt?:{transactionId,committedLSN}}
// or {schemaVersion:2,ok:false,error:{code,message,context}}. Property scalars are
// {type:"null"|"bool"|"int"|"double"|"string",value:...}. Free exactly once with
// graphdb_string_free, including errors. NULL/0 response means allocation failed.
GraphDBOpenResult graphdb_open_v2(const char *database_dir, unsigned long long path_len,
    long long hot_ttl_ms, long long sweep_interval_ms, const GraphDBQueryOptions *options);
GraphDBString graphdb_execute_query_v2(GraphDBHandle *handle,
    const char *query, unsigned long long query_len,
    const char *params_json, unsigned long long params_len, const GraphDBQueryOptions *options);

// Retain one reference for each independently owned handle. Every call requires
// a live reference; retain before handing it to another thread. Release is best
// effort cleanup, not an acknowledged close. Legacy graphdb_close closes/releases.
void graphdb_retain(GraphDBHandle *handle);
void graphdb_release(GraphDBHandle *handle);
// Requests are single-use, independently owned and safe to cancel without the
// operation gate. Deadline starts at creation (1..5000 ms); invalid/OOM => NULL.
GraphDBRequest *graphdb_request_create(unsigned long long timeout_ms);
void graphdb_request_retain(GraphDBRequest *request);
void graphdb_request_release(GraphDBRequest *request);
void graphdb_request_cancel(GraphDBRequest *request);
int graphdb_request_is_cancelled(GraphDBRequest *request);
int graphdb_request_is_expired(GraphDBRequest *request);
GraphDBString graphdb_execute_request_v2(GraphDBHandle *handle,
    const char *query, unsigned long long query_len,
    const char *params_json, unsigned long long params_len,
    const GraphDBQueryOptions *options, GraphDBRequest *request);
// Stops admission, cancels/drains operations and reports checkpoint errors.
// Does not release the caller's reference. Repeated calls return the same final
// status. A drain timeout leaves admission closed and can be retried. Cancellation
// cannot preempt user snapshot callback code or an in-progress OS IO call.
GraphDBString graphdb_close_v2(GraphDBHandle *handle, GraphDBRequest *request);
GraphDBString graphdb_suspend_v2(GraphDBHandle *handle, GraphDBRequest *request);
GraphDBString graphdb_resume_v2(GraphDBHandle *handle, GraphDBRequest *request);
// Maintenance checkpoint, matching legacy checkpoint and close/suspend. Request
// cancellation/deadline is checked while waiting and before work starts. Once
// started, the durability protocol finishes and reports its storage outcome.
// Query workspace/result limits do not apply; explicit NGQL checkpoint keeps them.
GraphDBString graphdb_checkpoint_v2(GraphDBHandle *handle, GraphDBRequest *request);
GraphDBString graphdb_trim_memory_v2(GraphDBHandle *handle, unsigned long long target_bytes, GraphDBRequest *request);
// Callback is synchronous and borrows the snapshot on this thread only. Do not
// retain it or use the database reentrantly. Options/deadline/budgets cover the
// entire callback and all its queries; only read/explain programs are permitted.
typedef void (*GraphDBSnapshotBody)(GraphDBSnapshot *snapshot, void *context);
GraphDBString graphdb_read_snapshot_v2(GraphDBHandle *handle, const GraphDBQueryOptions *options,
    GraphDBRequest *request, GraphDBSnapshotBody body, void *context);
GraphDBString graphdb_snapshot_query_v2(GraphDBSnapshot *snapshot,
    const char *query, unsigned long long query_len,
    const char *params_json, unsigned long long params_len);


// Opens a CURRENT-root database; holds an exclusive canonical-directory lock.
// Legacy opens return NULL on error; graphdb_open_result preserves the typed error.
GraphDBHandle *graphdb_open(const char *database_dir);
GraphDBHandle *graphdb_open_with_config(const char *database_dir, long long hot_ttl_ms,
                                        long long sweep_interval_ms);

// Backward-compatible alias retained for early Swift wrapper experiments.
GraphDBHandle *graphdb_open_memory(const char *database_dir);

void graphdb_close(GraphDBHandle *handle);

// Executes NGQL. Caller owns the returned GraphDBString and must call graphdb_string_free.
GraphDBString graphdb_execute_query(GraphDBHandle *handle, const char *query);
GraphDBString graphdb_execute_query_with_params(GraphDBHandle *handle, const char *query,
                                                const char *params_json);
GraphDBString graphdb_sweep_once(GraphDBHandle *handle);
GraphDBString graphdb_checkpoint(GraphDBHandle *handle);
GraphDBString graphdb_trim_memory(GraphDBHandle *handle);
GraphDBString graphdb_collect_garbage(GraphDBHandle *handle, unsigned long long max_files);
// Read one bounded inspection page (kind: 0 nodes, 1 edges, 2 declared property indexes).
// Limit 1...100. Empty cursor starts a scan; returned opaque cursor belongs to this
// database and kind and expires after any commit. Same v2 scalar/error schema and
// request ownership as execute_request_v2. Caller frees the response exactly once.
GraphDBString graphdb_inspect_v2(GraphDBHandle *handle, unsigned int kind,
    const char *cursor, unsigned long long cursor_len, unsigned int limit,
    const GraphDBQueryOptions *options, GraphDBRequest *request);

// Offline operations: inspect, verify, rebuild-indexes, backup, restore, migrate-v1.
// Destination is required only for backup, restore and migrate-v1.
GraphDBString graphdb_maintenance(const char *command, const char *source, const char *destination);
// Capture a consistent backup while this handle owns the database operation gate.
GraphDBString graphdb_backup(GraphDBHandle *handle, const char *destination);
GraphDBString graphdb_rebuild_indexes(GraphDBHandle *handle);
void graphdb_string_free(GraphDBString s);

#ifdef __cplusplus
}
#endif
