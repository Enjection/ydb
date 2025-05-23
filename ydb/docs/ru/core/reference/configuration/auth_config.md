# Секция конфигурации `auth_config`

{{ ydb-short-name }} позволяет использовать различные способы аутентификации пользователей в системе. Настройки аутентификации и провайдеров аутентификации задаются в секции `auth_config` файла конфигурации {{ ydb-short-name }}.

## Конфигурация аутентификации локальных пользователей {{ ydb-short-name }} {#local-auth-config}

Подробнее об аутентификации [локальных пользователей](../../concepts/glossary.md#access-user) см. в разделе про [аутентификацию по логину и паролю](../../security/authentication.md#static-credentials). Для настройки аутентификации локальных пользователей по логину и паролю необходимо указать следующие параметры в секции `auth_config`:

#|
|| Параметр | Описание ||
|| use_login_provider
| Флаг разрешает аутентификацию локальных пользователей по auth-токенам, полученным в результате входа по логину и паролю. Процедура входа в {{ ydb-short-name }} — это обмен логина и пароля на аутентификационный токен.

Возможные значения:

- `true` — разрешает аутентификацию внутренних пользователей по аутентификационным токенам;
- `false` — запрещает аутентификацию внутренних пользователей по аутентификационным токенам.

Значение по умолчанию: `true`

{% note info %}

Для возможности создания и аутентификации внутренних пользователей параметры `use_login_provider` и `enable_login_authentication` должны иметь значение `true`. В противном случае внутренние пользователи не смогут аутентифицироваться в YDB.

{% endnote %}
    ||
|| enable_login_authentication
| Флаг разрешает создание локальных пользователей и получение для них аутентификационного токена в обмен на логин и пароль.

Возможные значения:
- `true` — разрешает создание внутренних пользователей и получение для них аутентификационного токена;
- `false` — запрещает создание внутренних пользователей и получение для них аутентификационного токена.

Значение по умолчанию: `true`
    ||
|| domain_login_only
| Флаг определяет границы прав доступа локальных пользователей в кластере {{ ydb-short-name }}.

Возможные значения:

- `true` — локальные пользователи {{ ydb-short-name }} существуют на уровне кластера и им могут назначаться права на доступ к множеству [баз данных](../../concepts/glossary.md#database).

- `false` — локальные пользователи могут существовать как на уровне кластера, так и на уровне каждой отдельной базы данных. Границы прав доступа локальных пользователей, созданных на уровне базы данных, ограничиваются базой данных, в которой они созданы.

Значение по умолчанию: `true`
    ||
|| login_token_expire_time
| Время жизни аутентификационного токена, созданного в обмен на логин и пароль локального пользователя.

Значение по умолчанию: `12h`
    ||
|#

### Конфигурация блокировки пользователя при неправильно введённом пароле {#account-lockout}

{{ ydb-short-name }} позволяет запретить пользователю аутентифицироваться, если он совершил несколько неудачных попыток ввода пароля. Для настройки условий блокировки пользователя необходимо заполнить секцию `account_lockout`.

Пример секции `account_lockout`:

```yaml
auth_config:
  #...
  account_lockout:
    attempt_threshold: 4
    attempt_reset_duration: "1h"
  #...
```

#|
|| Параметр | Описание ||
|| attempt_threshold
| Количество неверных попыток ввода пароля, после которых учётная запись пользователя временно блокируется. Если пользователь ввёл неправильный пароль указанное количество раз подряд, ему запрещается аутентифицироваться на время, заданное в параметре `attempt_reset_duration`.

Если параметр имеет значение `0`, число попыток ввода неправильного пароля не ограничено. После успешной аутентификации (ввода правильных имени пользователя и пароля), счетчик неуспешных попыток сбрасывается в значение 0.

Значение по умолчанию: `4`
    ||
|| attempt_reset_duration
| Период времени, в течение которого пользователь считается заблокированным. В течение этого периода пользователь не сможет аутентифицироваться в системе даже если введёт правильные имя пользователя и пароль. Период блокировки начинается с момента последней неверной попытки ввода пароля.

Если указано нулевое значение (`"0s"` - запись, эквивалентная 0 секунд), пользователь будет заблокирован на неограниченное время. В этом случае снять блокировку можно при помощи команды [ALTER USER ...  LOGIN](../../yql/reference/syntax/alter-user.md).

Минимальный интервал времени блокировки 1 секунда.

Поддерживаемые единицы измерения:

- Секунды. `30s`
- Минуты. `20m`
- Часы. `5h`
- Дни. `3d`

Не допускается комбинировать единицы измерения в одной строке. Например такая запись некорректна: `1d12h`. Такую запись нужно заменить на эквивалентную, например `36h`.

Значение по умолчанию: `1h`
    ||
|#

### Конфигурация требований к сложности пароля {#password-complexity}

{{ ydb-short-name }} позволяет аутентифицировать пользователей по логину и паролю. Подробнее см. в разделе [аутентификация по логину и паролю](../../security/authentication.md#static-credentials). Для повышения безопасности в {{ ydb-short-name }} предусмотрена возможность настройки сложности используемых паролей [локальных пользователей](../../concepts/glossary.md#access-user). Для конфигурирования требований к паролю необходимо описать секцию `password_complexity`.

Пример секции `password_complexity`:

```yaml
auth_config:
  #...
  password_complexity:
    min_length: 8
    min_lower_case_count: 1
    min_upper_case_count: 1
    min_numbers_count: 1
    min_special_chars_count: 1
    special_chars: "!@#$%^&*()_+{}|<>?="
    can_contain_username: false
  #...
```

#|
|| Параметр | Описание ||
|| min_length
| Минимальная длина пароля.

Значение по умолчанию: 0 (не ограничено)
    ||
|| min_lower_case_count
| Минимальное количество строчных букв в пароле.

Значение по умолчанию: 0 (не ограничено)
    ||
|| min_upper_case_count
| Минимальное количество прописных букв в пароле.

Значение по умолчанию: 0 (не ограничено)
    ||
|| min_numbers_count
| Минимальное количество цифр в пароле.

Значение по умолчанию: 0 (не ограничено)
    ||
|| min_special_chars_count
| Минимальное количество специальных символов в пароле из указанных в параметре `special_chars`.

Значение по умолчанию: 0 (не ограничено)
    ||
|| special_chars
| Перечень специальных символов, допустимых при задании пароля.

Валидные значения: `!@#$%^&*()_+{}\|<>?=`

Значение по умолчанию: пустая строка (допускает использование всех валидных специальных символов)
    ||
|| can_contain_username
| Флаг определяет допустимость включения имени пользователя в пароль.

Значение по умолчанию: `false`
    ||
|#

{% note info %}

Любые изменения политики паролей не затрагивают уже действующие пароли пользователей, поэтому изменять существующие пароли не требуется, они будут приниматься в текущем виде.

{% endnote %}

## Конфигурация LDAP аутентификации {#ldap-auth-config}

Одним из способов аутентификации пользователей в {{ ydb-short-name }} является использование [LDAP](https://ru.wikipedia.org/wiki/LDAP)-каталога. Подробнее о таком виде аутентификации написано в разделе про [использование LDAP-каталога](../../security/authentication.md#ldap). Для конфигурирования LDAP-аутентификации необходимо описать секцию `ldap_authentication`.

Пример секции `ldap_authentication`:

```yaml
auth_config:
  ...
  ldap_authentication:
    hosts:
      - "ldap-hostname-01.example.net"
      - "ldap-hostname-02.example.net"
      - "ldap-hostname-03.example.net"
    port: 389
    base_dn: "dc=mycompany,dc=net"
    bind_dn: "cn=serviceAccaunt,dc=mycompany,dc=net"
    bind_password: "serviceAccauntPassword"
    search_filter: "uid=$username"
    use_tls:
      enable: true
      ca_cert_file: "/path/to/ca.pem"
      cert_require: DEMAND
  ldap_authentication_domain: "ldap"
  scheme: "ldap"
  requested_group_attribute: "memberOf"
  extended_settings:
      enable_nested_groups_search: true

  refresh_time: "1h"
  ...
```

#|
|| Параметр | Описание ||
|| `hosts`
| Список имен хостов, на котором работает LDAP-сервер
    ||
|| `port`
| Порт для подключения к LDAP-серверу
    ||
|| `base_dn`
| Корень поддерева в LDAP-каталоге, начиная с которого будет производиться поиск записи пользователя
    ||
|| `bind_dn`
| Отличительное имя (Distinguished Name, DN) сервисного аккаунта, от имени которого выполняется поиск записи пользователя
    ||
|| `bind_password`
| Пароль сервисного аккаунта, от имени которого выполняется поиск записи пользователя
    ||
|| `search_filter`
| Фильтр для поиска записи пользователя в LDAP-каталоге. В строке фильтра может встречаться последовательность символов *$username*, которая будет заменена на имя пользователя, запрошенное для аутентификации в базе данных
    ||
|| `use_tls`
| Настройки для конфигурирования TLS-соединения между {{ ydb-short-name }} и LDAP-сервером
    ||
|| `enable`
| Определяет, будет ли произведена попытка установить TLS-соединение с [использованием запроса `StartTls`](../../security/authentication.md#starttls). При установке значения этого параметра в `true`, необходимо отключить использование схемы соединения `ldaps`, присвоив параметру `ldap_authentication.scheme` значение `ldap`
    ||
|| `ca_cert_file`
| Путь до файла сертификата удостоверяющего центра
    ||
|| `cert_require`
| Уровень требований к сертификату LDAP-сервера.

Возможные значения:

- `NEVER` - {{ ydb-short-name }} не запрашивает сертификат или проверку проходит любой сертификат.
- `ALLOW` - {{ ydb-short-name }} требует, что бы LDAP-сервер предоставил сертификат. Если предоставленному сертификату нельзя доверять, TLS-сессия все равно установится.
- `TRY` - {{ ydb-short-name }} требует, что бы LDAP-сервер предоставил сертификат. Если предоставленному сертификату нельзя доверять, установление TLS-соединения прекращается.
- `DEMAND` и `HARD` - Эти требования эквивалентны параметру `TRY`.

Значение по умолчанию: `DEMAND`
    ||
|| `ldap_authentication_domain`
| Идентификатор, прикрепляемый к имени пользователя, позволяющий отличать пользователей из LDAP-каталога от пользователей аутентифицируемых с помощью других провайдеров.

Значение по умолчанию: `ldap`
    ||
|| `scheme`
| Схема соединения с LDAP-сервером.

Возможные значения:

- `ldap` — {{ ydb-short-name }} будет выполнять соединение с LDAP-сервером без какого-либо шифрования. Пароли будут отправляться на LDAP-сервер в открытом виде.
- `ldaps` — {{ ydb-short-name }} будет выполнять зашифрованное соединение с LDAP-сервером по протоколу TLS с самого первого запроса. Для успешного установления соединения по схеме `ldaps` необходимо отключить использование [запроса `StartTls`](../../security/authentication.md#starttls) в секции `ldap_authentication.use_tls.enable: false` и заполнить информацию о сертификате `ldap_authentication.use_tls.ca_cert_file` и уровне требования сертификата `ldap_authentication.use_tls.cert_require`.
- При использовании любого другого значения будет браться значение по умолчанию - `ldap`.

Значение по умолчанию: `ldap`
    ||
|| `requested_group_attribute`
| Атрибут обратного членства в группе. По умолчанию `memberOf`
    ||
|| `extended_settings.enable_nested_groups_search`
| Флаг определяет, будет ли выполнятся запрос для получения всего дерева групп, в которые входят непосредственные группы пользователя.

Возможные значения:

- `true` — {{ ydb-short-name }} запрашивает информацию о всех группах, в которые входят непосредственные группы пользователя. Запросы о всех родительских группах могут занимать много времени.
- `false` — {{ ydb-short-name }} запрашивает плоский список групп пользователя. Такой запрос не получает информацию о возможных вложенных родительских группах.

Значение по умолчанию: `false`
    ||
|| `host`
| Имя хоста, на котором работает LDAP-сервер. Это устаревший параметр, вместо него должен использоваться параметр `hosts`
    ||
|#

## Конфигурация аутентификации с использованием стороннего IAM-провайдера {#iam-auth-config}

{{ ydb-short-name }} поддерживает аутентификацию пользователей с использованием сервиса [Yandex Identity and Access Management (IAM)](https://yandex.cloud/en/services/iam), который используется в Yandex Cloud, или другого сервиса, совместимого с ним по API. Для конфигурирования IAM-аутентификации необходимо определить следующие параметры:

#|
|| Параметр | Описание ||
|| use_access_service
| Флаг разрешает аутентификацию пользователей в Yandex Cloud через IAM с использованием AccessService.

Значение по умолчанию: `false`
    ||
|| access_service_endpoint
| Адрес, по которому отправляются запросы в AccessService (IAM).

Значение по умолчанию: `as.private-api.cloud.yandex.net:4286`
    ||
|| use_access_service_tls
| Флаг включает использование TLS-соединений между {{ ydb-short-name }} и AccessService.

Значение по умолчанию: `true`
    ||
|| access_service_domain
| Суффикс «источника пользователя» в [SID](../../concepts/glossary.md#access-sid) для пользователей, приходящих в {{ ydb-short-name }} из Yandex Cloud IAM.

Значение по умолчанию: `as` ("access service")
    ||
|| path_to_root_ca
| Путь до файла сертификата удостоверяющего центра, используемого для взаимодействия с AccessService.

Значение по умолчанию: `/etc/ssl/certs/YandexInternalRootCA.pem`
    ||
|| access_service_grpc_keep_alive_time_ms
| Период времени, в миллисекундах, по истечении которого {{ ydb-short-name }} посылает keepalive ping IAM-серверу, чтобы сохранить соединение.

Значение по умолчанию: `10000`
    ||
|| access_service_grpc_keep_alive_timeout_ms
| Период времени ожидания ответа от IAM-сервера на keepalive ping, в миллисекундах. Если по истечении срока ожидания ответ от IAM-сервера не приходит, {{ ydb-short-name }} закрывает соединение.

Значение по умолчанию: `1000`
    ||
|| use_access_service_api_key
| Флаг разрешает использование API-ключей IAM. API-ключ — это секретный ключ, выписываемый в Yandex Cloud IAM для упрощённой авторизации сервисных аккаунтов в API Yandex Cloud. Его применяют, если нет возможности автоматически запрашивать IAM-токен.

Значение по умолчанию: `false`
    ||
|#

## Настройки кеширования результатов аутентификации

В процессе аутентификации пользовательская сессия получает аутентификационной токен, который передается вместе с каждым запросом к кластеру {{ ydb-short-name }}. Так как {{ ydb-short-name }} — это распределенная система, то пользовательские запросы будут в конечном итоге обрабатываться на одном или нескольких узлах {{ ydb-short-name }}. Каждый узел {{ ydb-short-name }}, получив запрос от пользователя, проводит верификацию аутентификационного токена, и в случае успешной проверки, генерирует **токен пользователя**, который действует только внутри текущего узла {{ ydb-short-name }} и используется для авторизации запрошенных пользователем действий. В рамках последующих запросов с тем же самым аутентификационным токеном на этот же узел {{ ydb-short-name }} уже не требуют верификации аутентификационного токена, а выполняют под токеном пользователя.

Время жизни и другие важные аспекты работы токена пользователя настраиваются в конфигурации {{ ydb-short-name }} с помощью следующих параметров:


#|
|| refresh_period
| Определяет как часто узел {{ ydb-short-name }} сканирует токены пользователей в кэше на достижение временных лимитов, указанных в параметрах `refresh_time`, `life_time` и `expire_time`, после которых требуется обновление или удаление токена. Чем короче указанный интервал проверки токенов пользователей, тем выше нагрузка на CPU.

Значение по умолчанию: `1s`
    ||
|| refresh_time
| Определяет время, прошедшее с последнего обновления, когда узел {{ ydb-short-name }} попытается обновить токен пользователя. Конкретное время обновления будет лежать в интервале от `refresh_time/2` до `refresh_time`.

Значение по умолчанию: `1h`
    ||
|| life_time
| Период хранения токена пользователя в кэше узла {{ ydb-short-name }} с момента его последнего использования. Если запросы от пользователя, для которого создан токен, не приходили на узел {{ ydb-short-name }} в течение указанного периода, узел удаляет этот токен пользователя из своего кэша.

Значение по умолчанию: `1h`
    ||
|| expire_time
| Период истечения срока действия токена пользователя, после которого токен удаляется из кэша узла {{ ydb-short-name }}. Удаление происходит независимо от периода, указанного в параметре `life_time`.

{% note warning %}

Если сторонняя система успешно аутентифицировалась на узле {{ ydb-short-name }} и с регулярностью чаще интервала `life_time` посылает запросы на тот же узел, {{ ydb-short-name }} гарантированно определит возможное удаление или изменение привилегий аккаунта пользователя только по истечении срока `expire_time`.

{% endnote %}

Чем короче указанный период, тем чаще узел {{ ydb-short-name }} заново аутентифицирует пользователей и обновляет их привилегии. Однако, чрезмерно частое повторение аутентификации пользователей замедляет работу {{ ydb-short-name }}, особенно если речь идёт о внешних пользователях. Установка значения этого параметра в секундах нивелирует работу кэша для токенов пользователей.

Значение по умолчанию: `24h`
    ||
|| min_error_refresh_time
| Минимальный период, после которого повторяется попытка обновить токен пользователя, при получении которого уже происходила ошибка (временный сбой).

Совместно с параметром `max_error_refresh_time`, определяет границы для выбора задержки перед повторной попыткой обновить токен пользователя, при получении которого происходила ошибка. Каждая последующая задержка увеличивается, пока не достигнет значения `max_error_refresh_time`. Попытки обновить токен пользователя продолжаются до успешного обновления или до окончания периода `expire_time`.

{% note warning %}

Не рекомендуется выставлять значение параметра в `0`, так как немедленные повторы создают избыточную нагрузку.

{% endnote %}

Значение по умолчанию: `1s`
    ||
|| max_error_refresh_time
| Максимальный период, до истечения которого повторяется попытка обновить токен пользователя, при получении которого происходила ошибка (временный сбой).

Совместно с параметром `min_error_refresh_time`, определяет границы для выбора задержки перед повторной попыткой обновить токен пользователя, при получении которого происходила ошибка. Каждая последующая задержка увеличивается, пока не достигнет значения `max_error_refresh_time`. Попытки обновить токен пользователя продолжаются до успешного обновления или до окончания периода `expire_time`.

Значение по умолчанию: `1m`
    ||
|#
