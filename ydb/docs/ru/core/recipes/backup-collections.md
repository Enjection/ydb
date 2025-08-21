# Коллекции резервных копий: Распространенные рецепты и примеры

Данное руководство предоставляет практические примеры и рецепты для распространенных случаев использования коллекций резервных копий. Для базовых операций см. [руководство по операциям](../operations/backup-collections.md). Для полного синтаксиса см. [справочник YQL](../yql/reference/backup-collections.md).

## Автоматизированное ежедневное резервное копирование {#automated-daily-backups}

### Базовое расписание ежедневного резервного копирования

Настройка простой ежедневной процедуры резервного копирования с еженедельными полными резервными копиями:

```sql
-- Воскресенье: Полная резервная копия
CREATE BACKUP COLLECTION IF NOT EXISTS `daily_production_backups`
    ( TABLE `/Root/production/users`
    , TABLE `/Root/production/orders`
    , TABLE `/Root/production/products`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- Воскресенье (день 0): Полная резервная копия
BACKUP `daily_production_backups`;

-- Понедельник-Суббота: Инкрементальные резервные копии
BACKUP `daily_production_backups` INCREMENTAL;
```

### Shell-скрипт для автоматизированного резервного копирования

```bash
#!/bin/bash
# daily_backup.sh - Автоматизированное ежедневное резервное копирование с ротацией

COLLECTION_NAME="daily_production_backups"
DATABASE="/Root/production"
LOG_FILE="/var/log/backup/ydb_daily.log"
RETENTION_DAYS=30

# Функция для записи сообщений в лог
log_message() {
    echo "$(date '+%Y-%m-%d %H:%M:%S'): $1" | tee -a "$LOG_FILE"
}

# Функция для проверки, является ли день воскресеньем (день полной резервной копии)
is_sunday() {
    [ "$(date +%u)" -eq 7 ]
}

# Создание коллекции, если она не существует
create_collection() {
    log_message "Создание коллекции резервных копий $COLLECTION_NAME"
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        CREATE BACKUP COLLECTION IF NOT EXISTS \`$COLLECTION_NAME\`
            ( TABLE \`/Root/production/users\`
            , TABLE \`/Root/production/orders\`
            , TABLE \`/Root/production/products\`
            )
        WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
    "
}

# Выполнение резервного копирования
perform_backup() {
    local backup_type="$1"
    log_message "Начало ${backup_type} резервного копирования"
    
    if [ "$backup_type" = "полной" ]; then
        ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\`;"
    else
        ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;"
    fi
    
    if [ $? -eq 0 ]; then
        log_message "${backup_type} резервное копирование завершено успешно"
    else
        log_message "ОШИБКА: ${backup_type} резервное копирование завершилось неудачно"
        exit 1
    fi
}

# Очистка старых резервных копий
cleanup_old_backups() {
    log_message "Очистка резервных копий старше $RETENTION_DAYS дней"
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        DELETE FROM SYS.BACKUP_HISTORY 
        WHERE collection_name = '$COLLECTION_NAME' 
        AND created_at < DateTime::MakeDate(CurrentUtcDate() - Interval('P${RETENTION_DAYS}D'));
    "
}

# Основная логика
main() {
    log_message "Запуск скрипта ежедневного резервного копирования"
    
    create_collection
    
    if is_sunday; then
        perform_backup "полной"
    else
        perform_backup "инкрементальной"
    fi
    
    cleanup_old_backups
    
    log_message "Скрипт резервного копирования завершен"
}

# Запуск основной функции
main "$@"
```

### Настройка cron для автоматического выполнения

```bash
# Добавить в crontab для ежедневного выполнения в 2:00 AM
0 2 * * * /opt/backup/daily_backup.sh >> /var/log/backup/cron.log 2>&1
```

## Резервное копирование по микросервисам {#microservice-backups}

### Отдельные коллекции для каждого сервиса

Создание отдельных коллекций для разных микросервисов:

