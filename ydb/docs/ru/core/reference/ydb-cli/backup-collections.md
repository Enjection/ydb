# YDB CLI: Коллекции резервных копий

Данный раздел охватывает команды и инструменты YDB CLI для работы с коллекциями резервных копий. Для операций резервного копирования на основе SQL см. [справочник по синтаксису YQL](../../yql/reference/syntax/backup-collections.md).

## Обзор {#overview}

YDB CLI предоставляет несколько инструментов для работы с коллекциями резервных копий:

- **Выполнение SQL**: Выполнение SQL команд коллекций резервных копий
- **Просмотр схемы**: Навигация и изучение структур резервных копий
- **Инструменты экспорта/импорта**: Перемещение резервных копий между системами
- **Мониторинг операций**: Отслеживание прогресса операций резервного копирования

## Выполнение SQL команд {#sql-execution}

### Базовое выполнение SQL

Выполняйте SQL команды коллекций резервных копий с помощью команды `ydb yql`:

```bash
# Создание коллекции резервных копий
ydb yql -s "CREATE BACKUP COLLECTION \`shop_backups\`
    ( TABLE \`/Root/shop/orders\`, TABLE \`/Root/shop/products\` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );"

# Создание полной резервной копии
ydb yql -s "BACKUP \`shop_backups\`;"

# Создание инкрементальной резервной копии
ydb yql -s "BACKUP \`shop_backups\` INCREMENTAL;"

# Удаление коллекции
ydb yql -s "DROP BACKUP COLLECTION \`shop_backups\`;"
```

### Выполнение скриптов

Для автоматизированных операций резервного копирования используйте файлы SQL скриптов:

```bash
# Создание файла скрипта резервного копирования
cat > backup_script.sql << 'EOF'
-- Скрипт ежедневного резервного копирования
BACKUP `production_data` INCREMENTAL;
EOF

# Выполнение скрипта
ydb yql -f backup_script.sql
```

### Интерактивный режим

Запуск интерактивной SQL сессии для команд резервного копирования:

```bash
# Запуск интерактивного режима
ydb yql

# В интерактивном режиме выполните:
YQL> CREATE BACKUP COLLECTION `test_backups`
     ( TABLE `/Root/test/users` )
     WITH ( STORAGE = 'cluster' );

YQL> BACKUP `test_backups`;

YQL> \exit
```

## Мониторинг и инспекция {#monitoring-inspection}

### Просмотр коллекций резервных копий

Просмотрите существующие коллекции резервных копий:

```bash
# Список всех коллекций
ydb yql -s "SELECT * FROM SYS.BACKUP_COLLECTIONS;"

# Подробная информация о конкретной коллекции
ydb yql -s "SELECT 
    collection_name,
    tables,
    storage_settings,
    created_at,
    last_backup_at
FROM SYS.BACKUP_COLLECTIONS 
WHERE collection_name = 'shop_backups';"
```

### Мониторинг операций резервного копирования

Отслеживайте текущие и завершенные операции:

```bash
# Текущие операции
ydb yql -s "SELECT 
    operation_id,
    collection_name,
    operation_type,
    status,
    started_at,
    progress_percent
FROM SYS.BACKUP_OPERATIONS 
WHERE status IN ('RUNNING', 'PENDING');"

# История операций
ydb yql -s "SELECT 
    operation_id,
    collection_name,
    operation_type,
    status,
    started_at,
    completed_at,
    error_message
FROM SYS.BACKUP_OPERATIONS 
WHERE collection_name = 'shop_backups'
ORDER BY started_at DESC 
LIMIT 10;"
```

### Просмотр истории резервных копий

Изучите историю резервных копий для коллекции:

```bash
# История резервных копий
ydb yql -s "SELECT 
    backup_id,
    collection_name,
    backup_type,
    created_at,
    size_bytes,
    status
FROM SYS.BACKUP_HISTORY 
WHERE collection_name = 'shop_backups'
ORDER BY created_at DESC 
LIMIT 20;"

# Статистика резервных копий
ydb yql -s "SELECT 
    collection_name,
    COUNT(*) as total_backups,
    SUM(CASE WHEN backup_type = 'FULL' THEN 1 ELSE 0 END) as full_backups,
    SUM(CASE WHEN backup_type = 'INCREMENTAL' THEN 1 ELSE 0 END) as incremental_backups,
    SUM(size_bytes) as total_size
FROM SYS.BACKUP_HISTORY 
WHERE collection_name = 'shop_backups'
GROUP BY collection_name;"
```

