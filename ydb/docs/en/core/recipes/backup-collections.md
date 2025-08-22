# Backup Collections: Common Recipes and Examples

This guide provides practical examples and recipes for common backup collection use cases. For basic operations, see the [operations guide](../maintenance/manual/backup-collections.md). For complete syntax, see the [YQL reference](../yql/reference/syntax/backup-collections.md).

## Automated daily backups {#automated-daily-backups}

### Basic daily backup schedule

Set up a simple daily backup routine with weekly full backups:

```sql
-- Sunday: Full backup
CREATE BACKUP COLLECTION IF NOT EXISTS `daily_production_backups`
    ( TABLE `/Root/production/users`
    , TABLE `/Root/production/orders`
    , TABLE `/Root/production/products`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- Sunday (day 0): Full backup
BACKUP `daily_production_backups`;

-- Monday-Saturday: Incremental backups
BACKUP `daily_production_backups` INCREMENTAL;
```

### Shell script for automated backups

```bash
#!/bin/bash
# daily_backup.sh - Automated daily backup with rotation

COLLECTION_NAME="daily_production_backups"
DATABASE="/Root/production"
LOG_FILE="/var/log/backup/ydb_daily.log"
RETENTION_DAYS=30

# Function to log messages
log_message() {
    echo "$(date '+%Y-%m-%d %H:%M:%S'): $1" | tee -a "$LOG_FILE"
}

# Function to check if it's Sunday (full backup day)
is_sunday() {
    [ "$(date +%u)" -eq 7 ]
}

# Create collection if it doesn't exist
create_collection() {
    log_message "Creating backup collection $COLLECTION_NAME"
    ydb -d "$DATABASE" yql -s "
        CREATE BACKUP COLLECTION \`$COLLECTION_NAME\`
            ( TABLE \`/Root/production/users\`
            , TABLE \`/Root/production/orders\`
            , TABLE \`/Root/production/products\`
            )
        WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );"
}

# Perform backup
perform_backup() {
    local backup_type="$1"
    log_message "Starting $backup_type backup for $COLLECTION_NAME"
    
    if [ "$backup_type" = "full" ]; then
        ydb -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\`;"
    else
        ydb -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;"
    fi
    
    if [ $? -eq 0 ]; then
        log_message "$backup_type backup completed successfully"
    else
        log_message "ERROR: $backup_type backup failed"
        exit 1
    fi
}

# Clean up old backups
cleanup_old_backups() {
    log_message "Cleaning up backups older than $RETENTION_DAYS days"
    
    # List backups and filter by age
    ydb -d "$DATABASE" scheme ls ".backups/collections/$COLLECTION_NAME/" | \
    while read -r backup_dir; do
        # Extract date from backup directory name (assumes format: backup_YYYYMMDD_HHMMSS)
        backup_date=$(echo "$backup_dir" | sed 's/backup_\([0-9]\{8\}\).*/\1/')
        if [ -n "$backup_date" ]; then
            # Calculate age in days
            backup_epoch=$(date -d "$backup_date" +%s 2>/dev/null)
            current_epoch=$(date +%s)
            age_days=$(( (current_epoch - backup_epoch) / 86400 ))
            
            if [ "$age_days" -gt "$RETENTION_DAYS" ]; then
                log_message "Removing old backup: $backup_dir (age: $age_days days)"
                ydb -d "$DATABASE" scheme rmdir -r ".backups/collections/$COLLECTION_NAME/$backup_dir"
            fi
        fi
    done
}

# Main execution
main() {
    log_message "Starting daily backup process"
    
    # Ensure collection exists
    create_collection 2>/dev/null  # Ignore error if already exists
    
    # Determine backup type
    if is_sunday; then
        perform_backup "full"
        cleanup_old_backups
    else
        perform_backup "incremental"
    fi
    
    log_message "Daily backup process completed"
}

# Run main function
main
```

### Cron configuration

```bash
# Add to crontab (crontab -e)
# Daily backup at 2 AM
0 2 * * * /opt/backup/daily_backup.sh

# Alternative: Separate schedules for full and incremental
0 2 * * 0 /opt/backup/weekly_full_backup.sh    # Sunday full backup
0 2 * * 1-6 /opt/backup/daily_incremental.sh  # Mon-Sat incremental
```

## Multi-region backup strategy {#multi-region-backup-strategy}