```sql
-- Сервис пользователей
CREATE BACKUP COLLECTION `user_service_backups`
    ( TABLE `/Root/users/profiles`
    , TABLE `/Root/users/sessions`
    , TABLE `/Root/users/preferences`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- Сервис заказов
CREATE BACKUP COLLECTION `order_service_backups`
    ( TABLE `/Root/orders/orders`
    , TABLE `/Root/orders/items`
    , TABLE `/Root/orders/payments`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- Сервис каталога
CREATE BACKUP COLLECTION `catalog_service_backups`
    ( TABLE `/Root/catalog/products`
    , TABLE `/Root/catalog/categories`
    , TABLE `/Root/catalog/inventory`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

### Координированное резервное копирование

```bash
#!/bin/bash
# microservice_backup.sh - Координированное резервное копирование микросервисов

SERVICES=("user_service" "order_service" "catalog_service")
BACKUP_TIMESTAMP=$(date "+%Y%m%d_%H%M%S")

# Функция для резервного копирования одного сервиса
backup_service() {
    local service_name="$1"
    local collection_name="${service_name}_backups"
    
    echo "Начало резервного копирования для $service_name"
    
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        BACKUP \`$collection_name\` INCREMENTAL;
    " && echo "✓ $service_name резервное копирование завершено" || {
        echo "✗ $service_name резервное копирование завершилось неудачно"
        return 1
    }
}

# Резервное копирование всех сервисов
for service in "${SERVICES[@]}"; do
    backup_service "$service"
done
```

## Аварийное восстановление {#disaster-recovery}

### Подготовка плана аварийного восстановления

```sql
-- 1. Создание коллекции для критически важных данных
CREATE BACKUP COLLECTION `disaster_recovery_critical`
    ( TABLE `/Root/critical/user_accounts`
    , TABLE `/Root/critical/financial_data`
    , TABLE `/Root/critical/system_config`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );

-- 2. Частое резервное копирование критически важных данных (каждые 4 часа)
-- Это должно выполняться через cron:
-- 0 */4 * * * /opt/backup/critical_backup.sh
```

### Скрипт аварийного восстановления

```bash
#!/bin/bash
# disaster_recovery.sh - Быстрое восстановление критически важных данных

COLLECTION_NAME="disaster_recovery_critical"
RECOVERY_TARGET="/Root/recovery"
BACKUP_TIMESTAMP="$1"

# Функция валидации
validate_parameters() {
    if [ -z "$BACKUP_TIMESTAMP" ]; then
        echo "Использование: $0 <backup_timestamp>"
        echo "Пример: $0 '2024-01-15T12:00:00Z'"
        exit 1
    fi
}

# Восстановление данных
restore_critical_data() {
    echo "Начало аварийного восстановления для временной метки: $BACKUP_TIMESTAMP"
    
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        RESTORE FROM \`$COLLECTION_NAME\` 
        AS OF SYSTEM TIME '$BACKUP_TIMESTAMP'
        TO \`$RECOVERY_TARGET/user_accounts\`,
           \`$RECOVERY_TARGET/financial_data\`,
           \`$RECOVERY_TARGET/system_config\`;
    "
    
    if [ $? -eq 0 ]; then
        echo "✓ Аварийное восстановление завершено успешно"
        echo "Данные восстановлены в: $RECOVERY_TARGET"
    else
        echo "✗ Аварийное восстановление завершилось неудачно"
        exit 1
    fi
}

# Основная логика
validate_parameters
restore_critical_data
```

## Восстановление на определенный момент времени {#point-in-time-recovery}

### Поиск подходящего момента времени

```sql
-- Найти доступные резервные копии в определенном диапазоне времени
SELECT 
    backup_id,
    created_at,
    backup_type,
    status
FROM SYS.BACKUP_HISTORY 
WHERE collection_name = 'production_backups'
  AND created_at BETWEEN '2024-01-15T00:00:00Z' AND '2024-01-15T23:59:59Z'
ORDER BY created_at DESC;
```

### Восстановление с точностью до минуты

```sql
-- Восстановление данных на точное время
RESTORE FROM `production_backups` 
AS OF SYSTEM TIME '2024-01-15T14:30:00Z'
TO `/Root/recovery/users_14_30`,
   `/Root/recovery/orders_14_30`,
   `/Root/recovery/products_14_30`;
```

### Скрипт восстановления с пользовательским интерфейсом

```bash
#!/bin/bash
# point_in_time_restore.sh - Интерактивное восстановление на момент времени

COLLECTION_NAME="$1"
TARGET_PATH="$2"

# Функция для отображения доступных резервных копий
show_available_backups() {
    echo "Доступные резервные копии для коллекции $COLLECTION_NAME:"
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        SELECT 
            backup_id,
            created_at,
            backup_type,
            status
        FROM SYS.BACKUP_HISTORY 
        WHERE collection_name = '$COLLECTION_NAME'
        ORDER BY created_at DESC
        LIMIT 20;
    "
}

# Функция для получения пользовательского ввода
get_restore_timestamp() {
    echo "Введите временную метку для восстановления (YYYY-MM-DDTHH:MM:SSZ):"
    read -r timestamp
    echo "$timestamp"
}

# Функция восстановления
perform_restore() {
    local timestamp="$1"
    
    echo "Начало восстановления на момент времени: $timestamp"
    echo "Целевой путь: $TARGET_PATH"
    
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        RESTORE FROM \`$COLLECTION_NAME\` 
        AS OF SYSTEM TIME '$timestamp'
        TO \`$TARGET_PATH\`;
    "
}

# Основная логика
if [ $# -lt 2 ]; then
    echo "Использование: $0 <collection_name> <target_path>"
    exit 1
fi

show_available_backups
timestamp=$(get_restore_timestamp)
perform_restore "$timestamp"
```

## Резервное копирование больших таблиц {#large-table-backups}

### Стратегия для больших наборов данных

```sql
-- Создание коллекции с оптимизацией для больших таблиц
CREATE BACKUP COLLECTION `large_data_backups`
    ( TABLE `/Root/analytics/events`      -- Большая таблица событий
    , TABLE `/Root/analytics/sessions`    -- Сессионные данные
    , TABLE `/Root/analytics/metrics`     -- Метрики
    )
WITH ( 
    STORAGE = 'cluster',
    INCREMENTAL_BACKUP_ENABLED = 'true',
    COMPRESSION = 'true',
    PARALLEL_WORKERS = 8
);
```

### Мониторинг прогресса резервного копирования

```bash
#!/bin/bash
# monitor_backup.sh - Мониторинг прогресса резервного копирования

COLLECTION_NAME="$1"

# Функция для отображения текущих операций
show_current_operations() {
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        SELECT 
            operation_id,
            collection_name,
            operation_type,
            status,
            started_at,
            progress_percent,
            estimated_completion
        FROM SYS.BACKUP_OPERATIONS 
        WHERE collection_name = '$COLLECTION_NAME'
          AND status IN ('RUNNING', 'PENDING')
        ORDER BY started_at DESC;
    "
}

# Мониторинг в реальном времени
echo "Мониторинг операций резервного копирования для коллекции: $COLLECTION_NAME"
echo "Нажмите Ctrl+C для выхода"

while true; do
    clear
    echo "=== Текущие операции резервного копирования ==="
    show_current_operations
    echo ""
    echo "Обновлено: $(date)"
    sleep 10
done
```

## Тестирование восстановления {#restore-testing}

### Автоматизированное тестирование восстановления

```bash
#!/bin/bash
# test_restore.sh - Автоматизированное тестирование процедур восстановления

COLLECTION_NAME="$1"
TEST_TARGET="/Root/test_restore"
LOG_FILE="/var/log/backup/restore_test.log"

# Функция для записи в лог
log_test() {
    echo "$(date): $1" | tee -a "$LOG_FILE"
}

# Функция очистки тестовых данных
cleanup_test_data() {
    log_test "Очистка тестовых данных"
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        DROP TABLE IF EXISTS \`$TEST_TARGET/users\`;
        DROP TABLE IF EXISTS \`$TEST_TARGET/orders\`;
    "
}

# Функция тестирования восстановления
test_restore() {
    log_test "Начало тестирования восстановления для коллекции: $COLLECTION_NAME"
    
    # Получение последней резервной копии
    latest_backup=$(ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        SELECT created_at 
        FROM SYS.BACKUP_HISTORY 
        WHERE collection_name = '$COLLECTION_NAME' 
          AND status = 'COMPLETED'
        ORDER BY created_at DESC 
        LIMIT 1;
    " | tail -n 1)
    
    log_test "Использование резервной копии от: $latest_backup"
    
    # Выполнение восстановления
    ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        RESTORE FROM \`$COLLECTION_NAME\` 
        AS OF SYSTEM TIME '$latest_backup'
        TO \`$TEST_TARGET/users\`,
           \`$TEST_TARGET/orders\`;
    "
    
    if [ $? -eq 0 ]; then
        log_test "✓ Тестирование восстановления прошло успешно"
        verify_restored_data
    else
        log_test "✗ Тестирование восстановления завершилось неудачно"
        return 1
    fi
}

# Функция верификации восстановленных данных
verify_restored_data() {
    log_test "Верификация восстановленных данных"
    
    # Проверка количества строк
    user_count=$(ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        SELECT COUNT(*) FROM \`$TEST_TARGET/users\`;
    " | tail -n 1)
    
    order_count=$(ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "
        SELECT COUNT(*) FROM \`$TEST_TARGET/orders\`;
    " | tail -n 1)
    
    log_test "Восстановлено пользователей: $user_count"
    log_test "Восстановлено заказов: $order_count"
    
    if [ "$user_count" -gt 0 ] && [ "$order_count" -gt 0 ]; then
        log_test "✓ Верификация данных прошла успешно"
    else
        log_test "✗ Верификация данных завершилась неудачно"
        return 1
    fi
}

# Основная логика
if [ $# -lt 1 ]; then
    echo "Использование: $0 <collection_name>"
    exit 1
fi

trap cleanup_test_data EXIT

log_test "Запуск автоматизированного теста восстановления"
test_restore
cleanup_test_data
log_test "Тест восстановления завершен"
```

## Резервное копирование по расписанию с уведомлениями {#scheduled-backups-notifications}

### Продвинутый скрипт с уведомлениями

```bash
#!/bin/bash
# advanced_backup.sh - Резервное копирование с уведомлениями

COLLECTION_NAME="$1"
SLACK_WEBHOOK_URL="$2"
EMAIL_RECIPIENTS="admin@company.com"
LOG_FILE="/var/log/backup/advanced_backup.log"

# Функция отправки уведомлений в Slack
notify_slack() {
    local message="$1"
    local color="$2"  # good, warning, danger
    
    if [ -n "$SLACK_WEBHOOK_URL" ]; then
        curl -X POST -H 'Content-type: application/json' \
            --data "{\"attachments\":[{\"color\":\"$color\",\"text\":\"$message\"}]}" \
            "$SLACK_WEBHOOK_URL"
    fi
}

# Функция отправки email уведомлений
notify_email() {
    local subject="$1"
    local message="$2"
    
    echo "$message" | mail -s "$subject" "$EMAIL_RECIPIENTS"
}

# Функция выполнения резервного копирования с уведомлениями
backup_with_notifications() {
    local start_time=$(date)
    local backup_type="incremental"
    
    # Определение типа резервной копии
    if [ "$(date +%u)" -eq 7 ]; then
        backup_type="full"
    fi
    
    notify_slack "🔄 Начато $backup_type резервное копирование коллекции: $COLLECTION_NAME" "warning"
    
    # Выполнение резервного копирования
    if [ "$backup_type" = "full" ]; then
        ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\`;"
    else
        ydb -e "$YDB_ENDPOINT" -d "$DATABASE" yql -s "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;"
    fi
    
    local end_time=$(date)
    local backup_result=$?
    
    if [ $backup_result -eq 0 ]; then
        local success_message="✅ $backup_type резервное копирование завершено успешно
Коллекция: $COLLECTION_NAME
Начато: $start_time
Завершено: $end_time"
        
        notify_slack "$success_message" "good"
        notify_email "Резервное копирование YDB - Успех" "$success_message"
    else
        local error_message="❌ $backup_type резервное копирование завершилось неудачно
Коллекция: $COLLECTION_NAME
Начато: $start_time
Завершено: $end_time
Код ошибки: $backup_result"
        
        notify_slack "$error_message" "danger"
        notify_email "Резервное копирование YDB - ОШИБКА" "$error_message"
    fi
    
    return $backup_result
}

# Основная логика
if [ $# -lt 1 ]; then
    echo "Использование: $0 <collection_name> [slack_webhook_url]"
    exit 1
fi

backup_with_notifications
```

## См. также {#see-also}

- [Концепции коллекций резервных копий](../concepts/backup-collections.md)
- [Операции с коллекциями резервных копий](../operations/backup-collections.md)
- [Справочник по YQL синтаксису для коллекций резервных копий](../yql/reference/backup-collections.md)
- [Команды YDB CLI для коллекций резервных копий](../reference/ydb-cli/backup-collections.md)
