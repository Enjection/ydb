# Backup Collection Commands

YQL supports SQL commands for managing [backup collections](../../../concepts/backup-collections.md). These commands allow you to create, manage, and take backups of database tables using a declarative SQL syntax.

For practical usage examples and operational guidance, see the [backup collections operations guide](../../../maintenance/manual/backup-collections.md).

## CREATE BACKUP COLLECTION {#create-backup-collection}

Creates a new backup collection with specified tables and configuration.

### Syntax

```sql
CREATE BACKUP COLLECTION <collection_name>
    ( TABLE <table_path> [, TABLE <table_path>]... )
WITH ( STORAGE = '<storage_backend>'
     [, INCREMENTAL_BACKUP_ENABLED = '<true|false>']
     [, <additional_options>] );
```

### Parameters

- **collection_name**: Unique identifier for the collection (must be quoted with backticks).
- **table_path**: Absolute path to a table to include in the collection.
- **STORAGE**: Storage backend type (currently supports 'cluster').
- **INCREMENTAL_BACKUP_ENABLED**: Enable or disable incremental backup support.

### Examples

**Basic collection creation:**

```sql
CREATE BACKUP COLLECTION `my_backups`
    ( TABLE `/Root/database/users` )
WITH ( STORAGE = 'cluster' );
```

**Collection with incremental backups (recommended):**

```sql
CREATE BACKUP COLLECTION `shop_backups`
    ( TABLE `/Root/shop/orders`, TABLE `/Root/shop/products` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

## BACKUP {#backup}

Creates a backup within an existing collection. The first backup is always a full backup; subsequent backups can be incremental.

### Syntax

```sql
BACKUP <collection_name> [INCREMENTAL];
```

### Parameters

- **collection_name**: Name of the existing backup collection.
- **INCREMENTAL**: Optional keyword to create an incremental backup.

### Backup types

**Full backup (default for first backup):**

```sql
BACKUP `shop_backups`;
```

**Incremental backup:**

```sql
BACKUP `shop_backups` INCREMENTAL;
```

### Complete workflow example

```sql
-- 1. Create collection
CREATE BACKUP COLLECTION `sales_data`
    ( TABLE `/Root/sales/transactions`, TABLE `/Root/sales/customers` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- 2. Take initial full backup
BACKUP `sales_data`;

-- 3. Take incremental backups
BACKUP `sales_data` INCREMENTAL;
```

## DROP BACKUP COLLECTION {#drop-backup-collection}

Removes a backup collection and all its associated backups.

### Syntax

```sql
DROP BACKUP COLLECTION <collection_name>;
```

{% note warning %}

This operation is irreversible and will delete all backups in the collection. Ensure you have alternative backups before dropping a collection.

{% endnote %}

### Example

```sql
DROP BACKUP COLLECTION `old_backups`;
```

## ALTER BACKUP COLLECTION {#alter-backup-collection}

{% note info %}

Currently, backup collections cannot be modified after creation. To change table membership or settings, you must create a new collection.

{% endnote %}

## DESCRIBE BACKUP COLLECTION {#describe-backup-collection}

View information about a backup collection through schema browsing:

```bash
# List all collections
ydb scheme ls .backups/collections/

# View specific collection structure  
ydb scheme ls .backups/collections/shop_backups/
```

For monitoring backup operations, use the [operation list](../../../reference/ydb-cli/operation-list.md) command:

```bash
# Monitor backup operations
ydb operation list incbackup
```

## Limitations and considerations {#limitations}

### Current limitations

- **Storage backend**: Only 'cluster' storage is currently supported via SQL.
- **Collection modification**: Cannot add or remove tables from existing collections.  
- **Concurrent backups**: Multiple backup operations on the same collection may conflict.

### Best practices

- **Use quoted identifiers**: Always quote collection names with backticks
- **Specify absolute paths**: Use full table paths starting with `/Root/`
- **Monitor operations**: Check operation status using [operation list](../../../reference/ydb-cli/operation-list.md) commands
- **Plan retention**: Consider backup chain dependencies before deletion

## Error handling

Common errors and solutions:

- **Collection already exists**: Choose a different name or drop the existing collection
- **Table not found**: Verify table paths are correct and accessible
- **Permission denied**: Ensure proper access rights to tables and backup storage
- **Storage unavailable**: Check cluster status and storage backend availability

## See also

- [Backup collections concepts](../../../concepts/backup-collections.md) - Core concepts and architecture
- [Backup collections operations](../../../maintenance/manual/backup-collections.md) - Practical operations guide
- [Backup collections recipes](../../../recipes/backup-collections.md) - Common use cases and examples
