# SQL API: Backup Collections (Legacy Location)

{% note info %}

This documentation has been moved and consolidated. Please refer to the [centralized YQL syntax reference](../../../../yql/reference/backup-collections.md) for complete and up-to-date SQL command documentation.

{% endnote %}

## New Documentation Structure

The backup collections SQL API documentation is now available at:
**[YQL Syntax Reference](../../../../yql/reference/backup-collections.md)**

This new location provides:
- **Complete SQL syntax** - All commands with full parameter documentation
- **Comprehensive examples** - Practical usage scenarios
- **Error handling** - Common errors and solutions
- **Best practices** - SQL operation guidelines
- **Parameter reference** - All options and configurations

## Quick Reference

For immediate reference, the main SQL commands:

- `CREATE BACKUP COLLECTION` - Creates a new backup collection
- `BACKUP` - Creates a backup (full or incremental)  
- `DROP BACKUP COLLECTION` - Removes a collection and all backups

**Example:**
```sql
CREATE BACKUP COLLECTION `my_collection`
    ( TABLE `/Root/db/table1` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

BACKUP `my_collection`;
BACKUP `my_collection` INCREMENTAL;
```

**Please use the [new centralized YQL reference](../../../../yql/reference/backup-collections.md) for complete syntax documentation.**