## Операции восстановления {#restore-operations}

### Восстановление из CLI

Выполняйте операции восстановления с помощью YDB CLI:

```bash
# Восстановление всех таблиц из коллекции
ydb yql -s "RESTORE FROM \`shop_backups\` 
TO \`/Root/shop_restored/orders\`, \`/Root/shop_restored/products\`;"

# Восстановление на определенный момент времени
ydb yql -s "RESTORE FROM \`shop_backups\` 
AS OF SYSTEM TIME '2024-01-15T12:00:00Z'
TO \`/Root/shop_restored/orders\`, \`/Root/shop_restored/products\`;"

# Восстановление отдельной таблицы
ydb yql -s "RESTORE TABLE \`/Root/shop/orders\` FROM \`shop_backups\` 
TO \`/Root/shop_restored/orders\`;"
```

### Верификация восстановления

Проверьте результаты операций восстановления:

```bash
# Проверка количества строк в восстановленной таблице
ydb yql -s "SELECT COUNT(*) as row_count 
FROM \`/Root/shop_restored/orders\`;"

# Сравнение схем таблиц
ydb scheme describe /Root/shop/orders
ydb scheme describe /Root/shop_restored/orders

# Проверка целостности данных
ydb yql -s "SELECT 
    MIN(created_at) as earliest_date,
    MAX(created_at) as latest_date,
    COUNT(DISTINCT customer_id) as unique_customers
FROM \`/Root/shop_restored/orders\`;"
```

## Инструменты экспорта и импорта {#export-import-tools}

### Экспорт резервных копий

Экспортируйте резервные копии для архивирования или переноса:

```bash
# Экспорт коллекции резервных копий
ydb export backup-collection shop_backups \
    --output-dir /backup/exports \
    --format json

# Экспорт с компрессией
ydb export backup-collection shop_backups \
    --output-dir /backup/exports \
    --format json \
    --compress gzip
```

### Импорт резервных копий

Импортируйте резервные копии из экспортированных данных:

```bash
# Импорт коллекции резервных копий
ydb import backup-collection \
    --input-dir /backup/exports/shop_backups \
    --target-collection shop_backups_imported

# Импорт с переименованием
ydb import backup-collection \
    --input-dir /backup/exports/shop_backups \
    --target-collection shop_backups_test \
    --target-tables /Root/test/orders,/Root/test/products
```

## Автоматизация и скрипты {#automation-scripts}

### Bash скрипты для CLI операций

Автоматизируйте операции резервного копирования с помощью bash скриптов:

```bash
#!/bin/bash
# ydb_backup_automation.sh - Автоматизация резервного копирования через CLI

# Конфигурация
YDB_ENDPOINT="grpc://localhost:2136"
YDB_DATABASE="/Root/production"
COLLECTION_NAME="automated_backups"
LOG_FILE="/var/log/ydb/backup_cli.log"

# Функция для выполнения YDB команд с логированием
ydb_exec() {
    local query="$1"
    echo "$(date): Выполнение: $query" >> "$LOG_FILE"
    
    ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "$query" 2>&1 | tee -a "$LOG_FILE"
    local result=${PIPESTATUS[0]}
    
    if [ $result -eq 0 ]; then
        echo "$(date): ✓ Команда выполнена успешно" >> "$LOG_FILE"
    else
        echo "$(date): ✗ Команда завершилась с ошибкой (код: $result)" >> "$LOG_FILE"
    fi
    
    return $result
}

# Функция создания коллекции
create_collection() {
    echo "Создание коллекции резервных копий..."
    ydb_exec "CREATE BACKUP COLLECTION IF NOT EXISTS \`$COLLECTION_NAME\`
        ( TABLE \`/Root/production/users\`
        , TABLE \`/Root/production/orders\`
        )
    WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );"
}

# Функция выполнения резервного копирования
perform_backup() {
    local backup_type="$1"
    echo "Выполнение $backup_type резервного копирования..."
    
    if [ "$backup_type" = "full" ]; then
        ydb_exec "BACKUP \`$COLLECTION_NAME\`;"
    else
        ydb_exec "BACKUP \`$COLLECTION_NAME\` INCREMENTAL;"
    fi
}

# Функция проверки статуса
check_status() {
    echo "Проверка статуса последней операции..."
    ydb_exec "SELECT 
        operation_id,
        operation_type,
        status,
        started_at,
        completed_at
    FROM SYS.BACKUP_OPERATIONS 
    WHERE collection_name = '$COLLECTION_NAME'
    ORDER BY started_at DESC 
    LIMIT 1;"
}

# Основная логика
case "${1:-daily}" in
    "create")
        create_collection
        ;;
    "full")
        perform_backup "full"
        check_status
        ;;
    "incremental"|"daily")
        perform_backup "incremental"
        check_status
        ;;
    "status")
        check_status
        ;;
    *)
        echo "Использование: $0 {create|full|incremental|status}"
        exit 1
        ;;
esac
```

