# Backup Collections Operations {#backup-collections-operations}

This guide covers all practical operations for creating, managing, and restoring from backup collections. For conceptual information, see [Backup collections concepts](../../concepts/backup-collections.md).

## Creating backup collections {#creating-collections}

### SQL Syntax Reference {#sql-syntax-reference}

```sql
-- CREATE BACKUP COLLECTION syntax
CREATE BACKUP COLLECTION `collection_name`
    ( TABLE `table_path` [, TABLE `table_path` ...] )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- BACKUP syntax
BACKUP `collection_name` [INCREMENTAL];

-- DROP BACKUP COLLECTION syntax
DROP BACKUP COLLECTION `collection_name`;
```

For detailed syntax, see [YQL reference documentation](../../yql/reference/syntax/index.md).

### Basic collection creation {#basic-collection-creation}

Create a backup collection using SQL commands. The collection defines which tables to include and storage settings:

```sql
CREATE BACKUP COLLECTION `shop_backups`
    ( TABLE `/Root/shop/orders`, TABLE `/Root/shop/products` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

### Collection planning {#collection-planning}

Before creating a collection, consider:

- **Table selection**: Include related tables that should be backed up consistently.
- **Storage requirements**: Estimate backup size and growth.
- **Backup frequency**: Plan for full and incremental backup schedules.
- **Retention policy**: Determine how long to keep backup chains.

### Naming conventions {#naming-conventions}

Use descriptive names that indicate:

- Application or service name.
- Environment (prod, staging, test).
- Purpose or scope.

Examples:

- `production_user_data`
- `staging_analytics`
- `daily_transaction_backups`

## Taking backups {#taking-backups}

### Initial full backup {#initial-full-backup}

After creating a collection, take the initial full backup:

```sql
BACKUP `shop_backups`;
```

The first backup without the `INCREMENTAL` keyword creates a full backup containing all data from the specified tables.

### Incremental backups {#incremental-backups}

Once you have a full backup, create incremental backups to capture changes:

```sql
BACKUP `shop_backups` INCREMENTAL;
```

### Backup timing considerations {#backup-timing-considerations}

Implement regular backup schedules based on your requirements. Note that scheduling must be implemented externally using cron or similar tools.

**Daily full backups:**

```sql
-- Example: Run daily at 2 AM (requires external scheduling)
BACKUP `shop_backups`;
```

**Hourly incrementals with weekly full backups:**

```sql
-- Example: Sunday: Full backup (requires external scheduling)
BACKUP `shop_backups`;

-- Example: Monday-Saturday: Incremental backups (requires external scheduling)
BACKUP `shop_backups` INCREMENTAL;
```

### Important scheduling notes {#important-scheduling-notes}

- **External scheduling required**: YDB does not provide built-in scheduling. Use cron or similar tools.
- **Background operations**: Backups run asynchronously and don't block database operations.
- **Multiple collections**: Can run backups on different collections independently.

Note: Backup scheduling must be implemented using external tools like cron, as YDB does not provide built-in scheduling.

## Monitoring backup operations {#monitoring}

### Monitoring backup operations {#monitoring-backup-operations}

```bash
# Check backup operation status
ydb operation list incbackup

# Get details for specific operation
ydb operation get <operation-id>

# Cancel running operation if needed
ydb operation cancel <operation-id>

# Browse backup collections
ydb scheme ls .backups/collections/

# List backups in a collection
ydb scheme ls .backups/collections/production_backups/

# Get backup metadata
ydb scheme describe .backups/collections/production_backups/backup_20240315_120000/
```

### Check operation status {#check-operation-status}

All backup operations run in the background. Monitor their progress using:

```bash
# List all backup operations
ydb operation list incbackup

# Check specific operation status
ydb operation get <operation-id>
```

### Browse backup structure {#browse-backup-structure}

View backups through the database schema:

```bash
# List all collections
ydb scheme ls .backups/collections/

# View specific collection structure
ydb scheme ls .backups/collections/shop_backups/

# Check backup timestamps
ydb scheme ls .backups/collections/shop_backups/ | sort
```

### Monitor backup chains {#monitor-backup-chains}

Verify backup chain integrity:

```bash
# List backups in chronological order
ydb scheme ls .backups/collections/shop_backups/ | sort

# Check individual backup contents
ydb scheme describe .backups/collections/shop_backups/backup_20240315/
```

## Managing backup collections {#managing-collections}

### Collection information {#collection-information}

Since collections are managed through SQL, browse them using schema commands:

```bash
# Browse all collections
ydb scheme ls .backups/collections/

# View collection directory contents
ydb scheme describe .backups/collections/shop_backups/
```

### Collection status monitoring {#collection-status-monitoring}

Check the health and status of your collections:

1. **Verify recent backups exist**.
2. **Check backup chain completeness**.
3. **Monitor storage usage**.
4. **Validate backup accessibility**.

### Collection lifecycle management {#collection-lifecycle-management}

**Creation checklist:**

- [ ] Define table list.
- [ ] Choose appropriate name.
- [ ] Configure storage settings.
- [ ] Enable incremental backups.
- [ ] Document backup schedule.

**Ongoing maintenance:**

- [ ] Monitor backup success.
- [ ] Manage retention policies.
- [ ] Update documentation.
- [ ] Review performance impact.

## Retention and cleanup {#retention-cleanup}

### Backup chain considerations {#backup-chain-considerations}

Before deleting backups, understand chain dependencies:

- **Full backups**: Required for all subsequent incrementals.
- **Incremental backups**: Depend on all previous backups in chain.
- **Chain breaks**: Deleting intermediate backups breaks the chain.

### Manual cleanup strategies {#manual-cleanup-strategies}

**Safe cleanup approach:**

1. Create new full backup.
2. Verify new backup is complete.
3. Delete old backup chains (full backup + all its incrementals).
4. Never delete partial chains.

**Example cleanup workflow:**

```bash
# 1. Create new full backup
ydb yql -s "BACKUP \`shop_backups\`;"

