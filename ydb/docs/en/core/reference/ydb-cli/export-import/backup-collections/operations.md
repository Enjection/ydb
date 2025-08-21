# Backup Collections Operations (Legacy Location)

{% note info %}

This documentation has been moved and consolidated. Please refer to the [centralized backup collections operations guide](../../../../operations/backup-collections.md) for complete and up-to-date operational documentation.

{% endnote %}

## New Documentation Structure

The backup collections operations documentation is now available at:
**[Operations Guide](../../../../operations/backup-collections.md)**

This new location provides comprehensive coverage of:
- **Creating backup collections** - Complete setup procedures
- **Taking backups** - Full and incremental backup operations  
- **Monitoring operations** - Progress tracking and status checking
- **Managing collections** - Collection lifecycle and maintenance
- **Retention and cleanup** - Safe backup removal strategies
- **Validation and verification** - Backup integrity checking
- **Recovery and restoration** - Complete restore procedures

## Quick Reference

For immediate reference, basic operations:

```sql
-- Create collection
CREATE BACKUP COLLECTION `shop_backups`
    ( TABLE `/Root/test1/orders`, TABLE `/Root/test1/products` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- Take full backup
BACKUP `shop_backups`;

-- Take incremental backup
BACKUP `shop_backups` INCREMENTAL;
```

Monitor operations:
```bash
ydb operation list incbackup
```

**Please use the [new centralized operations guide](../../../../operations/backup-collections.md) for complete step-by-step procedures.**

This shows all backup collections as directories in the `.backups/collections/` path.

### View collection information

Since collections are managed through SQL, you can browse them in the schema:

```bash
# Browse all collections
ydb scheme ls .backups/collections/

# Browse specific collection structure
ydb scheme ls .backups/collections/shop_backups/
```

### Collection status and monitoring

Check collection and backup status:

```bash
# View collection directory contents
ydb scheme ls .backups/collections/shop_backups/

# Check for recent backups
ydb scheme ls .backups/collections/shop_backups/ | sort
```

## Retention and cleanup {#retention-cleanup}

### Manual cleanup with SQL

Clean up old backups while maintaining chain validity using SQL commands:

```bash
# Browse backup structure first
ydb scheme ls .backups/collections/shop_backups/

# Remove specific backup directories using rmdir
ydb scheme rmdir -r .backups/collections/shop_backups/backup_20240315/

# Examine backup structure before cleanup
ydb scheme describe .backups/collections/shop_backups/backup_20240315/
```

For SQL-based cleanup:

```sql
-- Delete specific backup tables (syntax may vary)
-- Caution: Ensure you don't break backup chains
DROP TABLE `.backups/collections/shop_backups/backup_20240315/table_name`;
```

### CLI operations for backups

Use YDB operation commands where available:

```bash
# List operations (may include backup operations)
ydb operation list incbackup

# Check specific operation status
ydb operation get <operation-id>
```

**Important retention considerations:**

{% note warning %}

For chain validity rules and critical deletion warnings, see [Chain validity rules](concepts.md#chain-validity-rules).

{% endnote %}

- Always verify chain dependencies before deletion.
- Use schema browsing to understand backup structure.
- Test cleanup procedures in non-production environments first.

## Verification and validation {#verification-validation}

### Verify using CLI operations

Use YDB operation commands to check backup status:

```bash
# List all operations (including backup operations)
ydb operation list incbackup

# Check specific backup operation status
ydb operation get <operation-id>

# List restore operations
ydb operation list restore
```

### Schema-based validation

Verify backup existence and structure through schema browsing:

```bash
# Verify collection exists
ydb scheme ls .backups/collections/shop_backups/

# Check backup timestamps and structure
ydb scheme describe .backups/collections/shop_backups/

# List backup contents
ydb scheme ls .backups/collections/shop_backups/ | sort
```

### Pre-restore validation

Before critical restores, always verify:

1. **Backup chain completeness**
3. **Target environment readiness**
4. **Required permissions and access**

## Restore operations {#restore-operations}

### Export and import backups

Export backups from source database:

```bash
ydb tools dump -p .backups/collections/shop_backups -o shop_backups_export
```

**Create collection in destination database first:**

```sql
-- Create the collection structure to receive restored data
CREATE BACKUP COLLECTION `restored_shop_backups`
    ( TABLE `/Root/restored_db/orders`, TABLE `/Root/restored_db/products` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

Import to target database (applies backups in chronological order):

```bash
# Option 1: Restore entire collection (recommended)
ydb tools restore -i shop_backups_export -d /Root/restored_db

# Option 2: Restore to specific point
# Import full backup first
ydb tools restore -i shop_backups_export/full_backup_20240315 -d /Root/restored_db
# Then apply incrementals up to desired point
ydb tools restore -i shop_backups_export/incremental_20240316 -d /Root/restored_db
```

### SQL restore

After importing, use SQL to restore:

```sql
RESTORE `restored_shop_backups`;
```

### Validation

Verify restored data with row counts, sample queries, and schema checks.

## Next steps

- [Learn the complete SQL API syntax](sql-api.md).
- [Understand backup collection concepts](concepts.md).