### Мониторинг с помощью CLI

Создайте скрипт мониторинга резервных копий:

```bash
#!/bin/bash
# backup_monitor.sh - Мониторинг резервных копий через CLI

# Конфигурация
YDB_ENDPOINT="grpc://localhost:2136"
YDB_DATABASE="/Root/production"
ALERT_EMAIL="admin@company.com"
WARNING_HOURS=26  # Предупреждение, если резервная копия старше 26 часов
CRITICAL_HOURS=50 # Критическое состояние, если резервная копия старше 50 часов

# Функция проверки последних резервных копий
check_backup_freshness() {
    local collection_name="$1"
    
    echo "Проверка свежести резервных копий для коллекции: $collection_name"
    
    # Получение времени последней резервной копии
    local last_backup=$(ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "
        SELECT 
            created_at,
            DATETIME_DIFF('hour', created_at, CurrentUtcDateTime()) as hours_ago
        FROM SYS.BACKUP_HISTORY 
        WHERE collection_name = '$collection_name' 
          AND status = 'COMPLETED'
        ORDER BY created_at DESC 
        LIMIT 1;
    " | tail -n 1)
    
    echo "Результат запроса: $last_backup"
    
    # Извлечение количества часов
    local hours_ago=$(echo "$last_backup" | awk '{print $2}')
    
    if [ -z "$hours_ago" ]; then
        echo "❌ КРИТИЧЕСКОЕ: Резервные копии для коллекции $collection_name не найдены!"
        send_alert "КРИТИЧЕСКОЕ" "Резервные копии для коллекции $collection_name не найдены!"
        return 2
    elif [ "$hours_ago" -gt "$CRITICAL_HOURS" ]; then
        echo "❌ КРИТИЧЕСКОЕ: Последняя резервная копия для $collection_name была $hours_ago часов назад"
        send_alert "КРИТИЧЕСКОЕ" "Последняя резервная копия для $collection_name была $hours_ago часов назад"
        return 2
    elif [ "$hours_ago" -gt "$WARNING_HOURS" ]; then
        echo "⚠️  ПРЕДУПРЕЖДЕНИЕ: Последняя резервная копия для $collection_name была $hours_ago часов назад"
        send_alert "ПРЕДУПРЕЖДЕНИЕ" "Последняя резервная копия для $collection_name была $hours_ago часов назад"
        return 1
    else
        echo "✅ OK: Последняя резервная копия для $collection_name была $hours_ago часов назад"
        return 0
    fi
}

# Функция отправки оповещений
send_alert() {
    local level="$1"
    local message="$2"
    
    echo "YDB Backup Alert [$level]: $message" | mail -s "YDB Backup Alert" "$ALERT_EMAIL"
}

# Функция проверки всех коллекций
check_all_collections() {
    echo "=== Проверка всех коллекций резервных копий ==="
    
    # Получение списка всех коллекций
    local collections=$(ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "
        SELECT collection_name 
        FROM SYS.BACKUP_COLLECTIONS;
    " | tail -n +2)  # Пропустить заголовок
    
    local overall_status=0
    
    while IFS= read -r collection; do
        if [ -n "$collection" ]; then
            check_backup_freshness "$collection"
            local status=$?
            if [ $status -gt $overall_status ]; then
                overall_status=$status
            fi
        fi
    done <<< "$collections"
    
    echo "=== Общий статус: $overall_status ==="
    return $overall_status
}

# Основная логика
if [ $# -eq 1 ]; then
    check_backup_freshness "$1"
else
    check_all_collections
fi
```