# 2. Wait for completion and verify
ydb operation list incbackup

# 3. Browse backup structure
ydb scheme ls .backups/collections/shop_backups/ | sort

# 4. Remove old backup directories (entire chains only)
ydb scheme rmdir -r .backups/collections/shop_backups/backup_20240301/
ydb scheme rmdir -r .backups/collections/shop_backups/backup_20240302/
# ... (remove all related incrementals)
```

### Retention policies {#retention-policies}

Implement retention policies based on:

- **Business requirements**: How long data must be retained.
- **Storage costs**: Balance retention with storage usage.
- **Recovery needs**: Typical recovery scenarios and timeframes.
- **Compliance**: Legal or regulatory requirements.

**Example retention policy:**

- Keep daily backups for 30 days.
- Keep weekly backups for 12 weeks.
- Keep monthly backups for 12 months.
- Keep yearly backups for 7 years.

## Backup validation and verification {#validation}

### Pre-backup validation {#pre-backup-validation}

Before creating backups:

1. **Verify table accessibility**: Ensure all tables are accessible.
2. **Check storage space**: Confirm adequate storage is available.
3. **Validate permissions**: Verify backup operation permissions.
4. **Test connectivity**: Ensure database connectivity is stable.

### Post-backup validation {#post-backup-validation}

After backup completion:

1. **Verify operation success**: Check operation status.
2. **Validate backup structure**: Browse backup directories.
3. **Check backup size**: Compare with expected size.
4. **Test chain integrity**: Verify backup dependencies.

### Backup testing procedures {#backup-testing-procedures}

Regularly test backup restoration:

1. **Test environment**: Use separate test environment.
2. **Sample restoration**: Restore subset of data.
3. **Full restoration test**: Periodically test complete restoration.
4. **Performance testing**: Measure restoration time and resources.

## Troubleshooting common issues {#troubleshooting}

### Backup operation failures {#backup-operation-failures}

**Common causes and solutions:**

- **Insufficient storage**: Check available storage space.
- **Permission denied**: Verify user permissions for tables and backup storage.
- **Table lock conflicts**: Avoid concurrent schema changes during backup.
- **Network issues**: Check database connectivity during backup.

### Backup chain issues {#backup-chain-issues}

**Broken chains:**

- Identify missing backups in the sequence.
- Consider creating new full backup to start fresh chain.
- Document chain breaks for future reference.

**Inconsistent backups:**

- Check for concurrent operations during backup.
- Verify table consistency across backup points.
- Review backup logs for errors or warnings.

### Performance issues {#performance-issues}

**Slow backup operations:**

- Monitor system resources during backup.
- Consider adjusting backup timing.
- Review table sizes and data distribution.
- Check storage backend performance.

**High storage usage:**

- Review retention policies.
- Clean up old backup chains.
- Monitor incremental backup sizes.
- Consider backup frequency adjustments.

## Recovery and restoration {#recovery}

### Restoration planning {#restoration-planning}

Before restoring from backups:

1. **Assess recovery requirements**: Determine target recovery point.
2. **Plan restoration process**: Identify restoration steps and timeline.
3. **Prepare target environment**: Ensure target database is ready.
4. **Coordinate with stakeholders**: Communicate restoration timeline.

### Export and import restoration {#export-import-restoration}

For complete disaster recovery, use export/import operations:

```bash
# Export backup collection from source
ydb tools dump -p .backups/collections/shop_backups -o shop_backups_export

# Import to target database
ydb tools restore -i shop_backups_export -d /Root/restored_db
```

### Point-in-time recovery {#point-in-time-recovery}

To restore to a specific point in time:

1. **Identify target backup**: Find the appropriate backup point.
2. **Export backup chain**: Export the full backup and required incrementals.
3. **Restore in sequence**: Apply backups in chronological order.
4. **Verify restoration**: Validate restored data integrity.

## Best practices summary {#best-practices}

### Backup strategy {#backup-strategy}

- **Regular schedule**: Establish consistent backup timing.
- **Manage chain length**: Take new full backups periodically to avoid excessively long incremental chains.
- **Multiple collections**: Separate collections for different applications.
- **Documentation**: Maintain documentation of backup procedures.

### Monitoring and maintenance {#monitoring-maintenance}

- **Manual validation**: Periodically test backup restoration.
- **Performance tracking**: Monitor backup duration and resource usage.
- **Storage monitoring**: Track backup storage growth.

### Security and compliance {#security-compliance}

- **Access control**: Restrict backup operation permissions.
- **Audit logging**: Log backup and restoration activities.
- **Data protection**: Ensure backups are properly secured.
- **Compliance**: Follow organizational data retention policies.

## See also {#see-also}

- [Backup collections concepts](../../concepts/backup-collections.md) - Core concepts and architecture.
- [Common recipes](../../recipes/backup-collections.md) - Real-world usage examples.