### Cross-region backup replication

Implement backup replication across multiple regions:

```bash
#!/bin/bash
# multi_region_backup.sh - Cross-region backup replication

PRIMARY_REGION="us-east-1"
BACKUP_REGIONS=("us-west-2" "eu-west-1")
COLLECTION_NAME="global_production_backups"

# Primary region backup
primary_backup() {
    echo "Creating backup in primary region: $PRIMARY_REGION"
    ydb -e "grpc://$PRIMARY_REGION.ydb.cluster:2136" \
        -d "/Root/production" \
        yql -s "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;"
}

# Export and replicate to backup regions
replicate_to_regions() {
    local backup_export_dir="/tmp/backup_export_$(date +%Y%m%d_%H%M%S)"
    
    # Export from primary
    echo "Exporting backup from primary region"
    ydb -e "grpc://$PRIMARY_REGION.ydb.cluster:2136" \
        -d "/Root/production" \
        tools dump -p ".backups/collections/$COLLECTION_NAME" \
        -o "$backup_export_dir"
    
    # Replicate to each backup region
    for region in "${BACKUP_REGIONS[@]}"; do
        echo "Replicating to region: $region"
        
        # Create collection in backup region if needed
        ydb -e "grpc://$region.ydb.cluster:2136" \
            -d "/Root/backup" \
            yql -s "CREATE BACKUP COLLECTION IF NOT EXISTS \`$COLLECTION_NAME\`
                ( TABLE \`/Root/backup/users\`
                , TABLE \`/Root/backup/orders\`
                , TABLE \`/Root/backup/products\`
                )
            WITH ( STORAGE = 'cluster' );" 2>/dev/null
        
        # Import backup data
        ydb -e "grpc://$region.ydb.cluster:2136" \
            -d "/Root/backup" \
            tools restore -i "$backup_export_dir" -d "/Root/backup"
    done
    
    # Cleanup temporary export
    rm -rf "$backup_export_dir"
}

# Execute multi-region backup
primary_backup
replicate_to_regions
```

### Regional backup validation

```bash
#!/bin/bash
# validate_regional_backups.sh - Validate backups across regions

REGIONS=("us-east-1" "us-west-2" "eu-west-1")
COLLECTION_NAME="global_production_backups"
VALIDATION_REPORT="/var/log/backup/regional_validation.log"

validate_region() {
    local region="$1"
    local endpoint="grpc://$region.ydb.cluster:2136"
    local database="/Root/backup"
    
    echo "Validating backups in region: $region" | tee -a "$VALIDATION_REPORT"
    
    # Check collection exists
    if ! ydb -e "$endpoint" -d "$database" scheme ls ".backups/collections/$COLLECTION_NAME/" > /dev/null 2>&1; then
        echo "  ERROR: Collection not found in $region" | tee -a "$VALIDATION_REPORT"
        return 1
    fi
    
    # Count backups
    local backup_count=$(ydb -e "$endpoint" -d "$database" \
                        scheme ls ".backups/collections/$COLLECTION_NAME/" | wc -l)
    echo "  Found $backup_count backups" | tee -a "$VALIDATION_REPORT"
    
    # Check latest backup age
    local latest_backup=$(ydb -e "$endpoint" -d "$database" \
                         scheme ls ".backups/collections/$COLLECTION_NAME/" | sort | tail -1)
    echo "  Latest backup: $latest_backup" | tee -a "$VALIDATION_REPORT"
    
    return 0
}

# Validate all regions
echo "$(date): Starting regional backup validation" | tee -a "$VALIDATION_REPORT"
for region in "${REGIONS[@]}"; do
    validate_region "$region"
done
echo "$(date): Regional backup validation completed" | tee -a "$VALIDATION_REPORT"
```

## Retention policies implementation {#retention-policies}

### Grandfather-father-son retention strategy

Implement a comprehensive retention policy with multiple backup frequencies:

```bash
#!/bin/bash
# retention_policy.sh - Implement GFS retention strategy

COLLECTION_NAME="production_backups"
DATABASE="/Root/production"

# Retention periods
DAILY_RETENTION_DAYS=30
WEEKLY_RETENTION_WEEKS=12
MONTHLY_RETENTION_MONTHS=12
YEARLY_RETENTION_YEARS=7

# Function to get backup age in days
get_backup_age_days() {
    local backup_name="$1"
    local backup_date=$(echo "$backup_name" | grep -o '[0-9]\{8\}' | head -1)
    
    if [ -n "$backup_date" ]; then
        local backup_epoch=$(date -d "$backup_date" +%s 2>/dev/null)
        local current_epoch=$(date +%s)
        echo $(( (current_epoch - backup_epoch) / 86400 ))
    else
        echo "999999"  # Very old if can't parse date
    fi
}

# Function to determine if backup should be kept
should_keep_backup() {
    local backup_name="$1"
    local age_days=$(get_backup_age_days "$backup_name")
    local backup_date=$(echo "$backup_name" | grep -o '[0-9]\{8\}' | head -1)
    
    # Parse date components
    local year=${backup_date:0:4}
    local month=${backup_date:4:2}
    local day=${backup_date:6:2}
    
    # Current date components
    local current_year=$(date +%Y)
    local current_month=$(date +%m)
    
    # Daily retention (keep all backups within daily retention period)
    if [ "$age_days" -le "$DAILY_RETENTION_DAYS" ]; then
        echo "daily"
        return
    fi
    
    # Weekly retention (keep weekly backups - Sundays)
    local day_of_week=$(date -d "$backup_date" +%u 2>/dev/null)
    if [ "$day_of_week" -eq 7 ] && [ "$age_days" -le $((WEEKLY_RETENTION_WEEKS * 7)) ]; then
        echo "weekly"
        return
    fi
    
    # Monthly retention (keep first backup of each month)
    if [ "$day" -le 7 ] && [ "$age_days" -le $((MONTHLY_RETENTION_MONTHS * 30)) ]; then
        echo "monthly"
        return
    fi
    
    # Yearly retention (keep first backup of each year)
    if [ "$month" -eq 1 ] && [ "$day" -le 7 ] && [ $((current_year - year)) -le "$YEARLY_RETENTION_YEARS" ]; then
        echo "yearly"
        return
    fi
    
    echo "delete"
}

# Apply retention policy
apply_retention_policy() {
    echo "$(date): Applying retention policy to $COLLECTION_NAME"
    
    # Get all backups
    local backups=$(ydb -d "$DATABASE" scheme ls ".backups/collections/$COLLECTION_NAME/" | sort)
    
    echo "$backups" | while read -r backup_name; do
        if [ -n "$backup_name" ]; then
            local action=$(should_keep_backup "$backup_name")
            local age_days=$(get_backup_age_days "$backup_name")
            
            case "$action" in
                "delete")
                    echo "Deleting old backup: $backup_name (age: $age_days days)"
                    ydb -d "$DATABASE" scheme rmdir -r ".backups/collections/$COLLECTION_NAME/$backup_name"
                    ;;
                "daily"|"weekly"|"monthly"|"yearly")
                    echo "Keeping $action backup: $backup_name (age: $age_days days)"
                    ;;
            esac
        fi
    done
    
    echo "$(date): Retention policy application completed"
}

# Execute retention policy
apply_retention_policy
```

### Automated retention with monitoring

```bash
#!/bin/bash
# retention_monitor.sh - Monitor and report retention policy compliance

COLLECTION_NAME="production_backups"
DATABASE="/Root/production"
ALERT_EMAIL="admin@company.com"
REPORT_FILE="/var/log/backup/retention_report.txt"

generate_retention_report() {
    echo "Backup Retention Report - $(date)" > "$REPORT_FILE"
    echo "========================================" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    
    # Count backups by age category
    local daily_count=0
    local weekly_count=0
    local monthly_count=0
    local yearly_count=0
    local total_count=0
    
    ydb -d "$DATABASE" scheme ls ".backups/collections/$COLLECTION_NAME/" | \
    while read -r backup_name; do
        if [ -n "$backup_name" ]; then
            local age_days=$(get_backup_age_days "$backup_name")
            local category=$(should_keep_backup "$backup_name")
            
            case "$category" in
                "daily") ((daily_count++)) ;;
                "weekly") ((weekly_count++)) ;;
                "monthly") ((monthly_count++)) ;;
                "yearly") ((yearly_count++)) ;;
            esac
            ((total_count++))
        fi
    done
    
    echo "Backup counts by retention category:" >> "$REPORT_FILE"
    echo "  Daily backups: $daily_count" >> "$REPORT_FILE"
    echo "  Weekly backups: $weekly_count" >> "$REPORT_FILE"
    echo "  Monthly backups: $monthly_count" >> "$REPORT_FILE"
    echo "  Yearly backups: $yearly_count" >> "$REPORT_FILE"
    echo "  Total backups: $total_count" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    
    # Check for compliance issues
    if [ "$daily_count" -eq 0 ]; then
        echo "WARNING: No daily backups found!" >> "$REPORT_FILE"
    fi
    
    if [ "$total_count" -gt 100 ]; then
        echo "WARNING: High backup count ($total_count) - consider more aggressive retention" >> "$REPORT_FILE"
    fi
}

# Send alerts if needed
check_and_alert() {
    if grep -q "WARNING" "$REPORT_FILE"; then
        mail -s "Backup Retention Alert - $COLLECTION_NAME" "$ALERT_EMAIL" < "$REPORT_FILE"
    fi
}

# Execute monitoring
generate_retention_report
check_and_alert
```

