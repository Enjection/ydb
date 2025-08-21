# YDB CLI: Backup Collections

This section covers YDB CLI commands and tools for working with backup collections. For SQL-based backup operations, see the [YQL syntax reference](../yql/reference/backup-collections.md).

## Overview {#overview}

The YDB CLI provides several tools for working with backup collections:

- **SQL execution**: Execute backup collection SQL commands
- **Schema browsing**: Navigate and inspect backup structures
- **Export/import tools**: Move backups between systems
- **Operation monitoring**: Track backup operation progress

## SQL command execution {#sql-execution}

### Basic SQL execution

Execute backup collection SQL commands using the `ydb yql` command:

```bash
# Create a backup collection
ydb yql -s "CREATE BACKUP COLLECTION \`shop_backups\`
    ( TABLE \`/Root/shop/orders\`, TABLE \`/Root/shop/products\` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );"

# Take a full backup
ydb yql -s "BACKUP \`shop_backups\`;"

# Take an incremental backup
ydb yql -s "BACKUP \`shop_backups\` INCREMENTAL;"

# Drop a collection
ydb yql -s "DROP BACKUP COLLECTION \`shop_backups\`;"
```

### Script execution

For automated backup operations, use SQL script files:

```bash
# Create backup script file
cat > backup_script.sql << 'EOF'
-- Daily backup script
BACKUP `production_data` INCREMENTAL;
EOF

# Execute script
ydb yql -f backup_script.sql
```

### Command-line options

Common CLI options for backup operations:

```bash
# Execute with specific database connection
ydb -e grpc://localhost:2136 -d /Root/mydb yql -s "BACKUP \`my_collection\`;"

# Execute with authentication
ydb --ca-file ca.crt --user myuser --password-file pass.txt yql -s "..."

# Execute with output formatting
ydb yql -s "..." --format json
```

## Schema browsing commands {#schema-browsing}

### List backup collections

Browse available backup collections:

```bash
# List all collections
ydb scheme ls .backups/collections/

# List with detailed information
ydb scheme ls -l .backups/collections/

# Recursive listing
ydb scheme ls -R .backups/collections/
```

### Inspect collection contents

Examine specific backup collections:

```bash
# View collection structure
ydb scheme ls .backups/collections/shop_backups/

# Sort backups by timestamp
ydb scheme ls .backups/collections/shop_backups/ | sort

# View backup details
ydb scheme describe .backups/collections/shop_backups/backup_20240315/
```

### Schema navigation examples

```bash
# Find collections containing specific patterns
ydb scheme ls .backups/collections/ | grep shop

# Count backups in a collection
ydb scheme ls .backups/collections/shop_backups/ | wc -l

# Check newest backups
ydb scheme ls .backups/collections/shop_backups/ | sort | tail -5
```

## Export and import operations {#export-import}

### Export backup collections

Export backups for external storage or cross-cluster migration:

```bash
# Export entire collection
ydb tools dump -p .backups/collections/shop_backups -o shop_backups_export

# Export specific backup
ydb tools dump -p .backups/collections/shop_backups/backup_20240315 -o single_backup_export

# Export with compression
ydb tools dump -p .backups/collections/shop_backups -o shop_backups_export.tar.gz --compression gzip
```

### Import backup collections

Import previously exported backups:

```bash
# Import to target database
ydb tools restore -i shop_backups_export -d /Root/restored_db

# Import with table mapping
ydb tools restore -i shop_backups_export -d /Root/new_db --remap-tables

# Import specific tables only
ydb tools restore -i shop_backups_export -d /Root/new_db --include-tables orders,products
```

### Cross-cluster backup migration

Complete workflow for moving backups between clusters:

```bash
# Source cluster: Export collection
ydb -e grpc://source:2136 -d /Root/source_db tools dump \
  -p .backups/collections/production_data -o production_backup_export

# Transfer files to target cluster
scp -r production_backup_export target_server:/tmp/

# Target cluster: Import collection  
ydb -e grpc://target:2136 -d /Root/target_db tools restore \
  -i /tmp/production_backup_export -d /Root/target_db
```

## Operation monitoring {#monitoring}

### List backup operations

Monitor backup operation progress:

```bash
# List all incremental backup operations
ydb operation list incbackup

# List with detailed status
ydb operation list incbackup --format json

# List recent operations only
ydb operation list incbackup --limit 10
```

