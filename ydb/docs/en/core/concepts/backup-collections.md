# Backup Collections

Backup collections provide an advanced backup solution for YDB that organizes full and incremental backups into managed collections. This approach is designed for production workloads requiring efficient disaster recovery and point-in-time recovery capabilities.

## What are backup collections? {#what-are-backup-collections}

A backup collection is a named set of coordinated backups for selected database tables. Collections organize related backups and ensure they can be restored together consistently, providing:

- **Efficiency**: Incremental backups capture only changes since the previous backup
- **Organization**: Related backups are grouped into logical collections
- **Recovery flexibility**: Enables recovery using any backup in the chain

## Core concepts {#core-concepts}

### Backup collection

A named container that groups backups for a specific set of database tables. Collections ensure that all included tables are backed up consistently.

### Full backup

A complete snapshot of all selected tables at a specific point in time. Serves as the baseline for subsequent incremental backups and contains all data needed for independent restoration.

### Incremental backup

Captures only the changes (inserts, updates, deletes) since the previous backup in the chain. Significantly smaller than full backups for datasets with limited changes.

### Backup chain

An ordered sequence of backups starting with a full backup followed by zero or more incremental backups. Each incremental backup depends on all previous backups in the chain for complete restoration.

## Architecture and components {#architecture}

### Backup flow {#backup-flow}

1. **Collection creation**: Define which tables to include and storage settings
2. **Initial full backup**: Create baseline snapshot of all tables
3. **Regular incremental backups**: Capture ongoing changes on-demand
4. **Chain management**: Monitor backup chains and manage retention manually

### Storage structure {#storage-structure}

Backup collections are stored in a dedicated directory structure within the database:

```text
/.backups/collections/
├── collection_name_1/
│   ├── backup_20240315_120000/     # Full backup
│   ├── backup_20240315_180000/     # Incremental backup
│   └── backup_20240316_060000/     # Incremental backup
├── collection_name_2/
│   └── backup_20240316_000000/     # Full backup
```

Each backup contains:

- Table schemas at backup time
- Data files (full or incremental changes)
- Metadata for chain validation and restoration

### Storage backends {#storage-backends}

#### Cluster storage

Backups are stored within the YDB cluster itself, providing:

- **High availability**: Leverages cluster replication and fault tolerance
- **Performance**: Fast backup and restore operations
- **Integration**: Seamless integration with cluster operations
- **Security**: Uses cluster security mechanisms

```sql
WITH ( STORAGE = 'cluster' )
```

#### External storage (future)

Future versions may support automatic export to external storage systems for long-term archival. Currently, use [export/import operations](../reference/ydb-cli/export-import/index.md) for external storage.

### Background operations {#background-operations}

All backup operations run asynchronously in the background, allowing you to:

- Continue normal database operations during backups
- Monitor progress through the long operations API
- Handle large datasets without blocking other activities

## How backup collections work internally {#how-they-work}

### Backup creation process

1. **Transaction isolation**: Backup starts from a consistent snapshot point
2. **Table scanning**: Each table is scanned for data and schema
3. **Change tracking**: For incremental backups, only changes since last backup are captured
4. **Storage writing**: Data is written to the backup storage location
5. **Metadata recording**: Backup metadata is recorded for chain validation

### Incremental backup mechanism

Incremental backups use change tracking to identify:

- **New rows**: Added since last backup
- **Modified rows**: Changed data in existing rows  
- **Deleted rows**: Removed data (tombstone records)
- **Schema changes**: Table structure modifications

### Chain validation and integrity

The system ensures backup chain integrity through:

- **Dependency tracking**: Each incremental backup records its parent
- **Validation checks**: Chain completeness verified before operations
- **Consistency guarantees**: All tables backed up from the same transaction point
- **Error detection**: Corrupted or missing backups identified automatically

## Relationship with incremental backups {#relationship-with-incremental-backups}

Backup collections are the foundation for incremental backup functionality:

- **Collections enable incrementals**: You must have a collection to create incremental backups
- **Chain management**: Collections manage the sequence of full and incremental backups
- **Consistency**: All tables in a collection are backed up consistently

Without backup collections, only full export/import operations are available.

## When to use backup collections {#when-to-use}

**Ideal scenarios:**

- Production environments requiring regular backup schedules
- Large datasets where incremental changes are much smaller than total data size
- Scenarios requiring backup chains for efficiency

**Consider traditional export/import for:**

- Small databases or individual tables
- One-time data migration tasks
- Development/testing environments
- Simple backup scenarios without incremental needs
- External storage requirements (until automatic external storage is supported)

## Benefits and limitations {#benefits-limitations}

### Benefits

- **Storage efficiency**: Incremental backups use significantly less storage
- **Faster backups**: Only changes are processed after initial full backup
- **SQL interface**: Familiar SQL commands for backup management
- **Background processing**: Non-blocking operations
- **Chain integrity**: Automatic validation and consistency checks

### Current limitations

- **Cluster storage only**: External storage requires manual export/import
- **No collection modification**: Cannot add/remove tables after creation
- **Single storage backend**: Only 'cluster' storage supported via SQL
- **Concurrent limitations**: Multiple backups on same collection may conflict

## Next steps {#next-steps}

- **Get started**: Follow the [operations guide](../maintenance/manual/backup-collections.md) for step-by-step instructions
- **See examples**: Explore [common scenarios](../recipes/backup-collections.md) and best practices

## See also

- [General backup concepts](backup.md) - Overview of all backup approaches in YDB
- [Operations guide](../maintenance/manual/backup-collections.md) - Practical instructions and examples
- [Common recipes](../recipes/backup-collections.md) - Real-world usage scenarios