## Disaster recovery setup {#disaster-recovery-setup}

### Complete disaster recovery workflow

Implement a comprehensive disaster recovery solution:

```bash
#!/bin/bash
# disaster_recovery.sh - Complete DR backup and recovery workflow

# Configuration
PRIMARY_CLUSTER="grpc://primary.ydb.cluster:2136"
DR_CLUSTER="grpc://dr.ydb.cluster:2136"
PRIMARY_DB="/Root/production"
DR_DB="/Root/disaster_recovery"
COLLECTION_NAME="production_dr_backups"
DR_SCHEDULE_HOURS=4  # DR backup every 4 hours

# Logging
DR_LOG="/var/log/backup/disaster_recovery.log"
log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S'): $1" | tee -a "$DR_LOG"
}

# Create DR backup collection
setup_dr_collection() {
    log "Setting up disaster recovery backup collection"
    
    # Primary cluster collection
    ydb -e "$PRIMARY_CLUSTER" -d "$PRIMARY_DB" yql -s "
        CREATE BACKUP COLLECTION IF NOT EXISTS \`$COLLECTION_NAME\`
            ( TABLE \`/Root/production/users\`
            , TABLE \`/Root/production/orders\`
            , TABLE \`/Root/production/products\`
            , TABLE \`/Root/production/transactions\`
            , TABLE \`/Root/production/audit_log\`
            )
        WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );"
    
    # DR cluster collection (for importing backups)
    ydb -e "$DR_CLUSTER" -d "$DR_DB" yql -s "
        CREATE BACKUP COLLECTION IF NOT EXISTS \`$COLLECTION_NAME\`
            ( TABLE \`/Root/disaster_recovery/users\`
            , TABLE \`/Root/disaster_recovery/orders\`
            , TABLE \`/Root/disaster_recovery/products\`
            , TABLE \`/Root/disaster_recovery/transactions\`
            , TABLE \`/Root/disaster_recovery/audit_log\`
            )
        WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );" 2>/dev/null
}

# Perform DR backup
perform_dr_backup() {
    log "Starting disaster recovery backup"
    
    # Take backup on primary cluster
    ydb -e "$PRIMARY_CLUSTER" -d "$PRIMARY_DB" yql -s "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;"
    
    if [ $? -eq 0 ]; then
        log "Primary cluster backup completed successfully"
        export_and_replicate
    else
        log "ERROR: Primary cluster backup failed"
        return 1
    fi
}

# Export and replicate to DR site
export_and_replicate() {
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local export_dir="/tmp/dr_backup_$timestamp"
    
    log "Exporting backup for DR replication"
    
    # Export latest backup
    ydb -e "$PRIMARY_CLUSTER" -d "$PRIMARY_DB" \
        tools dump -p ".backups/collections/$COLLECTION_NAME" -o "$export_dir"
    
    if [ $? -eq 0 ]; then
        log "Backup export completed, replicating to DR site"
        
        # Import to DR cluster
        ydb -e "$DR_CLUSTER" -d "$DR_DB" \
            tools restore -i "$export_dir" -d "$DR_DB"
        
        if [ $? -eq 0 ]; then
            log "DR replication completed successfully"
        else
            log "ERROR: DR replication failed"
        fi
        
        # Cleanup export directory
        rm -rf "$export_dir"
    else
        log "ERROR: Backup export failed"
        return 1
    fi
}

# Validate DR environment
validate_dr_environment() {
    log "Validating disaster recovery environment"
    
    # Check DR cluster connectivity
    if ! ydb -e "$DR_CLUSTER" -d "$DR_DB" scheme ls / > /dev/null 2>&1; then
        log "ERROR: Cannot connect to DR cluster"
        return 1
    fi
    
    # Check backup collection exists
    if ! ydb -e "$DR_CLUSTER" -d "$DR_DB" scheme ls ".backups/collections/$COLLECTION_NAME/" > /dev/null 2>&1; then
        log "ERROR: DR backup collection not found"
        return 1
    fi
    
    # Check latest backup age
    local latest_backup=$(ydb -e "$DR_CLUSTER" -d "$DR_DB" \
                         scheme ls ".backups/collections/$COLLECTION_NAME/" | sort | tail -1)
    log "Latest DR backup: $latest_backup"
    
    # Verify table accessibility
    local table_count=$(ydb -e "$DR_CLUSTER" -d "$DR_DB" scheme ls "$DR_DB/" | wc -l)
    log "DR database contains $table_count objects"
    
    log "DR environment validation completed"
}

# Emergency recovery procedure
emergency_recovery() {
    log "EMERGENCY: Starting disaster recovery procedure"
    
    # Validate DR environment
    validate_dr_environment
    
    # Switch application to DR database
    log "Switching applications to DR database"
    # (Application-specific switchover logic here)
    
    # Notify stakeholders
    echo "DISASTER RECOVERY ACTIVATED at $(date)" | \
        mail -s "CRITICAL: DR Activated" admin@company.com
    
    log "Emergency recovery procedure completed"
}

# Scheduled DR backup
scheduled_dr_backup() {
    setup_dr_collection
    perform_dr_backup
    validate_dr_environment
}

# Main execution based on argument
case "${1:-scheduled}" in
    "setup")
        setup_dr_collection
        ;;
    "backup")
        scheduled_dr_backup
        ;;
    "validate")
        validate_dr_environment
        ;;
    "emergency")
        emergency_recovery
        ;;
    "scheduled")
        scheduled_dr_backup
        ;;
    *)
        echo "Usage: $0 {setup|backup|validate|emergency|scheduled}"
        exit 1
        ;;
esac
```