### Check operation status

Get detailed information about specific operations:

```bash
# Get operation details
ydb operation get <operation-id>

# Get operation with full output
ydb operation get <operation-id> --format json

# Monitor operation until completion
while ! ydb operation get <operation-id> | grep -q "READY\|SUCCESS"; do
  echo "Backup still in progress..."
  sleep 30
done
```

### Operation filtering and searching

```bash
# List operations by status
ydb operation list incbackup | grep "READY"

# List failed operations
ydb operation list incbackup | grep "FAILED"

# Count running operations
ydb operation list incbackup | grep "RUNNING" | wc -l
```

## Automation and scripting {#automation}

### Backup automation scripts

Create automated backup workflows:

```bash
#!/bin/bash
# daily_backup.sh - Automated daily backup script

COLLECTION_NAME="production_data"
DATABASE="/Root/production"
LOG_FILE="/var/log/ydb_backup.log"

echo "$(date): Starting backup for $COLLECTION_NAME" >> "$LOG_FILE"

# Take incremental backup
if ydb -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;" >> "$LOG_FILE" 2>&1; then
    echo "$(date): Backup completed successfully" >> "$LOG_FILE"
else
    echo "$(date): Backup failed" >> "$LOG_FILE"
    exit 1
fi

# Monitor operation completion
# (Add operation monitoring logic here)

echo "$(date): Backup workflow completed" >> "$LOG_FILE"
```

### Cron job integration

Schedule regular backups using cron:

```bash
# Edit crontab
crontab -e

# Add daily backup at 2 AM
0 2 * * * /path/to/daily_backup.sh

# Add hourly incremental backup during business hours
0 9-17 * * 1-5 /path/to/hourly_backup.sh
```

### Backup validation scripts

Automate backup validation:

```bash
#!/bin/bash
# validate_backups.sh - Backup validation script

COLLECTION_NAME="production_data"
BACKUP_PATH=".backups/collections/$COLLECTION_NAME"

# Check if recent backup exists (within last 24 hours)
LATEST_BACKUP=$(ydb scheme ls "$BACKUP_PATH/" | sort | tail -1)
if [ -z "$LATEST_BACKUP" ]; then
    echo "ERROR: No backups found in collection $COLLECTION_NAME"
    exit 1
fi

# Verify backup structure
if ydb scheme describe "$BACKUP_PATH/$LATEST_BACKUP/" > /dev/null 2>&1; then
    echo "SUCCESS: Latest backup $LATEST_BACKUP is accessible"
else
    echo "ERROR: Cannot access latest backup $LATEST_BACKUP"
    exit 1
fi
```

## Performance optimization {#performance}

### Connection optimization

Optimize CLI connections for backup operations:

```bash
# Use connection pooling for multiple operations
export YDB_CONNECTION_POOLING=true

# Increase timeout for large backup operations
ydb --timeout 3600 yql -s "BACKUP \`large_collection\`;"

# Use compression for network efficiency
ydb --compression gzip yql -s "..."
```

### Batch operations

Execute multiple backup commands efficiently:

```bash
# Batch multiple commands in single session
ydb yql << 'EOF'
BACKUP `collection_1` INCREMENTAL;
BACKUP `collection_2` INCREMENTAL;
BACKUP `collection_3` INCREMENTAL;
EOF
```

### Resource monitoring

Monitor resource usage during backup operations:

```bash
# Monitor backup operation with system resources
ydb operation list incbackup &
top -p $(pgrep ydb)

# Track backup storage usage
watch -n 30 "du -sh .backups/collections/*"
```

## Error handling and troubleshooting {#troubleshooting}

### Common CLI errors

**Connection errors:**
```bash
# Check connectivity
ydb scheme ls /

# Verify endpoint and database
ydb -e grpc://localhost:2136 -d /Root/mydb scheme ls /
```

**Authentication errors:**
```bash
# Check credentials
ydb --user myuser --password-file pass.txt scheme ls /

# Verify certificates
ydb --ca-file ca.crt scheme ls /
```

**Permission errors:**
```bash
# Check user permissions
ydb yql -s "SELECT * FROM [/.sys/acl] WHERE path = '/'"

# Test backup permissions
ydb yql -s "SELECT 1" # Basic query test first
```

### Debugging backup operations

Enable detailed logging:

