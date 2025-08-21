# Backup Collections Concepts (Legacy Location)

{% note info %}

This documentation has been moved and consolidated. Please refer to the [centralized backup collections concepts](../../../../concepts/backup-collections.md) for complete and up-to-date conceptual documentation.

{% endnote %}

## New Documentation Structure

The backup collections concepts documentation is now available at:
**[Core Concepts Documentation](../../../../concepts/backup-collections.md)**

This new location provides:
- **Comprehensive architecture overview** - How backup collections work internally
- **Complete storage structure** - Directory layout and organization  
- **Chain management details** - Backup chain validation and integrity
- **Storage backends** - Current and future storage options
- **When to use guidance** - Ideal scenarios and alternatives

## Quick Reference

For immediate reference on chain validity rules:

{% note alert %}

Never delete full backups that have dependent incremental backups. Deleting a full backup breaks the entire chain, making all subsequent incrementals unrestorable.

{% endnote %}

**Chain management best practices:**
- Keep backup chains reasonably short (7-14 incremental backups recommended)
- Plan retention carefully and consider chain dependencies
- Verify backup structure before deletion using schema browsing

**Please use the [new centralized concepts documentation](../../../../concepts/backup-collections.md) for complete information.**

- Apply retention policies carefully to preserve chain validity.
- Verify backup chains periodically before critical operations.
- Respect chronological order when applying backups during restore.

### Maximum chain length recommendations (TBD)

- **Daily incrementals**: Limit chains to 7-14 incremental backups
- **Hourly incrementals**: Limit chains to 24-48 incremental backups
- **High-frequency backups**: Consider synthetic full backups to reset chains

## Limitations and requirements {#limitations-requirements}

### Current limitations

- Cluster storage is the primary backend; filesystem and S3 require export/import operations.
- Chain order must be respected - cannot skip or reorder backups during restore.

## Next steps

- [Learn how to create and manage collections](operations.md).
- [Explore the complete SQL API](sql-api.md).
