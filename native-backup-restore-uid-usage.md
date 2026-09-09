# Native backup and restore UIDs

Draft usage for the implementation in this worktree. End-to-end verification is still incomplete; this document does not announce released support. See the [implementation progress](native-backup-restore-idempotency-progress.md).

A UID identifies one native full backup, incremental backup, or restore. Keep both the UID and the exact original SQL text when retrying a submission. While its operation record exists, the server returns the original operation ID for identical SQL and rejects different SQL under that UID.

## Submit from a SQL editor

```sql
BACKUP `daily` WITH (uid = 'daily:full:run-42');
BACKUP `daily` INCREMENTAL WITH (uid = 'daily:incremental:run-43');
RESTORE `daily` WITH (uid = 'daily:restore:run-44');
```

Send each statement separately, outside an explicit transaction. Other statements, scripts, batches, bound parameters, and unsupported execution modes cannot use this native UID contract.

## Python Query SDK

```python
import ydb

sql = "BACKUP `daily`;"
uid = "daily:full:run-42"

with ydb.QuerySessionPool(driver) as pool:
    results = pool.execute_with_retries(sql, uid=uid)
    raw_id = results[0].rows[0]["operation_id"]
    operation_id = f"ydb://fullbackup/14?id={raw_id}"
```

Persist `sql` and `uid` before submission. If the response is lost or the worker restarts, submit those same values again and save the recovered operation ID. The UID can also be passed as `uid=...` through the asynchronous query session/pool APIs. An SDK retry-policy flag such as `idempotent=True` does not supply a server UID.

## C++ Query SDK

```cpp
const std::string sql = "BACKUP `daily`;";
const auto settings = NYdb::NQuery::TExecuteQuerySettings()
    .Uid("daily:full:run-42");

const auto result = queryClient.ExecuteQuery(
    sql, NYdb::NQuery::TTxControl::NoTx(), settings).GetValueSync();
if (!result.IsSuccess()) {
    throw std::runtime_error(result.GetIssues().ToString());
}
NYdb::TResultSetParser parser(result.GetResultSet(0));
if (parser.TryNextRow()) {
    const auto rawId = parser.ColumnParser("operation_id").GetUtf8();
    // Persist the full-backup operation ID: "ydb://fullbackup/14?id=" + rawId.
}
```

Use the same settings and original SQL for submission recovery. Completion of the Query Service request returns a native operation identity; use the operation API to inspect its outcome.

## Poll and forget

The native `operation_id` result column contains the numeric SchemeShard ID. Existing operation APIs use these forms:

| Statement | Operation ID |
| --- | --- |
| Full `BACKUP` | `ydb://fullbackup/14?id=N` |
| Incremental `BACKUP` | `ydb://incbackup/11?id=N` |
| `RESTORE` | `ydb://restore/12?id=N` |

For example, with the usual CLI endpoint/database settings:

```bash
ydb operation get 'ydb://fullbackup/14?id=123'
ydb operation forget 'ydb://fullbackup/14?id=123'
```

Current CancelOperation returns UNSUPPORTED for these three native families.

Wait for the operation to be ready and check its terminal status. Stop submission retries before Forget. Eligible Forget/cleanup deletes the operation, its UID, and stored SQL together. After deletion, a delayed retry or intentional reuse of the UID can create a new operation. Successful or failed completion alone keeps the UID reserved. There is no fixed retention period or independent receipt after deletion.

The current native record cleanup paths are:

| Operation family | Record removal |
| --- | --- |
| Full backup | Eligible Forget |
| Incremental backup | Eligible Forget |
| Restore | Eligible Forget or backup-collection drop cleanup |

These paths have no timer-based operation-record expiry. Database/tablet deletion also ends this contract. Deleting snapshot tables alone leaves the operation records and their UIDs intact. A controller must coordinate collection deletion as well as Forget with its submission retries.

## Identity rules

- A native UID is case-sensitive and contains 1–256 ASCII bytes from `[A-Za-z0-9_.:-]`. No prefix is required. Omit it for unkeyed execution; an explicitly empty value is invalid.
- Scope is SchemeShard tablet + native operation family + UID. Full backup, incremental backup, and restore have separate namespaces. Existing import/export UID behavior is unchanged.
- Stored operations retain their database binding. The same UID in another resolved database on the same tablet causes a namespace collision. A UID grants no access to another user's operation.
- Equality compares original SQL bytes, including whitespace, comments, quoting, and semicolons. It does not compare normalized SQL or a query plan.
- Request and SQL UIDs may both be supplied only when they match. Moving a UID out of SQL changes the SQL text and therefore conflicts with a retained submission. Choose either placement for new operations and keep it stable during retries.
- Native UID support must be enforced by the target endpoint and SchemeShard. Legacy import/export UID support alone does not establish this contract. The SDK selects `EXEC_MODE_EXECUTE_WITH_UID` automatically for keyed execution. Raw gRPC clients must set both this mode and `uid`; older endpoints reject the unfamiliar mode. KQP forwarding retains a guarded query type, and SchemeShard admission uses a dedicated native protocol. None of these paths falls back to unkeyed execution.

## Raw Query Service request

```json
{
  "exec_mode": "EXEC_MODE_EXECUTE_WITH_UID",
  "query_content": {
    "syntax": "SYNTAX_YQL_V1",
    "text": "BACKUP `daily`;"
  },
  "uid": "daily:full:run-42"
}
```

Supply the usual database/authentication gRPC metadata. Omit transaction control for NoTx execution. Setting `uid` with ordinary `EXEC_MODE_EXECUTE`, or selecting the guarded mode without `uid`, returns BAD_REQUEST. A supported server still validates the statement and UID before admitting work. This example describes the worktree protocol and does not imply released support.
