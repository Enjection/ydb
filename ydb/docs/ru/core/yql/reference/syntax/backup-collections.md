# Команды коллекций резервных копий

YQL поддерживает SQL команды для управления [коллекциями резервных копий](../../../concepts/backup-collections.md). Эти команды позволяют создавать, управлять и создавать резервные копии таблиц базы данных с использованием декларативного SQL синтаксиса.

Для практических примеров использования и операционного руководства см. [руководство по операциям с коллекциями резервных копий](../../../maintenance/manual/backup-collections.md).

## CREATE BACKUP COLLECTION {#create-backup-collection}

Создает новую коллекцию резервных копий с указанными таблицами и конфигурацией.

### Синтаксис

```sql
CREATE BACKUP COLLECTION <collection_name>
    ( TABLE <table_path> [, TABLE <table_path>]... )
WITH ( STORAGE = '<storage_backend>'
     [, INCREMENTAL_BACKUP_ENABLED = '<true|false>']
     [, <additional_options>] );
```

### Параметры

- **collection_name**: Уникальный идентификатор коллекции (должен быть заключен в обратные кавычки).
- **table_path**: Абсолютный путь к таблице для включения в коллекцию.
- **STORAGE**: Тип бэкенда хранения (в настоящее время поддерживает 'cluster').
- **INCREMENTAL_BACKUP_ENABLED**: Включить или отключить поддержку инкрементального резервного копирования.

### Примеры

**Базовое создание коллекции:**

```sql
CREATE BACKUP COLLECTION `my_backups`
    ( TABLE `/Root/database/users` )
WITH ( STORAGE = 'cluster' );
```

**Коллекция с инкрементальными резервными копиями (рекомендуется):**

```sql
CREATE BACKUP COLLECTION `shop_backups`
    ( TABLE `/Root/shop/orders`, TABLE `/Root/shop/products` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

**Коллекция с несколькими таблицами:**

```sql
CREATE BACKUP COLLECTION `complete_database`
    ( TABLE `/Root/app/users`
    , TABLE `/Root/app/orders`
    , TABLE `/Root/app/products`
    , TABLE `/Root/app/sessions`
    )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

**Условное создание коллекции:**

```sql
CREATE BACKUP COLLECTION IF NOT EXISTS `daily_backups`
    ( TABLE `/Root/production/critical_data` )
WITH ( STORAGE = 'cluster', INCREMENTAL_BACKUP_ENABLED = 'true' );
```

## BACKUP {#backup}

Создает резервную копию в существующей коллекции. Первая резервная копия всегда является полной; последующие резервные копии могут быть инкрементальными.

### Синтаксис

```sql
-- Полная резервная копия (первая резервная копия или принудительная полная)
BACKUP <collection_name>;

-- Инкрементальная резервная копия
BACKUP <collection_name> INCREMENTAL;
```

### Параметры

- **collection_name**: Имя существующей коллекции резервных копий.
- **INCREMENTAL**: Указывает, что должна быть создана инкрементальная резервная копия.

### Примеры

**Создание первой (полной) резервной копии:**

```sql
BACKUP `shop_backups`;
```

**Создание инкрементальной резервной копии:**

```sql
BACKUP `shop_backups` INCREMENTAL;
```

**Принудительная полная резервная копия:**

```sql
-- Создает полную резервную копию, даже если предыдущие резервные копии существуют
BACKUP `shop_backups`;
```

## RESTORE {#restore}

Восстанавливает данные из коллекции резервных копий в указанные таблицы.

### Синтаксис

```sql
-- Восстановление всех таблиц из последней резервной копии
RESTORE FROM <collection_name>
TO <target_table_path> [, <target_table_path>]...;

-- Восстановление на определенный момент времени
RESTORE FROM <collection_name>
AS OF SYSTEM TIME '<timestamp>'
TO <target_table_path> [, <target_table_path>]...;

-- Восстановление отдельной таблицы
RESTORE TABLE <source_table_path> FROM <collection_name>
TO <target_table_path>;
```

### Параметры

- **collection_name**: Имя коллекции резервных копий для восстановления.
- **timestamp**: Временная метка в формате ISO 8601 для восстановления на определенный момент времени.
- **source_table_path**: Путь к исходной таблице в коллекции (для восстановления отдельной таблицы).
- **target_table_path**: Путь, где должна быть восстановлена таблица.

### Примеры

**Восстановление всех таблиц из последней резервной копии:**

```sql
RESTORE FROM `shop_backups`
TO `/Root/shop_restored/orders`, `/Root/shop_restored/products`;
```

**Восстановление на определенный момент времени:**

```sql
RESTORE FROM `shop_backups`
AS OF SYSTEM TIME '2024-01-15T12:00:00Z'
TO `/Root/shop_recovered/orders`, `/Root/shop_recovered/products`;
```

**Восстановление отдельной таблицы:**

```sql
RESTORE TABLE `/Root/shop/orders` FROM `shop_backups`
TO `/Root/shop_test/orders`;
```

**Восстановление с переименованием:**

```sql
RESTORE FROM `production_backups`
TO `/Root/staging/users`, `/Root/staging/orders`;
```

## ALTER BACKUP COLLECTION {#alter-backup-collection}