### DR monitoring and alerting

```bash
#!/bin/bash
# dr_monitoring.sh - Monitor DR backup health and alert on issues

COLLECTION_NAME="production_dr_backups"
PRIMARY_CLUSTER="grpc://primary.ydb.cluster:2136"
DR_CLUSTER="grpc://dr.ydb.cluster:2136"
PRIMARY_DB="/Root/production"
DR_DB="/Root/disaster_recovery"
ALERT_EMAIL="dr-team@company.com"
MAX_BACKUP_AGE_HOURS=6

# Check backup age
check_backup_age() {
    local cluster="$1"
    local database="$2"
    local cluster_name="$3"
    
    local latest_backup=$(ydb -e "$cluster" -d "$database" \
                         scheme ls ".backups/collections/$COLLECTION_NAME/" | sort | tail -1)
    
    if [ -z "$latest_backup" ]; then
        echo "ERROR: No backups found in $cluster_name cluster"
        return 1
    fi
    
    # Extract timestamp and calculate age
    local backup_date=$(echo "$latest_backup" | grep -o '[0-9]\{8\}_[0-9]\{6\}')
    if [ -n "$backup_date" ]; then
        local backup_epoch=$(date -d "${backup_date:0:8} ${backup_date:9:2}:${backup_date:11:2}:${backup_date:13:2}" +%s 2>/dev/null)
        local current_epoch=$(date +%s)
        local age_hours=$(( (current_epoch - backup_epoch) / 3600 ))
        
        echo "Latest backup in $cluster_name: $latest_backup (age: $age_hours hours)"
        
        if [ "$age_hours" -gt "$MAX_BACKUP_AGE_HOURS" ]; then
            echo "WARNING: Backup in $cluster_name is $age_hours hours old (max: $MAX_BACKUP_AGE_HOURS)"
            return 1
        fi
    else
        echo "ERROR: Cannot parse backup timestamp in $cluster_name"
        return 1
    fi
    
    return 0
}

# Generate DR status report
generate_dr_report() {
    local report_file="/tmp/dr_status_report.txt"
    
    echo "Disaster Recovery Status Report - $(date)" > "$report_file"
    echo "================================================" >> "$report_file"
    echo "" >> "$report_file"
    
    # Check primary cluster
    echo "PRIMARY CLUSTER STATUS:" >> "$report_file"
    if check_backup_age "$PRIMARY_CLUSTER" "$PRIMARY_DB" "primary" >> "$report_file" 2>&1; then
        echo "  Status: OK" >> "$report_file"
    else
        echo "  Status: ERROR" >> "$report_file"
    fi
    echo "" >> "$report_file"
    
    # Check DR cluster
    echo "DR CLUSTER STATUS:" >> "$report_file"
    if check_backup_age "$DR_CLUSTER" "$DR_DB" "DR" >> "$report_file" 2>&1; then
        echo "  Status: OK" >> "$report_file"
    else
        echo "  Status: ERROR" >> "$report_file"
    fi
    echo "" >> "$report_file"
    
    # Check cluster connectivity
    echo "CONNECTIVITY STATUS:" >> "$report_file"
    if ydb -e "$PRIMARY_CLUSTER" -d "$PRIMARY_DB" scheme ls / > /dev/null 2>&1; then
        echo "  Primary cluster: OK" >> "$report_file"
    else
        echo "  Primary cluster: ERROR - Cannot connect" >> "$report_file"
    fi
    
    if ydb -e "$DR_CLUSTER" -d "$DR_DB" scheme ls / > /dev/null 2>&1; then
        echo "  DR cluster: OK" >> "$report_file"
    else
        echo "  DR cluster: ERROR - Cannot connect" >> "$report_file"
    fi
    
    # Send alert if errors found
    if grep -q "ERROR\|WARNING" "$report_file"; then
        mail -s "DR Status Alert - Issues Detected" "$ALERT_EMAIL" < "$report_file"
    fi
    
    # Copy to log directory
    cp "$report_file" "/var/log/backup/dr_status_$(date +%Y%m%d_%H%M%S).txt"
    rm -f "$report_file"
}

# Execute monitoring
generate_dr_report
```