```bash
# Verbose output
ydb --verbose yql -s "BACKUP \`test_collection\`;"

# Debug connection issues
ydb --debug yql -s "..."

# Save operation logs
ydb yql -s "BACKUP \`collection\`;" 2>&1 | tee backup.log
```

### Recovery from failures

Handle common failure scenarios:

```bash
# Retry failed operations
ydb operation list incbackup | grep FAILED | while read -r op_id; do
    echo "Retrying operation: $op_id"
    # Implement retry logic based on operation details
done

# Clean up failed backup attempts
ydb scheme ls .backups/collections/my_collection/ | grep incomplete | while read -r dir; do
    ydb scheme rmdir -r ".backups/collections/my_collection/$dir"
done
```

## Configuration and environment {#configuration}

### Environment variables

Configure CLI behavior with environment variables:

```bash
# Connection settings
export YDB_ENDPOINT="grpc://localhost:2136"
export YDB_DATABASE="/Root/production"
export YDB_USER="backup_user"
export YDB_PASSWORD_FILE="/etc/ydb/backup_password"

# SSL/TLS settings
export YDB_CA_FILE="/etc/ydb/ca.crt"
export YDB_CERT_FILE="/etc/ydb/client.crt"
export YDB_KEY_FILE="/etc/ydb/client.key"

# Performance settings
export YDB_TIMEOUT="3600"
export YDB_COMPRESSION="gzip"
```

### Configuration files

Use configuration files for consistent settings:

```yaml
# ~/.ydb/config.yaml
endpoint: grpc://localhost:2136
database: /Root/production
auth:
  user: backup_user
  password-file: /etc/ydb/backup_password
ssl:
  ca-file: /etc/ydb/ca.crt
timeout: 3600
compression: gzip
```

### Profile management

Manage multiple database profiles:

```bash
# Create profile for production
ydb config profile create production \
  --endpoint grpc://prod:2136 \
  --database /Root/production \
  --user prod_backup

# Create profile for staging
ydb config profile create staging \
  --endpoint grpc://staging:2136 \
  --database /Root/staging \
  --user staging_backup

# Use specific profile
ydb --profile production yql -s "BACKUP \`prod_data\`;"
```

## Integration examples {#integration}

### CI/CD pipeline integration

Integrate backup operations into deployment pipelines:

```yaml
# GitHub Actions example
- name: Create backup before deployment
  run: |
    ydb --profile production yql -s "BACKUP \`app_data\` INCREMENTAL;"
    
    # Wait for backup completion
    while ! ydb --profile production operation list incbackup | tail -1 | grep -q "READY"; do
      sleep 30
    done

- name: Deploy application
  run: ./deploy.sh

- name: Verify deployment
  run: ./verify.sh
```

### Monitoring system integration

Send backup status to monitoring systems:

```bash
#!/bin/bash
# backup_monitor.sh - Send backup metrics to monitoring

COLLECTION_NAME="production_data"
METRIC_ENDPOINT="http://monitoring:8080/metrics"

# Get backup count
BACKUP_COUNT=$(ydb scheme ls .backups/collections/$COLLECTION_NAME/ | wc -l)

# Get latest backup age (in hours)
LATEST_BACKUP=$(ydb scheme ls .backups/collections/$COLLECTION_NAME/ | sort | tail -1)
# Calculate age based on backup timestamp...

# Send metrics
curl -X POST "$METRIC_ENDPOINT" \
  -d "backup_count{collection=\"$COLLECTION_NAME\"} $BACKUP_COUNT"
```

### Log aggregation

Integrate with centralized logging:

```bash
# Send backup logs to syslog
ydb yql -s "BACKUP \`production_data\` INCREMENTAL;" 2>&1 | \
  logger -t ydb_backup -p local0.info

# Send structured logs to log aggregation system
ydb yql -s "BACKUP \`production_data\` INCREMENTAL;" 2>&1 | \
  jq -n --arg msg "$(cat)" --arg ts "$(date -Iseconds)" \
    '{timestamp: $ts, message: $msg, service: "ydb_backup"}' | \
  curl -X POST logging-service:8080/logs -d @-
```

## See also

- [YQL syntax reference](../yql/reference/backup-collections.md) - Complete SQL command documentation
- [Operations guide](../operations/backup-collections.md) - Practical backup procedures
- [Concepts](../concepts/backup-collections.md) - Core concepts and architecture
- [CLI reference](./ydb-cli/) - General YDB CLI documentation