Изменяет существующую коллекцию резервных копий, добавляя или удаляя таблицы.

### Синтаксис

```sql
-- Добавление таблиц
ALTER BACKUP COLLECTION <collection_name>
ADD TABLE <table_path> [, TABLE <table_path>]...;

-- Удаление таблиц
ALTER BACKUP COLLECTION <collection_name>
DROP TABLE <table_path> [, TABLE <table_path>]...;

-- Изменение настроек
ALTER BACKUP COLLECTION <collection_name>
SET ( <option> = '<value>' [, <option> = '<value>']... );
```

### Примеры

**Добавление таблиц в существующую коллекцию:**

```sql
ALTER BACKUP COLLECTION `shop_backups`
ADD TABLE `/Root/shop/customers`, TABLE `/Root/shop/reviews`;
```

**Удаление таблиц из коллекции:**

```sql
ALTER BACKUP COLLECTION `shop_backups`
DROP TABLE `/Root/shop/temp_data`;
```

**Изменение настроек коллекции:**

```sql
ALTER BACKUP COLLECTION `shop_backups`
SET ( INCREMENTAL_BACKUP_ENABLED = 'false' );
```

## DROP BACKUP COLLECTION {#drop-backup-collection}

Удаляет коллекцию резервных копий и все связанные с ней резервные копии.

### Синтаксис

```sql
DROP BACKUP COLLECTION <collection_name>;

-- Условное удаление
DROP BACKUP COLLECTION IF EXISTS <collection_name>;
```

### Примеры

**Удаление коллекции:**

```sql
DROP BACKUP COLLECTION `old_backups`;
```

**Условное удаление коллекции:**

```sql
DROP BACKUP COLLECTION IF EXISTS `temporary_backups`;
```

**Предупреждение**: Удаление коллекции также удаляет все связанные резервные копии. Эта операция необратима.

## Системные представления {#system-views}

YDB предоставляет системные представления для мониторинга и управления коллекциями резервных копий:

### SYS.BACKUP_COLLECTIONS

Содержит информацию обо всех коллекциях резервных копий:

```sql
SELECT 
    collection_name,
    tables,
    storage_settings,
    created_at,
    last_backup_at
FROM SYS.BACKUP_COLLECTIONS;
```

### SYS.BACKUP_HISTORY

Содержит историю всех резервных копий:

```sql
SELECT 
    backup_id,
    collection_name,
    backup_type,
    created_at,
    size_bytes,
    status
FROM SYS.BACKUP_HISTORY
WHERE collection_name = 'shop_backups'
ORDER BY created_at DESC;
```

### SYS.BACKUP_OPERATIONS

Содержит информацию о текущих и завершенных операциях:

```sql
SELECT 
    operation_id,
    collection_name,
    operation_type,
    status,
    started_at,
    completed_at,
    progress_percent,
    error_message
FROM SYS.BACKUP_OPERATIONS
WHERE status = 'RUNNING';
```

## Рекомендации по использованию {#usage-recommendations}

### Именование коллекций

- Используйте описательные имена, указывающие назначение
- Включайте информацию об окружении (prod, staging, test)
- Придерживайтесь консистентного стиля именования

```sql
-- Хорошие примеры имен коллекций
CREATE BACKUP COLLECTION `production_user_data`...;
CREATE BACKUP COLLECTION `staging_analytics_tables`...;
CREATE BACKUP COLLECTION `daily_transaction_logs`...;
```

### Планирование резервного копирования

- Начните с полной резервной копии
- Используйте инкрементальные резервные копии для регулярного обновления
- Планируйте полные резервные копии еженедельно или ежемесячно

```sql
-- Воскресенье: полная резервная копия
BACKUP `production_data`;

-- Понедельник-Суббота: инкрементальные резервные копии
BACKUP `production_data` INCREMENTAL;
```

### Восстановление и тестирование

- Регулярно тестируйте процедуры восстановления
- Восстанавливайте в отдельные пути для тестирования
- Верифицируйте целостность восстановленных данных

```sql
-- Тестирование восстановления в отдельной области
RESTORE FROM `production_backups`
TO `/Root/test_restore/users`, `/Root/test_restore/orders`;
```

## Ограничения и особенности {#limitations-considerations}

### Текущие ограничения

- Коллекции резервных копий поддерживают только регулярные таблицы
- Временные таблицы и представления не поддерживаются
- Межбазовое восстановление требует дополнительной настройки

### Рекомендации по производительности

- Группируйте связанные таблицы в одну коллекцию
- Избегайте слишком больших коллекций (более 100 таблиц)
- Планируйте резервное копирование в периоды низкой нагрузки

### Безопасность

- Убедитесь, что у пользователей есть соответствующие права доступа
- Рассмотрите шифрование резервных копий для чувствительных данных
- Ведите аудиторский след операций резервного копирования

## См. также {#see-also}

- [Концепции коллекций резервных копий](../../../concepts/backup-collections.md)
- [Операции с коллекциями резервных копий](../../../maintenance/manual/backup-collections.md)
- [Команды YDB CLI для коллекций резервных копий](../../../reference/ydb-cli/backup-collections.md)
- [Рецепты по коллекциям резервных копий](../../../recipes/backup-collections.md)