### Cron configuration for DR

```bash
# DR backup every 4 hours
0 */4 * * * /opt/backup/disaster_recovery.sh backup

# DR validation every hour
0 * * * * /opt/backup/dr_monitoring.sh

# Weekly full DR test (Saturday 2 AM)
0 2 * * 6 /opt/backup/disaster_recovery.sh validate
```

## Application-specific backup patterns {#application-patterns}

### Microservices backup coordination

Coordinate backups across multiple microservices:

```bash
#!/bin/bash
# microservices_backup.sh - Coordinate backups across microservices

SERVICES=("user-service" "order-service" "payment-service" "inventory-service")
DATABASE="/Root/microservices"

# Function to backup individual service
backup_service() {
    local service="$1"
    local collection_name="${service}_backups"
    
    echo "Backing up $service..."
    
    # Service-specific table patterns
    case "$service" in
        "user-service")
            tables="TABLE \`/Root/microservices/users\`, TABLE \`/Root/microservices/profiles\`"
            ;;
        "order-service")
            tables="TABLE \`/Root/microservices/orders\`, TABLE \`/Root/microservices/order_items\`"
            ;;
        "payment-service")
            tables="TABLE \`/Root/microservices/payments\`, TABLE \`/Root/microservices/transactions\`"
            ;;
        "inventory-service")
            tables="TABLE \`/Root/microservices/products\`, TABLE \`/Root/microservices/inventory\`"
            ;;
    esac
    
    # Create collection if needed
    ydb -d "$DATABASE" yql -s "
        CREATE BACKUP COLLECTION IF NOT EXISTS \`$collection_name\`
            ( $tables )
        WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );" 2>/dev/null
    
    # Take incremental backup
    ydb -d "$DATABASE" yql -s "BACKUP \`$collection_name\` INCREMENTAL;"
}

# Coordinate backup across all services
coordinate_backups() {
    echo "Starting coordinated microservices backup at $(date)"
    
    # Backup all services in parallel
    for service in "${SERVICES[@]}"; do
        backup_service "$service" &
    done
    
    # Wait for all backups to complete
    wait
    
    echo "All microservices backups completed at $(date)"
}

# Execute coordinated backup
coordinate_backups
```

### Database sharding backup strategy

Handle backups for sharded databases:

```bash
#!/bin/bash
# sharded_backup.sh - Backup strategy for sharded databases

SHARD_COUNT=4
DATABASE_PREFIX="/Root/shard"
COLLECTION_PREFIX="shard_backups"

# Function to backup individual shard
backup_shard() {
    local shard_id="$1"
    local database="${DATABASE_PREFIX}_${shard_id}"
    local collection_name="${COLLECTION_PREFIX}_${shard_id}"
    
    echo "Backing up shard $shard_id..."
    
    # Create shard collection
    ydb -d "$database" yql -s "
        CREATE BACKUP COLLECTION IF NOT EXISTS \`$collection_name\`
            ( TABLE \`${database}/users\`
            , TABLE \`${database}/data\`
            )
        WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );"
    
    # Take backup
    ydb -d "$database" yql -s "BACKUP \`$collection_name\` INCREMENTAL;"
}

# Backup all shards
backup_all_shards() {
    echo "Starting sharded database backup"
    
    # Backup shards in parallel
    for ((shard=0; shard<SHARD_COUNT; shard++)); do
        backup_shard "$shard" &
    done
    
    # Wait for completion
    wait
    
    echo "All shard backups completed"
}

# Validate shard backup consistency
validate_shard_backups() {
    echo "Validating shard backup consistency"
    
    local backup_timestamps=()
    
    # Collect latest backup timestamps from all shards
    for ((shard=0; shard<SHARD_COUNT; shard++)); do
        local database="${DATABASE_PREFIX}_${shard}"
        local collection_name="${COLLECTION_PREFIX}_${shard}"
        
        local latest_backup=$(ydb -d "$database" \
                             scheme ls ".backups/collections/$collection_name/" | sort | tail -1)
        backup_timestamps+=("$latest_backup")
    done
    
    # Check if all backups are from the same time period (within 1 hour)
    local first_timestamp="${backup_timestamps[0]}"
    local first_epoch=$(date -d "$(echo "$first_timestamp" | grep -o '[0-9]\{8\}_[0-9]\{6\}' | \
                                sed 's/_/ /' | sed 's/\(.*\) \(..\)\(..\)\(..\)/\1 \2:\3:\4/')" +%s 2>/dev/null)
    
    for timestamp in "${backup_timestamps[@]}"; do
        local epoch=$(date -d "$(echo "$timestamp" | grep -o '[0-9]\{8\}_[0-9]\{6\}' | \
                              sed 's/_/ /' | sed 's/\(.*\) \(..\)\(..\)\(..\)/\1 \2:\3:\4/')" +%s 2>/dev/null)
        local diff_minutes=$(( (epoch - first_epoch) / 60 ))
        
        if [ "${diff_minutes#-}" -gt 60 ]; then  # Absolute value > 60 minutes
            echo "WARNING: Shard backup timing inconsistency detected"
            echo "  Reference: $first_timestamp"
            echo "  Different: $timestamp (diff: $diff_minutes minutes)"
        fi
    done
    
    echo "Shard backup validation completed"
}

# Execute sharded backup
backup_all_shards
validate_shard_backups
```

## Monitoring and alerting integration {#monitoring-integration}

### Prometheus metrics integration

Export backup metrics for Prometheus monitoring:

