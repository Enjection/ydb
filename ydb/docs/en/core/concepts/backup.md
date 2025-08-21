# Backup concepts

This section covers backup concepts and technologies available in {{ ydb-short-name }}.

{{ ydb-short-name }} provides several approaches for creating backups, each designed for different use cases and requirements:

## Export/import {#export-import}

For large-scale data migration and portability scenarios:

- **Use cases**: Large data migration between systems, archival storage, production data transfers.
- **Characteristics**: Point-in-time snapshots with flexible format options, optimized for large datasets.
- **Storage**: S3-compatible storage.

## Backup/restore {#backup-restore}

For local database backups and development workflows:

- **Use cases**: Local development environments, testing scenarios, smaller production environments, database cloning for local use.
- **Characteristics**: Designed for local filesystem operations with moderate data volumes.
- **Storage**: Filesystem.

Learn more:

- [Export and import reference](../reference/ydb-cli/export-import/index.md) - Export/import operations.

## Choosing the right approach {#choosing-approach}

| Approach | Best for | Key advantages | Considerations |
|----------|----------|----------------|----------------|
| **Export/import** | Large data migration, archival, production data transfers | Portability between systems, flexible formats, handles large datasets | Full snapshots only |
| **Backup/restore** | Local development, testing, smaller production environments | Local filesystem operations, suitable for moderate data volumes | Full snapshots only, primarily for local use |

## See also

- [Backup and recovery guide](../devops/backup-and-recovery.md).
- [Export and import reference](../reference/ydb-cli/export-import/index.md).