## Настройка подключения {#connection-setup}

### Переменные окружения

Настройте переменные окружения для упрощения работы:

```bash
# Добавьте в ~/.bashrc или ~/.zshrc
export YDB_ENDPOINT="grpc://localhost:2136"
export YDB_DATABASE="/Root/production"
export YDB_TOKEN="your_auth_token"

# Псевдонимы для частых команд
alias ydb-backup='ydb -e $YDB_ENDPOINT -d $YDB_DATABASE yql -s'
alias ydb-monitor='ydb -e $YDB_ENDPOINT -d $YDB_DATABASE yql -s "SELECT * FROM SYS.BACKUP_OPERATIONS WHERE status IN (\"RUNNING\", \"PENDING\");"'
```

### Файлы конфигурации

Создайте файл конфигурации для автоматизации:

```bash
# ~/.ydb/backup_config
YDB_ENDPOINT="grpc://localhost:2136"
YDB_DATABASE="/Root/production"
DEFAULT_COLLECTION="production_backups"
LOG_LEVEL="INFO"
NOTIFICATION_EMAIL="admin@company.com"
RETENTION_DAYS=30

# Использование в скриптах
source ~/.ydb/backup_config
```

## Устранение неполадок {#troubleshooting}

### Диагностика проблем

Используйте YDB CLI для диагностики проблем резервного копирования:

```bash
# Проверка состояния подключения
ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" discovery list

# Проверка прав доступа
ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "SELECT USER();"

# Проверка доступного места
ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" monitoring
```

### Частые проблемы и решения

1. **Ошибка прав доступа**:

   ```bash
   # Проверьте права пользователя
   ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "
   SELECT * FROM SYS.USER_PERMISSIONS 
   WHERE user_name = USER();"
   ```

2. **Нехватка места**:

   ```bash
   # Проверьте использование дискового пространства
   ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "
   SELECT * FROM SYS.STORAGE_USAGE;"
   ```

3. **Зависшие операции**:

   ```bash
   # Найдите долго выполняющиеся операции
   ydb -e "$YDB_ENDPOINT" -d "$YDB_DATABASE" yql -s "
   SELECT 
       operation_id,
       collection_name,
       status,
       started_at,
       DATETIME_DIFF('minute', started_at, CurrentUtcDateTime()) as minutes_running
   FROM SYS.BACKUP_OPERATIONS 
   WHERE status = 'RUNNING'
     AND DATETIME_DIFF('hour', started_at, CurrentUtcDateTime()) > 2;"
   ```

## Лучшие практики CLI {#cli-best-practices}

### Автоматизация

- Используйте переменные окружения для конфигурации
- Создавайте переиспользуемые скрипты для частых операций
- Реализуйте логирование и мониторинг
- Настройте системы оповещений

### Безопасность

- Храните токены аутентификации в безопасном месте
- Используйте минимально необходимые права доступа
- Регулярно ротируйте учетные данные
- Логируйте все операции для аудита

### Производительность

- Выполняйте резервное копирование в периоды низкой нагрузки
- Используйте инкрементальные резервные копии для больших данных
- Мониторьте использование ресурсов во время операций
- Оптимизируйте параллелизм для больших коллекций

## См. также {#see-also}

- [Концепции коллекций резервных копий](../../concepts/backup-collections.md)
- [Операции с коллекциями резервных копий](../../maintenance/manual/backup-collections.md)
- [Справочник по YQL синтаксису для коллекций резервных копий](../../yql/reference/syntax/backup-collections.md)
- [Рецепты по коллекциям резервных копий](../../recipes/backup-collections.md)
