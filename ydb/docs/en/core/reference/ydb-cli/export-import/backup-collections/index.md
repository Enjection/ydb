# Backup Collections (Legacy Location)

{% note info %}

This documentation has been moved and consolidated to provide better organization. The backup collections documentation is now available in a centralized structure:

- **[Concepts](../../../../concepts/backup-collections.md)** - Core concepts and architecture
- **[YQL Syntax Reference](../../../../yql/reference/backup-collections.md)** - Complete SQL command documentation  
- **[Operations Guide](../../../../operations/backup-collections.md)** - Practical procedures and examples
- **[CLI Reference](../../../../reference/ydb-cli/backup-collections.md)** - Command-line tools and automation
- **[Common Recipes](../../../../recipes/backup-collections.md)** - Real-world usage scenarios

{% endnote %}

## Quick Start (Redirected Content)

For the complete quick start guide, see the [new centralized documentation](../../../../concepts/backup-collections.md#quick-start).

```sql
-- 1. Create collection
CREATE BACKUP COLLECTION `my_backups`
    ( TABLE `/Root/db/table1`, TABLE `/Root/db/table2` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- 2. Take full backup
BACKUP `my_backups`;

-- 3. Take incremental backups
BACKUP `my_backups` INCREMENTAL;
```

## New Documentation Structure

The backup collections documentation has been reorganized to eliminate duplication and improve usability:

- **Single source of truth**: Each piece of information exists in one location
- **Better organization**: Clear separation between concepts, reference, and practical guides  
- **Comprehensive coverage**: All aspects of backup collections documented
- **Easy maintenance**: Updates only need to be made once

**Please use the [new centralized documentation](../../../../concepts/backup-collections.md) for complete and up-to-date information.**