```bash
#!/bin/bash
# backup_metrics.sh - Export backup metrics for Prometheus

COLLECTION_NAME="production_backups"
DATABASE="/Root/production"
METRICS_FILE="/var/lib/node_exporter/textfile_collector/ydb_backup_metrics.prom"

# Function to get backup count
get_backup_count() {
    ydb -d "$DATABASE" scheme ls ".backups/collections/$COLLECTION_NAME/" | wc -l
}

# Function to get latest backup age in seconds
get_latest_backup_age() {
    local latest_backup=$(ydb -d "$DATABASE" scheme ls ".backups/collections/$COLLECTION_NAME/" | sort | tail -1)
    
    if [ -n "$latest_backup" ]; then
        local backup_date=$(echo "$latest_backup" | grep -o '[0-9]\{8\}_[0-9]\{6\}')
        if [ -n "$backup_date" ]; then
            local backup_epoch=$(date -d "${backup_date:0:8} ${backup_date:9:2}:${backup_date:11:2}:${backup_date:13:2}" +%s 2>/dev/null)
            local current_epoch=$(date +%s)
            echo $((current_epoch - backup_epoch))
        else
            echo "-1"
        fi
    else
        echo "-1"
    fi
}

# Function to check if backup operation is running
get_backup_operation_status() {
    local running_ops=$(ydb operation list incbackup | grep -c "RUNNING")
    echo "$running_ops"
}

# Generate metrics
generate_metrics() {
    local backup_count=$(get_backup_count)
    local backup_age=$(get_latest_backup_age)
    local running_operations=$(get_backup_operation_status)
    local timestamp=$(date +%s)
    
    cat > "$METRICS_FILE" << EOF
# HELP ydb_backup_collection_count Number of backups in collection
# TYPE ydb_backup_collection_count gauge
ydb_backup_collection_count{collection="$COLLECTION_NAME"} $backup_count

# HELP ydb_backup_latest_age_seconds Age of latest backup in seconds
# TYPE ydb_backup_latest_age_seconds gauge
ydb_backup_latest_age_seconds{collection="$COLLECTION_NAME"} $backup_age

# HELP ydb_backup_operations_running Number of currently running backup operations
# TYPE ydb_backup_operations_running gauge
ydb_backup_operations_running{collection="$COLLECTION_NAME"} $running_operations

# HELP ydb_backup_metrics_last_update_timestamp Last time metrics were updated
# TYPE ydb_backup_metrics_last_update_timestamp gauge
ydb_backup_metrics_last_update_timestamp $timestamp
EOF
}

# Execute metrics generation
generate_metrics
```

### Grafana dashboard configuration

Example Grafana dashboard JSON snippet:

```json
{
  "dashboard": {
    "title": "YDB Backup Collections",
    "panels": [
      {
        "title": "Backup Count",
        "type": "stat",
        "targets": [
          {
            "expr": "ydb_backup_collection_count",
            "legendFormat": "{{ "{{" }}collection{{ "}}" }}"
          }
        ]
      },
      {
        "title": "Latest Backup Age",
        "type": "stat",
        "targets": [
          {
            "expr": "ydb_backup_latest_age_seconds / 3600",
            "legendFormat": "Hours"
          }
        ],
        "fieldConfig": {
          "defaults": {
            "thresholds": {
              "steps": [
                {"color": "green", "value": 0},
                {"color": "yellow", "value": 24},
                {"color": "red", "value": 48}
              ]
            }
          }
        }
      },
      {
        "title": "Running Backup Operations",
        "type": "stat",
        "targets": [
          {
            "expr": "ydb_backup_operations_running",
            "legendFormat": "Running"
          }
        ]
      },
      {
        "title": "Backup Timeline",
        "type": "graph",
        "targets": [
          {
            "expr": "increase(ydb_backup_collection_count[1h])",
            "legendFormat": "Backups per hour"
          }
        ]
      }
    ]
  }
}
```

### Alerting rules

Prometheus alerting rules for backup monitoring:

```yaml
# backup_alerts.yml
groups:
  - name: ydb_backup_alerts
    rules:
      - alert: BackupTooOld
        expr: ydb_backup_latest_age_seconds / 3600 > 48
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "YDB backup is too old"
          description: "Collection {% raw %}{{ $labels.collection }}{% endraw %} has not been backed up for {% raw %}{{ $value }}{% endraw %} hours"
      
      - alert: BackupOperationStuck
        expr: ydb_backup_operations_running > 0 and rate(ydb_backup_collection_count[1h]) == 0
        for: 30m
        labels:
          severity: warning
        annotations:
          summary: "YDB backup operation appears stuck"
          description: "Backup operation running for {% raw %}{{ $labels.collection }}{% endraw %} but no progress detected"
      
      - alert: NoRecentBackups
        expr: increase(ydb_backup_collection_count[24h]) == 0
        for: 1h
        labels:
          severity: critical
        annotations:
          summary: "No YDB backups created in 24 hours"
          description: "Collection {% raw %}{{ $labels.collection }}{% endraw %} has not had any new backups in 24 hours"
```

## See also

- [Operations guide](../maintenance/manual/backup-collections.md) - Complete operational procedures
- [Concepts](../concepts/backup-collections.md) - Core concepts and architecture  
- [YQL reference](../yql/reference/syntax/backup-collections.md) - Complete SQL syntax documentation
- [CLI reference](../reference/ydb-cli/backup-collections.md) - Command-line tools and options
