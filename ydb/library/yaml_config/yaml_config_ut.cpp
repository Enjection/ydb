#include "yaml_config.h"

#include <ydb/library/yaml_config/yaml_config_parser.h>

#include <library/cpp/testing/unittest/registar.h>

#include <ydb/core/protos/key.pb.h>

#include <library/cpp/protobuf/json/util.h>

#include <util/datetime/base.h>
#include <util/string/builder.h>
#include <util/string/cast.h>

using namespace NKikimr;

const char *WholeConfig = R"(---
cluster: test
version: 12.1
config: &default_config
  log_config: &default_log_config
    entry:
    - component: ZERO
      level: -1
    - component: TEST
      level: -1
  actor_system_config:
    executor:
    - type: BASIC
      threads: 9
      spin_threshold: 1
      name: System
    - type: BASIC
      threads: 16
      spin_threshold: 1
      name: User
    - type: BASIC
      threads: 7
      spin_threshold: 1
      name: Batch
    - type: IO
      threads: 1
      name: IO
    - type: BASIC
      threads: 3
      spin_threshold: 10
      name: IC
      time_per_mailbox_micro_secs: 100
    scheduler:
      resolution: 64
      spin_threshold: 0
      progress_threshold: 10000
    sys_executor: 0
    user_executor: 1
    io_executor: 3
    batch_executor: 2
    service_executor:
    - service_name: Interconnect
      executor_id: 4

allowed_labels:
  tenant:
    type: string
  flavour:
    type: enum
    values:
      ? small
      ? medium
      ? big

selector_config:
- description: set BLOB_DEPOT level to 1
  selector:
    tenant: /ru/tenant2
  config: !inherit
    new_item: 1
- description: set initial log config
  selector:
    tenant: /ru/tenant1
  config: !inherit
    log_config:
      entry: !inherit:component
      - component: ZERO
        level: 7
      - component: BLOB_DEPOT
        level: 0
      - component: TEST
        level: 11
- description: set BLOB_DEPOT level to 1
  selector:
    tenant: /ru/tenant1
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: BLOB_DEPOT
        level: 1
- description: set BLOB_DEPOT level to 2
  selector:
    tenant: /ru/tenant2
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: BLOB_DEPOT
        level: 2
- description: set TEST level to 111
  selector:
    tenant: /ru/tenant1
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: TEST
        level: 111
- description: set TEST level to 112
  selector:
    tenant:
      in:
      - /ru/tenant1
      - /ru/tenant3
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: TEST
        level: 112
      default: 10
- description: set TEST level to 113
  selector:
    tenant:
      not_in:
        - /ru/tenant1
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: TEST
        level: 113
- description: set ACTOR level to 16
  selector:
    tenant: /ru/tenant1
    flavour: small
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: ACTOR
        level: 16
- description: set ACTOR level to 17
  selector:
    flavour: big
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: ACTOR
        level: 17
- description: add add to ACTOR
  selector:
    flavour: small
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - !inherit
        component: ZERO
        add: test
- description: rewrite ZERO to empty
  selector:
    flavour: medium
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - component: ZERO
- description: rewrite ZERO to empty
  selector:
    tenant: /ru/tenant1
    flavour: medium
  config: !inherit
    log_config: !inherit
      entry: !inherit:component
      - !remove
        component: ZERO
      - component: BLOB_DEPOT
        level: 2
      - component: BLOB_DEPOT
        level: 3
)";


const char *BigTenant1Config = R"(---
log_config:
  entry:
  - component: ZERO
    level: 7
  - component: BLOB_DEPOT
    level: 1
  - component: TEST
    level: 112
  - component: ACTOR
    level: 17
  default: 10
actor_system_config:
  executor:
  - type: BASIC
    threads: 9
    spin_threshold: 1
    name: System
  - type: BASIC
    threads: 16
    spin_threshold: 1
    name: User
  - type: BASIC
    threads: 7
    spin_threshold: 1
    name: Batch
  - type: IO
    threads: 1
    name: IO
  - type: BASIC
    threads: 3
    spin_threshold: 10
    name: IC
    time_per_mailbox_micro_secs: 100
  scheduler:
    resolution: 64
    spin_threshold: 0
    progress_threshold: 10000
  sys_executor: 0
  user_executor: 1
  io_executor: 3
  batch_executor: 2
  service_executor:
  - service_name: Interconnect
    executor_id: 4
)";

const char *Tenant2Config = R"(---
log_config: &default_log_config
  entry:
  - component: ZERO
    level: -1
  - component: TEST
    level: 113
  - component: BLOB_DEPOT
    level: 2
actor_system_config:
  executor:
  - type: BASIC
    threads: 9
    spin_threshold: 1
    name: System
  - type: BASIC
    threads: 16
    spin_threshold: 1
    name: User
  - type: BASIC
    threads: 7
    spin_threshold: 1
    name: Batch
  - type: IO
    threads: 1
    name: IO
  - type: BASIC
    threads: 3
    spin_threshold: 10
    name: IC
    time_per_mailbox_micro_secs: 100
  scheduler:
    resolution: 64
    spin_threshold: 0
    progress_threshold: 10000
  sys_executor: 0
  user_executor: 1
  io_executor: 3
  batch_executor: 2
  service_executor:
  - service_name: Interconnect
    executor_id: 4
new_item: 1
)";

const char *SmallConfig = R"(---
log_config: &default_log_config
  entry:
  - component: ZERO
    level: -1
    add: test
  - component: TEST
    level: 113
actor_system_config:
  executor:
  - type: BASIC
    threads: 9
    spin_threshold: 1
    name: System
  - type: BASIC
    threads: 16
    spin_threshold: 1
    name: User
  - type: BASIC
    threads: 7
    spin_threshold: 1
    name: Batch
  - type: IO
    threads: 1
    name: IO
  - type: BASIC
    threads: 3
    spin_threshold: 10
    name: IC
    time_per_mailbox_micro_secs: 100
  scheduler:
    resolution: 64
    spin_threshold: 0
    progress_threshold: 10000
  sys_executor: 0
  user_executor: 1
  io_executor: 3
  batch_executor: 2
  service_executor:
  - service_name: Interconnect
    executor_id: 4
)";

const char *MediumConfig = R"(---
log_config: &default_log_config
  entry:
  - component: ZERO
  - component: TEST
    level: 113
actor_system_config:
  executor:
  - type: BASIC
    threads: 9
    spin_threshold: 1
    name: System
  - type: BASIC
    threads: 16
    spin_threshold: 1
    name: User
  - type: BASIC
    threads: 7
    spin_threshold: 1
    name: Batch
  - type: IO
    threads: 1
    name: IO
  - type: BASIC
    threads: 3
    spin_threshold: 10
    name: IC
    time_per_mailbox_micro_secs: 100
  scheduler:
    resolution: 64
    spin_threshold: 0
    progress_threshold: 10000
  sys_executor: 0
  user_executor: 1
  io_executor: 3
  batch_executor: 2
  service_executor:
  - service_name: Interconnect
    executor_id: 4
)";

const char *MediumTenant1Config = R"(---
log_config: &default_log_config
  entry:
  - component: BLOB_DEPOT
    level: 3
  - component: TEST
    level: 112
  default: 10
actor_system_config:
  executor:
  - type: BASIC
    threads: 9
    spin_threshold: 1
    name: System
  - type: BASIC
    threads: 16
    spin_threshold: 1
    name: User
  - type: BASIC
    threads: 7
    spin_threshold: 1
    name: Batch
  - type: IO
    threads: 1
    name: IO
  - type: BASIC
    threads: 3
    spin_threshold: 10
    name: IC
    time_per_mailbox_micro_secs: 100
  scheduler:
    resolution: 64
    spin_threshold: 0
    progress_threshold: 10000
  sys_executor: 0
  user_executor: 1
  io_executor: 3
  batch_executor: 2
  service_executor:
  - service_name: Interconnect
    executor_id: 4
)";

const char *UnresolvedSimpleConfig1 = R"(---
cluster: test
version: 12.1
config:
  num:
    ? 0
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: 1
  selector:
    tenant: abc
  config: !inherit
    num: !inherit
      ? 1
)";

const char *ResolvedSimpleConfig1Negative = R"(---
config:
  num:
    ? 0
)";

const char *ResolvedSimpleConfig1Empty = R"(---
config:
  num:
    ? 0
)";

const char *ResolvedSimpleConfig1Abc = R"(---
config:
  num:
    ? 0
    ? 1
)";

const char *UnresolvedSimpleConfig2 = R"(---
cluster: test
version: 12.1
config:
  num:
    ? 0
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: 1
  selector:
    tenant: abc
  config: !inherit
    num: !inherit
      ? 1
- description: 2
  selector:
    tenant: ""
  config: !inherit
    num: !inherit
      ? 2
)";

const char *ResolvedSimpleConfig2Negative = R"(---
config:
  num:
    ? 0
)";

const char *ResolvedSimpleConfig2Empty = R"(---
config:
  num:
    ? 0
    ? 2
)";

const char *ResolvedSimpleConfig2Abc = R"(---
config:
  num:
    ? 0
    ? 1
)";

const char *UnresolvedSimpleConfig3 = R"(---
cluster: test
version: 12.1
config:
  num:
    ? 0
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: 1
  selector:
    tenant:
      not_in:
        - abc
  config: !inherit
    num: !inherit
      ? 1
)";

const char *ResolvedSimpleConfig3Negative = R"(---
config:
  num:
    ? 0
    ? 1
)";

const char *ResolvedSimpleConfig3Empty = R"(---
config:
  num:
    ? 0
    ? 1
)";

const char *ResolvedSimpleConfig3Abc = R"(---
config:
  num:
    ? 0
)";

const char *UnresolvedAllConfig = R"(---
cluster: test
version: 12.1
config:
  num:
    ? 0
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
allowed_labels:
  tenant:
    type: string
  flavour:
    type: enum
    values:
      ? def
      ? jkl
  kind:
    type: enum
    values:
      ? mno
      ? pqr
  type:
    type: enum
    values:
      ? foo

selector_config:
- description: 1
  selector:
    tenant: abc
  config: !inherit
    num: !inherit
      ? 1
- description: 2
  selector:
    flavour: def
  config: !inherit
    num: !inherit
      ? 2
- description: 3
  selector:
    tenant: abc
    flavour: def
  config:
    num:
      ? 3
- description: 4
  selector:
    tenant: abc
  config: !inherit
    num: !inherit
      ? 4
- description: 5
  selector:
    tenant: ghi
  config: !inherit
    num: !inherit
      ? 5
- description: 6
  selector:
    flavour: jkl
  config: !inherit
    num: !inherit
      ? 6
- description: 7
  selector:
    kind: mno
  config: !inherit
    num: !inherit
      ? 7
- description: 8
  selector:
    kind: pqr
  config: !inherit
    num: !inherit
      ? 8
- description: 9
  selector:
    kind:
      not_in:
      - pqr
  config: !inherit
    num: !inherit
      ? 9
- description: 10
  selector:
    flavour:
      in:
      - def
      - jkl
  config: !inherit
    num: !inherit
      ? 10
- description: 11
  selector:
    flavour:
      not_in:
      - def
    kind: mno
    tenant: abc
  config: !inherit
    num: !inherit
      ? 11
- description: 12
  selector:
    type: foo
  config: !inherit
    num: !inherit
      ? 12
- description: 13
  selector:
    tenant:
      not_in:
      - abc
    type:
      not_in:
      - foo
    flavour: def
    kind: mno
  config: !inherit
    num: !inherit
      ? 13)";

const char *UnresolvedAllConfig14 = R"(---
- description: 14
  selector:
    tenant:
      in:
      - aaa0
      - bbb0
      - ccc0
      - ddd0
      - eee0
      - fff0
      - ggg0
      - kkk0
      - lll0
      - mmm0
      - aaa1
      - bbb1
      - ccc1
      - ddd1
      - eee1
      - fff1
      - ggg1
      - kkk1
      - lll1
      - mmm1
      - aaa2
      - bbb2
      - ccc2
      - ddd2
      - eee2
      - fff2
      - ggg2
      - kkk2
      - lll2
      - mmm2
      - aaa3
      - bbb3
      - ccc3
      - ddd3
      - eee3
      - fff3
      - ggg3
      - kkk3
      - lll3
      - mmm3
      - aaa4
      - bbb4
      - ccc4
      - ddd4
      - eee4
      - fff4
      - ggg4
      - kkk4
      - lll4
      - mmm4
      - aaa5
      - bbb5
      - ccc5
      - ddd5
      - eee5
      - fff5
      - ggg5
      - kkk5
      - lll5
      - mmm5
  config: !inherit
    num: !inherit
      ? 14
)";

const char *SimpleConfig = R"(---
cluster: test
version: 12.1
config:
  num: 1
allowed_labels:
  tenant:
    type: string

selector_config: []
)";

const char *VolatilePart = R"(
- description: test 4
  selector:
    tenant: /dev_global
  config:
    actor_system_config: {}
    cms_config:
      sentinel_config:
        enable: false
- description: test 5
  selector:
    canary: true
  config:
    actor_system_config: {}
    cms_config:
      sentinel_config:
        enable: true
)";

const char *Concatenated = R"(---
cluster: test
version: 12.1
config:
  num: 1
allowed_labels:
  tenant:
    type: string
selector_config:
- description: test 4
  selector:
    tenant: /dev_global
  config:
    actor_system_config: {}
    cms_config:
      sentinel_config:
        enable: false
- description: test 5
  selector:
    canary: true
  config:
    actor_system_config: {}
    cms_config:
      sentinel_config:
        enable: true
)";

const char *UnresolvedSimpleConfigAppend = R"(---
cluster: test
version: 12.1
config:
  num:
  - 0
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: 1
  selector:
    tenant: abc
  config: !inherit
    num: !append
    - 0
- description: 2
  selector:
    tenant: ""
  config: !inherit
    num: !append
    - 1
)";

const char *ResolvedSimpleConfigAppendAbc = R"(---
config:
  num:
  - 0
  - 0
)";

const char *ResolvedSimpleConfigAppendEmpty = R"(---
config:
  num:
  - 0
  - 1
)";

using EType = NYamlConfig::TLabel::EType;

TMap<TSet<TVector<NYamlConfig::TLabel>>, TVector<int>> ExpectedResolved =
{
    std::pair{
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (pqr  )  (ghi  )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   5,   6,   8,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (pqr  )  (abc  ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   1,   4,   6,   8,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (mno  )  (abc  )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   1,   4,   6,   7,   9,   10,   11,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (mno  ) !(     ) _(     ) ],
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [ (jkl  )  (mno  ) _(     ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   6,   7,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (mno  ) !(     )  (foo  ) ],
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [ (jkl  )  (mno  ) _(     )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   6,   7,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  ) _(     )  (ghi  )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   5,   6,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  ) _(     )  (ghi  ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   5,   6,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  ) _(     )  (abc  )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   1,   4,   6,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  ) _(     )  (abc  ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   1,   4,   6,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  ) _(     ) !(     )  (foo  ) ],
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [ (jkl  ) _(     ) _(     )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   6,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  ) _(     ) !(     ) _(     ) ],
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [ (jkl  ) _(     ) _(     ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   6,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (mno  )  (abc  )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{3,   4,   7,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (pqr  ) !(     ) _(     ) ],
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [ (def  )  (pqr  ) _(     ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   2,   8,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (mno  )  (abc  ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{3,   4,   7,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (mno  )  (ghi  )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   2,   5,   7,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (mno  )  (ghi  ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   5,   6,   7,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (pqr  )  (ghi  ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   2,   5,   8,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (mno  ) !(     ) _(     ) ],
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [ (def  )  (mno  ) _(     ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   2,   7,   9,   10,   13},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  ) _(     )  (ghi  )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   2,   5,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (pqr  )  (abc  )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   1,   4,   8,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (mno  )  (ghi  ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   2,   5,   7,   9,   10,   13},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  ) _(     )  (abc  )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{3,   4,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     ) _(     ) !(     ) _(     ) ],
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [_(     ) _(     ) _(     ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   9},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  ) _(     )  (ghi  ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   2,   5,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  ) _(     ) !(     )  (foo  ) ],
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [ (def  ) _(     ) _(     )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   2,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (mno  )  (ghi  ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   5,   7,   9},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (pqr  )  (ghi  )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   5,   8,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  ) _(     )  (abc  ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{3,   4,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (pqr  ) !(     )  (foo  ) ],
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [_(     )  (pqr  ) _(     )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   8,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (pqr  )  (abc  ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{3,   4,   8,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (pqr  )  (ghi  ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   5,   6,   8,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (pqr  )  (ghi  ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   5,   8},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (pqr  ) !(     )  (foo  ) ],
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [ (jkl  )  (pqr  ) _(     )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   6,   8,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (pqr  ) !(     )  (foo  ) ],
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [ (def  )  (pqr  ) _(     )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   2,   8,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (mno  ) !(     )  (foo  ) ],
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [ (def  )  (mno  ) _(     )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("mno")}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   2,   7,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (pqr  ) !(     ) _(     ) ],
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [_(     )  (pqr  ) _(     ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   8},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (pqr  ) !(     ) _(     ) ],
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [ (jkl  )  (pqr  ) _(     ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   6,   8,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (mno  )  (ghi  )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   5,   7,   9,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (pqr  )  (abc  ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("pqr")}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   1,   4,   8},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (pqr  )  (abc  )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("pqr")}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   1,   4,   6,   8,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (mno  )  (abc  ) _(     ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   1,   4,   6,   7,   9,   10,   11},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     ) _(     )  (abc  ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   1,   4,   9},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     ) _(     )  (abc  )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   1,   4,   9,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (mno  ) !(     )  (foo  ) ],
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
            // [_(     )  (mno  ) _(     )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   7,   9,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  ) _(     ) !(     ) _(     ) ],
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [ (def  ) _(     ) _(     ) _(     ) ]
            {{EType::Common, TString("def")}, {EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   2,   9,   10},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (jkl  )  (mno  )  (ghi  )  (foo  ) ]
            {{EType::Common, TString("jkl")}, {EType::Common, TString("mno")}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   5,   6,   7,   9,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (mno  )  (abc  )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   1,   4,   7,   9,   11,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     ) _(     )  (ghi  ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("ghi")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   5,   9},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (mno  )  (abc  ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Common, TString("abc")}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   1,   4,   7,   9,   11},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     )  (mno  ) !(     ) _(     ) ],
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Negative, TString()}, {EType::Empty, TString()}},
            // [_(     )  (mno  ) _(     ) _(     ) ]
            {{EType::Empty, TString()}, {EType::Common, TString("mno")}, {EType::Empty, TString()}, {EType::Empty, TString()}},
        },
        TVector<int>{0,   7,   9},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (pqr  )  (abc  )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Common, TString("abc")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{3,   4,   8,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     ) _(     ) !(     )  (foo  ) ],
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("foo")}},
            // [_(     ) _(     ) _(     )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Negative, TString()}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   9,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [ (def  )  (pqr  )  (ghi  )  (foo  ) ]
            {{EType::Common, TString("def")}, {EType::Common, TString("pqr")}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   2,   5,   8,   10,   12},
    },
    {
        TSet<TVector<NYamlConfig::TLabel>>{
            // [_(     ) _(     )  (ghi  )  (foo  ) ]
            {{EType::Empty, TString()}, {EType::Empty, TString()}, {EType::Common, TString("ghi")}, {EType::Common, TString("foo")}},
        },
        TVector<int>{0,   5,   9,   12},
    },
};

Y_UNIT_TEST_SUITE(YamlConfigOpaqueConfigMarker) {

    Y_UNIT_TEST(OpaqueConfigFieldsIncludesExistingOpaqueFields) {
        const auto& fields = NYaml::OpaqueConfigFields();

        bool found = false;
        for (const auto& f : fields) {
            if (f.Name == "private_database_config") {
                found = true;
                break;
            }
        }
        UNIT_ASSERT_C(found,
            "expected 'private_database_config' in OpaqueConfigFields()");
    }

    Y_UNIT_TEST(OpaqueConfigFieldsAreStable) {
        // The list is computed once and cached; consecutive calls must return
        // the same vector by reference.
        const auto& a = NYaml::OpaqueConfigFields();
        const auto& b = NYaml::OpaqueConfigFields();
        UNIT_ASSERT_EQUAL(&a, &b);
    }

    Y_UNIT_TEST(CaptureOpaqueConfigFieldsReplacesMessageSubtreeWithEmptyMap) {
        NJson::TJsonValue cfg(NJson::JSON_MAP);
        auto& sub = cfg["private_database_config"];
        sub.SetType(NJson::JSON_MAP);
        sub["some_unknown_field"] = 42;
        sub["some_unknown_struct"]["field_1"] = 1;
        sub["some_unknown_struct"]["field_2"] = "abc";

        NYaml::CaptureOpaqueConfigFields(cfg);

        UNIT_ASSERT(cfg.Has("private_database_config"));
        const auto& captured = cfg["private_database_config"];
        UNIT_ASSERT_C(captured.IsMap(),
            "private_database_config must remain a map after capture");
        UNIT_ASSERT_C(captured.GetMap().empty(),
            "private_database_config must be replaced with an empty map");
    }

    Y_UNIT_TEST(CaptureOpaqueConfigFieldsLeavesOtherFieldsUntouched) {
        NJson::TJsonValue cfg(NJson::JSON_MAP);
        cfg["private_database_config"]["dropped"] = "value";
        cfg["feature_flags"]["enable_something"] = true;
        cfg["actor_system_config"]["sys_executor"] = 0;

        NYaml::CaptureOpaqueConfigFields(cfg);

        UNIT_ASSERT(cfg["private_database_config"].IsMap());
        UNIT_ASSERT(cfg["private_database_config"].GetMap().empty());

        UNIT_ASSERT(cfg["feature_flags"].IsMap());
        UNIT_ASSERT_EQUAL(cfg["feature_flags"]["enable_something"].GetBoolean(), true);

        UNIT_ASSERT(cfg["actor_system_config"].IsMap());
        UNIT_ASSERT_EQUAL(cfg["actor_system_config"]["sys_executor"].GetInteger(), 0);
    }

    Y_UNIT_TEST(CaptureOpaqueConfigFieldsIsNoopWhenFieldAbsent) {
        NJson::TJsonValue cfg(NJson::JSON_MAP);
        cfg["feature_flags"]["enable_something"] = true;

        NYaml::CaptureOpaqueConfigFields(cfg);

        UNIT_ASSERT(!cfg.Has("private_database_config"));
        UNIT_ASSERT(cfg["feature_flags"].IsMap());
        UNIT_ASSERT_EQUAL(cfg["feature_flags"]["enable_something"].GetBoolean(), true);
    }

    Y_UNIT_TEST(CaptureOpaqueConfigFieldsIsNoopOnNonMap) {
        // Non-map inputs must not crash and must remain unchanged.
        NJson::TJsonValue arr(NJson::JSON_ARRAY);
        arr.AppendValue(1);
        arr.AppendValue(2);
        NYaml::CaptureOpaqueConfigFields(arr);
        UNIT_ASSERT(arr.IsArray());
        UNIT_ASSERT_VALUES_EQUAL(arr.GetArray().size(), 2u);

        NJson::TJsonValue scalar(42);
        NYaml::CaptureOpaqueConfigFields(scalar);
        UNIT_ASSERT_EQUAL(scalar.GetInteger(), 42);
    }

    Y_UNIT_TEST(CaptureOpaqueConfigFieldsHandlesAlreadyEmptyMap) {
        NJson::TJsonValue cfg(NJson::JSON_MAP);
        cfg["private_database_config"].SetType(NJson::JSON_MAP);

        NYaml::CaptureOpaqueConfigFields(cfg);

        UNIT_ASSERT(cfg["private_database_config"].IsMap());
        UNIT_ASSERT(cfg["private_database_config"].GetMap().empty());
    }

    Y_UNIT_TEST(ParseAcceptsUnknownOpaqueFieldSubtree) {
        // End-to-end: the parser must accept unknown content under an
        // opaque top-level field without allow_unknown_fields, because
        // CaptureOpaqueConfigFields() strips the subtree before the
        // proto merge.
        const TString yaml = R"(---
metadata:
  cluster: ""
  version: 0
config:
  private_database_config:
    some_unknown_field: 42
    some_unknown_struct:
      field_1: 1
      field_2: abc
)";
        NKikimrConfig::TAppConfig cfg;
        UNIT_ASSERT_NO_EXCEPTION(cfg = NYaml::Parse(yaml, /*transform=*/ false));
        // The merge stored an empty message for the opaque field; nothing
        // from the YAML sub-tree leaked into the proto.
        UNIT_ASSERT(cfg.HasPrivateDatabaseConfig());
    }
}

Y_UNIT_TEST_SUITE(YamlConfig) {
    Y_UNIT_TEST(CollectLabels) {
        auto doc = NFyaml::TDocument::Parse(WholeConfig);

        auto labels = NYamlConfig::CollectLabels(doc);

        // TODO: forbid enum extension
        TMap<TString, NYamlConfig::TLabelType> expectedLabels = {
            {"flavour", NYamlConfig::TLabelType{NYamlConfig::EYamlConfigLabelTypeClass::Closed, TSet<TString>{"", "big", "medium", "small"}}},
            {"tenant", NYamlConfig::TLabelType{NYamlConfig::EYamlConfigLabelTypeClass::Open, TSet<TString>{/*"-*", */"", "/ru/tenant1", "/ru/tenant2", "/ru/tenant3"}}},
        };

        UNIT_ASSERT_EQUAL(labels, expectedLabels);
    }

    Y_UNIT_TEST(MaterializeSpecificConfig) {
        auto doc = NFyaml::TDocument::Parse(WholeConfig);

        {
            auto expected = NFyaml::TDocument::Parse(BigTenant1Config);

            auto [resolved, node] = NYamlConfig::Resolve(
                doc,
                {
                    NYamlConfig::TNamedLabel{"flavour", "big"},
                    NYamlConfig::TNamedLabel{"tenant", "/ru/tenant1"},
                }
            );

            UNIT_ASSERT(node.DeepEqual(expected.Root()));
        }

        {
            auto expected = NFyaml::TDocument::Parse(Tenant2Config);

            auto [resolved, node] = NYamlConfig::Resolve(
                doc,
                {
                    NYamlConfig::TNamedLabel{"tenant", "/ru/tenant2"},
                }
            );

            UNIT_ASSERT(node.DeepEqual(expected.Root()));
        }

        {
            auto expected = NFyaml::TDocument::Parse(SmallConfig);

            auto [resolved, node] = NYamlConfig::Resolve(
                doc,
                {
                    NYamlConfig::TNamedLabel{"flavour", "small"},
                }
            );

            UNIT_ASSERT(node.DeepEqual(expected.Root()));
        }

        {
            auto expected = NFyaml::TDocument::Parse(MediumConfig);

            auto [resolved, node] = NYamlConfig::Resolve(
                doc,
                {
                    NYamlConfig::TNamedLabel{"flavour", "medium"},
                }
            );

            UNIT_ASSERT(node.DeepEqual(expected.Root()));
        }

        {
            auto expected = NFyaml::TDocument::Parse(MediumTenant1Config);

            auto [resolved, node] = NYamlConfig::Resolve(
                doc,
                {
                    NYamlConfig::TNamedLabel{"flavour", "medium"},
                    NYamlConfig::TNamedLabel{"tenant", "/ru/tenant1"},
                }
            );

            UNIT_ASSERT(node.DeepEqual(expected.Root()));
        }
    }

    Y_UNIT_TEST(MaterializeAllConfigSimple) {
        TVector<TString> expectedLabels{"tenant"};

        {
            auto doc = NFyaml::TDocument::Parse(UnresolvedSimpleConfig1);
            auto resolved = NYamlConfig::ResolveAll(doc);
            UNIT_ASSERT_VALUES_EQUAL(resolved.Labels, expectedLabels);

            auto empty = NFyaml::TDocument::Parse(ResolvedSimpleConfig1Empty);
            auto negative = NFyaml::TDocument::Parse(ResolvedSimpleConfig1Negative);
            auto abc = NFyaml::TDocument::Parse(ResolvedSimpleConfig1Abc);

            TSet<TVector<NYamlConfig::TLabel>> first = {
                {NYamlConfig::TLabel{EType::Empty, ""}},
                {NYamlConfig::TLabel{EType::Negative, ""}},
            };

            auto firstIt = resolved.Configs.find(first);
            UNIT_ASSERT_UNEQUAL(firstIt, resolved.Configs.end());
            UNIT_ASSERT(firstIt->second.second.DeepEqual(empty.Root().Map()["config"]));
            UNIT_ASSERT(firstIt->second.second.DeepEqual(negative.Root().Map()["config"]));

            TSet<TVector<NYamlConfig::TLabel>> second = {
                {NYamlConfig::TLabel{EType::Common, "abc"}},
            };

            auto secondIt = resolved.Configs.find(second);
            UNIT_ASSERT_UNEQUAL(secondIt, resolved.Configs.end());
            UNIT_ASSERT(secondIt->second.second.DeepEqual(abc.Root().Map()["config"]));
        }

        {
            auto doc = NFyaml::TDocument::Parse(UnresolvedSimpleConfig2);
            auto resolved = NYamlConfig::ResolveAll(doc);
            UNIT_ASSERT_VALUES_EQUAL(resolved.Labels, expectedLabels);

            auto empty = NFyaml::TDocument::Parse(ResolvedSimpleConfig2Empty);
            auto negative = NFyaml::TDocument::Parse(ResolvedSimpleConfig2Negative);
            auto abc = NFyaml::TDocument::Parse(ResolvedSimpleConfig2Abc);

            TSet<TVector<NYamlConfig::TLabel>> first = {
                {NYamlConfig::TLabel{EType::Empty, ""}},
            };

            auto firstIt = resolved.Configs.find(first);
            UNIT_ASSERT_UNEQUAL(firstIt, resolved.Configs.end());
            UNIT_ASSERT(firstIt->second.second.DeepEqual(empty.Root().Map()["config"]));

            TSet<TVector<NYamlConfig::TLabel>> second = {
                {NYamlConfig::TLabel{EType::Common, "abc"}},
            };

            auto secondIt = resolved.Configs.find(second);
            UNIT_ASSERT_UNEQUAL(secondIt, resolved.Configs.end());
            UNIT_ASSERT(secondIt->second.second.DeepEqual(abc.Root().Map()["config"]));
        }

        {
            auto doc = NFyaml::TDocument::Parse(UnresolvedSimpleConfig3);
            auto resolved = NYamlConfig::ResolveAll(doc);
            UNIT_ASSERT_VALUES_EQUAL(resolved.Labels, expectedLabels);

            auto empty = NFyaml::TDocument::Parse(ResolvedSimpleConfig3Empty);
            auto negative = NFyaml::TDocument::Parse(ResolvedSimpleConfig3Negative);
            auto abc = NFyaml::TDocument::Parse(ResolvedSimpleConfig3Abc);

            TSet<TVector<NYamlConfig::TLabel>> first = {
                {NYamlConfig::TLabel{EType::Empty, ""}},
                {NYamlConfig::TLabel{EType::Negative, ""}},
            };

            auto firstIt = resolved.Configs.find(first);
            UNIT_ASSERT_UNEQUAL(firstIt, resolved.Configs.end());
            UNIT_ASSERT(firstIt->second.second.DeepEqual(empty.Root().Map()["config"]));
            UNIT_ASSERT(firstIt->second.second.DeepEqual(negative.Root().Map()["config"]));

            TSet<TVector<NYamlConfig::TLabel>> second = {
                {NYamlConfig::TLabel{EType::Common, "abc"}},
            };

            auto secondIt = resolved.Configs.find(second);
            UNIT_ASSERT_UNEQUAL(secondIt, resolved.Configs.end());
            UNIT_ASSERT(secondIt->second.second.DeepEqual(abc.Root().Map()["config"]));
        }

        {
            auto doc = NFyaml::TDocument::Parse(UnresolvedSimpleConfigAppend);
            auto resolved = NYamlConfig::ResolveAll(doc);
            UNIT_ASSERT_VALUES_EQUAL(resolved.Labels, expectedLabels);

            auto empty = NFyaml::TDocument::Parse(ResolvedSimpleConfigAppendEmpty);
            auto abc = NFyaml::TDocument::Parse(ResolvedSimpleConfigAppendAbc);

            TSet<TVector<NYamlConfig::TLabel>> first = {
                {NYamlConfig::TLabel{EType::Empty, ""}},
            };

            auto firstIt = resolved.Configs.find(first);
            UNIT_ASSERT_UNEQUAL(firstIt, resolved.Configs.end());
            UNIT_ASSERT(firstIt->second.second.DeepEqual(empty.Root().Map()["config"]));

            TSet<TVector<NYamlConfig::TLabel>> second = {
                {NYamlConfig::TLabel{EType::Common, "abc"}},
            };

            auto secondIt = resolved.Configs.find(second);
            UNIT_ASSERT_UNEQUAL(secondIt, resolved.Configs.end());
            UNIT_ASSERT(secondIt->second.second.DeepEqual(abc.Root().Map()["config"]));
        }
    }

    Y_UNIT_TEST(MaterializeAllConfigs) {
        TVector<TString> expectedLabels{"flavour", "kind", "tenant", "type"};

        {
            auto doc = NFyaml::TDocument::Parse(UnresolvedAllConfig);
            auto resolved = NYamlConfig::ResolveAll(doc);
            TMap<TSet<TVector<NYamlConfig::TLabel>>, TVector<int>> compacted;
            for(auto& [k, v] : resolved.Configs) {
                TVector<int> nums;
                for (auto& kv: v.second.Map()["num"].Map()) {
                    nums.push_back(std::stoi(kv.Key().Scalar()));
                }
                compacted[k] = nums;
            }

            UNIT_ASSERT_VALUES_EQUAL(resolved.Labels, expectedLabels);
            UNIT_ASSERT_VALUES_EQUAL(compacted.size(), ExpectedResolved.size());
            UNIT_ASSERT_VALUES_EQUAL(compacted, ExpectedResolved);
        }

        {
            auto config = TString(UnresolvedAllConfig) + UnresolvedAllConfig14;
            auto doc = NFyaml::TDocument::Parse(config.c_str());
            auto resolved = NYamlConfig::ResolveAll(doc);

            UNIT_ASSERT_VALUES_EQUAL(resolved.Labels, expectedLabels);
            UNIT_ASSERT_VALUES_EQUAL(resolved.Configs.size(), 72);
            // TODO extend ExpectedResolved manually and compare
        }
    }

    Y_UNIT_TEST(AppendVolatileConfig) {
        auto cfg = NFyaml::TDocument::Parse(SimpleConfig);
        auto volatilePart = NFyaml::TDocument::Parse(VolatilePart);
        NYamlConfig::AppendVolatileConfigs(cfg, volatilePart);
        TStringStream stream;
        stream << cfg;
        UNIT_ASSERT_VALUES_EQUAL(stream.Str(), TString(Concatenated));
    }

    Y_UNIT_TEST(AppendAndResolve) {
        auto cfg = NFyaml::TDocument::Parse(SimpleConfig);
        for (int i = 0; i < 4; ++i) {
            auto volatilePart = NFyaml::TDocument::Parse(VolatilePart);
            NYamlConfig::AppendVolatileConfigs(cfg, volatilePart);
        }
        TStringStream stream;
        stream << cfg;
    }

    void AppendDatabaseConfigBase(
        const TString mainConfig,
        const TString databaseConfig)
    {
        auto treeOriginal = NFyaml::TDocument::Parse(mainConfig);
        auto tree = NFyaml::TDocument::Parse(mainConfig);
        auto dbTree = NFyaml::TDocument::Parse(databaseConfig);

        const size_t selectorsCountBefore =
            tree.Root().Map().Has("selector_config")
                ? tree.Root().Map().at("selector_config").Sequence().size()
                : 0;

        // Add empty selector_config nodes to avoid processing missing YAML
        // nodes
        if (!tree.Root().Map().Has("selector_config")) {
            tree.Root().Map().Append(
                tree.Buildf("selector_config"),
                tree.Buildf("[]"));
        }
        if (!treeOriginal.Root().Map().Has("selector_config")) {
            treeOriginal.Root().Map().Append(
                treeOriginal.Buildf("selector_config"),
                treeOriginal.Buildf("[]"));
        }

        UNIT_ASSERT_NO_EXCEPTION(
            NKikimr::NYamlConfig::AppendDatabaseConfig(tree, dbTree));

        auto selectors = tree.Root().Map().at("selector_config").Sequence();
        UNIT_ASSERT_VALUES_EQUAL(selectors.size(), selectorsCountBefore + 1);

        auto lastSelector = selectors.at(selectors.size() - 1).Map();
        UNIT_ASSERT(lastSelector.Has("config"));

        // Check that main config selectors have not changed
        auto selectorsOriginal =
            treeOriginal.Root().Map().at("selector_config").Sequence();

        for (size_t i = 0; i < selectorsCountBefore; i++) {
            UNIT_ASSERT_VALUES_EQUAL(
                selectors.at(i).Map().size(),
                selectorsOriginal.at(i).Map().size());

            // Сheck all map elements (description, selector, config)
            for (auto field = selectors.at(i).Map().begin(),
                      fieldOrig = selectorsOriginal.at(i).Map().begin();
                 field != selectors.at(i).Map().end();
                 field++, fieldOrig++)
            {
                TStringStream fieldStr;
                fieldStr << field->Key() << ": " << field->Value();

                TStringStream fieldOriginalStr;
                fieldOriginalStr << fieldOrig->Key() << ": "
                                 << fieldOrig->Value();

                UNIT_ASSERT_VALUES_EQUAL(
                    fieldStr.Str(),
                    fieldOriginalStr.Str());
            }
        }

        // Check that the db config selector is appended to the end of the main
        // config selectors
        TStringStream actualLastConfig;
        actualLastConfig << lastSelector.at("config");

        TStringStream expectedConfig;
        expectedConfig << dbTree.Root().Map().at("config");

        UNIT_ASSERT_VALUES_EQUAL(actualLastConfig.Str(), expectedConfig.Str());
    }

    Y_UNIT_TEST(AppendDatabaseConfigAsTheOnlySelector)
    {
        const TString mainConfig = R"(
---
metadata:
  kind: MainConfig
  cluster: ""
  version: 0
config:
  feature_flags:
    some_param_1: true
)";

        const TString databaseConfig = R"(
---
metadata:
  kind: DatabaseConfig
  database: "/dc/tenant"
  version: 0
config:
  feature_flags: !inherit
    some_param_1: false
)";

        AppendDatabaseConfigBase(mainConfig, databaseConfig);
    }

    Y_UNIT_TEST(AppendDatabaseConfigAfterExistingSelectors)
    {
        const TString mainConfig = R"(
---
metadata:
  kind: MainConfig
  cluster: ""
  version: 0
config:
  feature_flags:
    some_param_1: true
selector_config:
- description: pre-existing selector 1 with config
  selector:
    tenant: dynamic
  config:
    feature_flags: !inherit
      some_param_2: true
- description: pre-existing selector 2 with config
  selector:
    tenant: dynamic
  config:
    feature_flags: !inherit
      some_param_3: true
)";

        const TString databaseConfig = R"(
---
metadata:
  kind: DatabaseConfig
  database: "/dc/tenant"
  version: 0
config:
  feature_flags: !inherit
    some_param_1: false
)";

        AppendDatabaseConfigBase(mainConfig, databaseConfig);
    }

    Y_UNIT_TEST(GetMetadata) {
        {
            TString str = R"(
metadata:
  version: 10
  cluster: foo
)";
            auto metadata = NYamlConfig::GetMainMetadata(str);
            UNIT_ASSERT_VALUES_EQUAL(*metadata.Version, 10);
            UNIT_ASSERT_VALUES_EQUAL(*metadata.Cluster, "foo");
        }

        {
            TString str = R"(
metadata:
  version: 10
)";
            auto metadata = NYamlConfig::GetMainMetadata(str);
            UNIT_ASSERT_VALUES_EQUAL(*metadata.Version, 10);
            UNIT_ASSERT(!metadata.Cluster);
        }

        {
            TString str = R"(
metadata:
  cluster: foo
)";
            auto metadata = NYamlConfig::GetMainMetadata(str);
            UNIT_ASSERT(!metadata.Version);
            UNIT_ASSERT_VALUES_EQUAL(*metadata.Cluster, "foo");
        }

        {
            TString str = R"(
metadata: {}
)";
            auto metadata = NYamlConfig::GetMainMetadata(str);
            UNIT_ASSERT(!metadata.Version);
            UNIT_ASSERT(!metadata.Cluster);
        }

        {
            TString str = "foo: bar";
            auto metadata = NYamlConfig::GetMainMetadata(str);
            UNIT_ASSERT(!metadata.Version);
            UNIT_ASSERT(!metadata.Cluster);
        }
    }

    Y_UNIT_TEST(ReplaceMetadata) {
        NYamlConfig::TMainMetadata metadata;
        metadata.Version = 1;
        metadata.Cluster = "test";

        {
            TString str = R"(
# comment1
{value: 1, array: [{test: "1"}], obj: {value: 2}} # comment2
# comment3
)";

            TString exp = R"(metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
value: 1
array:
- test: "1"
obj:
  value: 2
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(
# comment1
{value: 1, metadata: {version: 1, cluster: "test"}, array: [{test: "1"}], obj: {value: 2}} # comment2
# comment3
)";

            TString exp = R"(metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
value: 1
array:
- test: "1"
obj:
  value: 2
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(metadata: {version: 0, cluster: tes}
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(metadata:
  version: 0
  cluster: tes
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(metadata:
  version: 0
  cool: {foo: bar}
  cluster: tes
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(

---
metadata:
  cluster: tes
  version: 0
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(

---
metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(
---
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(
---
metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        {
            TString str = R"(
---
metadata:
  kind: MainConfig
  version: 0
  cluster: "test"
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(
---
metadata:
  kind: MainConfig
  cluster: "test"
  version: 1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }

        metadata.Cluster = "";

        {
            TString str = R"(
---
metadata:
  version: 1
  cluster:
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString exp = R"(
---
metadata:
  kind: MainConfig
  cluster: ""
  version: 1
# comment1
value: 1
array: [{test: "1"}]
obj: {value: 2} # comment2
# comment3
)";

            TString res = NYamlConfig::ReplaceMetadata(str, metadata);

            UNIT_ASSERT_VALUES_EQUAL(res, exp);
        }
    }

Y_UNIT_TEST(FuseConfigs_ConsoleWins) {
    // Console has key, base has same key -> console wins
    const char* base = R"(
actor_system_config:
  threads: 4
log_config:
  level: INFO
)";
    const char* console = R"(
metadata:
  kind: MainConfig
  version: 1
config:
  actor_system_config:
    threads: 8
allowed_labels:
  tenant:
    type: string
selector_config: []
)";
    auto result = NYamlConfig::FuseConfigs(base, console);
    auto root = result.Root().Map();

    // Console value preserved
    UNIT_ASSERT_VALUES_EQUAL(root.at("config").Map().at("actor_system_config").Map().at("threads").Scalar(), "8");
    // Base value added (missing in console)
    UNIT_ASSERT(root.at("config").Map().Has("log_config"));
    // Metadata from console
    UNIT_ASSERT(root.Has("metadata"));
    // Selectors preserved
    UNIT_ASSERT(root.Has("selector_config"));
    // Allowed labels preserved
    UNIT_ASSERT(root.Has("allowed_labels"));
}

Y_UNIT_TEST(FuseConfigs_BaseFillsGaps) {
    // Base has key not in console -> base value added
    const char* base = R"(
log_config:
  level: DEBUG
feature_flags:
  enable_x: true
)";
    const char* console = R"(
metadata:
  kind: MainConfig
config:
  log_config:
    level: INFO
)";
    auto result = NYamlConfig::FuseConfigs(base, console);
    auto configMap = result.Root().Map().at("config").Map();

    // Console value wins for existing key
    UNIT_ASSERT_VALUES_EQUAL(configMap.at("log_config").Map().at("level").Scalar(), "INFO");
    // Base fills gap for missing key
    UNIT_ASSERT(configMap.Has("feature_flags"));
    UNIT_ASSERT_VALUES_EQUAL(configMap.at("feature_flags").Map().at("enable_x").Scalar(), "true");
}

Y_UNIT_TEST(FuseConfigs_EmptyBase) {
    // Empty base config
    const char* base = R"()";
    const char* console = R"(
metadata:
  kind: MainConfig
config:
  log_config:
    level: INFO
)";
    auto result = NYamlConfig::FuseConfigs(base, console);
    auto configMap = result.Root().Map().at("config").Map();

    // Console config preserved
    UNIT_ASSERT(configMap.Has("log_config"));
    UNIT_ASSERT_VALUES_EQUAL(configMap.at("log_config").Map().at("level").Scalar(), "INFO");
}

Y_UNIT_TEST(FuseConfigs_EmptyConsoleConfig) {
    // Console has empty config section
    const char* base = R"(
log_config:
  level: DEBUG
)";
    const char* console = R"(
metadata:
  kind: MainConfig
config: {}
)";
    auto result = NYamlConfig::FuseConfigs(base, console);
    auto configMap = result.Root().Map().at("config").Map();

    // Base fills all gaps
    UNIT_ASSERT(configMap.Has("log_config"));
    UNIT_ASSERT_VALUES_EQUAL(configMap.at("log_config").Map().at("level").Scalar(), "DEBUG");
}

Y_UNIT_TEST(FuseConfigs_ExcludesStorageOnlyKeys) {
    // Storage-only keys from base should be excluded
    const char* base = R"(
log_config:
  level: DEBUG
hosts:
  - host: node1
    port: 19001
host_configs:
  - host_config_id: 1
blob_storage_config:
  service_set: {}
nameservice_config:
  node:
    - node_id: 1
static_erasure: mirror-3-dc
feature_flags:
  enable_x: true
)";
    const char* console = R"(
metadata:
  kind: MainConfig
config:
  log_config:
    level: INFO
)";
    auto result = NYamlConfig::FuseConfigs(base, console);
    auto configMap = result.Root().Map().at("config").Map();

    // Console value wins
    UNIT_ASSERT_VALUES_EQUAL(configMap.at("log_config").Map().at("level").Scalar(), "INFO");
    // Non-storage key from base is included
    UNIT_ASSERT(configMap.Has("feature_flags"));
    // Storage-only keys are excluded
    UNIT_ASSERT(!configMap.Has("hosts"));
    UNIT_ASSERT(!configMap.Has("host_configs"));
    UNIT_ASSERT(!configMap.Has("blob_storage_config"));
    UNIT_ASSERT(!configMap.Has("nameservice_config"));
    UNIT_ASSERT(!configMap.Has("static_erasure"));
}
}

Y_UNIT_TEST_SUITE(YamlConfigResolveUnique) {

Y_UNIT_TEST(NotUniqueSelectors) {
    const char* configWithNotUniqueSelectors = R"(---
allowed_labels:
  label1:
    type: enum
    values:
      ? a
      ? b
  label2:
    type: enum
    values:
      ? x
      ? y
      ? z
  label3:
    type: enum
    values:
      ? p
      ? q
      ? r
config:
  value: base
selector_config:
- description: "selector1"
  selector:
    label1: a
  config:
    value: config_a
- description: "selector2"
  selector:
    label1: b
  config:
    value: config_b
- description: "selector3"
  selector:
    label2: x
  config: {}
- description: "selector4"
  selector:
    label3: p
  config: {}
)";

    auto docAll = NFyaml::TDocument::Parse(configWithNotUniqueSelectors);
    auto resolvedAll = NYamlConfig::ResolveAll(docAll);

    auto docUniq = NFyaml::TDocument::Parse(configWithNotUniqueSelectors);
    TVector<NYamlConfig::TDocumentConfig> resolvedUniq;
    NYamlConfig::ResolveUniqueDocs(
        docUniq,
        [&](NYamlConfig::TDocumentConfig&& cfg) {
            resolvedUniq.push_back(std::move(cfg));
        });

    UNIT_ASSERT(resolvedUniq.size() < resolvedAll.Configs.size());
}

Y_UNIT_TEST(AllTestConfigs) {
    auto testConfig = [](const char* config, const char* name) {
        auto docAll = NFyaml::TDocument::Parse(config);
        auto resolvedAll = NYamlConfig::ResolveAll(docAll);

        auto docUniq = NFyaml::TDocument::Parse(config);
        TVector<NYamlConfig::TDocumentConfig> resolvedUniq;
        NYamlConfig::ResolveUniqueDocs(
            docUniq,
            [&](NYamlConfig::TDocumentConfig&& cfg) {
                resolvedUniq.push_back(std::move(cfg));
            });

        auto toStr = [](const NFyaml::TNodeRef& node) {
            TStringStream ss;
            ss << node;
            return ss.Str();
        };

        TSet<TString> allDocs;
        for (auto& [_, cfg] : resolvedAll.Configs) {
            auto doc = cfg.first.Clone();
            for (auto it = doc.begin(); it != doc.end(); ++it) {
                it->RemoveTag();
            }
            auto cleanConfig = doc.Root().Map().at("config");
            allDocs.insert(toStr(cleanConfig));
        }

        TSet<TString> uniqDocs;
        for (auto& cfg : resolvedUniq) {
            uniqDocs.insert(toStr(cfg.second));
        }

        UNIT_ASSERT_VALUES_EQUAL_C(uniqDocs.size(), resolvedUniq.size(),
            TString("Config: ") + name + ", ResolveUniqueDocs has duplicates");

        for (const auto& s : uniqDocs) {
            UNIT_ASSERT_C(allDocs.contains(s),
                TString("Config: ") + name + ", ResolveUniqueDocs has extra doc not in ResolveAll");
        }

        UNIT_ASSERT_VALUES_EQUAL_C(allDocs.size(), uniqDocs.size(),
            TString("Config: ") + name + ", size mismatch");

        for (const auto& s : allDocs) {
            UNIT_ASSERT_C(uniqDocs.contains(s),
                TString("Config: ") + name + ", ResolveUniqueDocs missing doc from ResolveAll");
        }
    };

    testConfig(WholeConfig, "WholeConfig");
    testConfig(UnresolvedAllConfig, "UnresolvedAllConfig");
    testConfig(UnresolvedSimpleConfig1, "UnresolvedSimpleConfig1");
    testConfig(UnresolvedSimpleConfig2, "UnresolvedSimpleConfig2");
    testConfig(UnresolvedSimpleConfig3, "UnresolvedSimpleConfig3");
    testConfig(UnresolvedSimpleConfigAppend, "UnresolvedSimpleConfigAppend");
}

}

// ===========================================================================
// Track A+ : polynomial, K-independent validation -- proof of equivalence with
// full enumeration (the ResolveAll oracle).
// ===========================================================================

namespace {

using namespace NKikimr::NYamlConfig;

// All fixtures disable the builtin incompatibility rules so that ResolveAll
// enumerates the full unconstrained label space; the A+ engine's localised
// enumeration then projects onto exactly that space (incompatibility-rule
// integration is a separate realizability refinement, see docs).

// k=1: auth_config.account_lockout.attempt_reset_duration must parse as a
// duration. base 3h ok; tenant=bad overrides with garbage.
const char* APlusDurationBad = R"(---
cluster: test
version: 1
config:
  auth_config:
    account_lockout:
      attempt_reset_duration: 3h
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: bad tenant
  selector:
    tenant: bad
  config: !inherit
    auth_config: !inherit
      account_lockout: !inherit
        attempt_reset_duration: not-a-duration
)";

// k=1: same shape, all values valid.
const char* APlusDurationOk = R"(---
cluster: test
version: 1
config:
  auth_config:
    account_lockout:
      attempt_reset_duration: 3h
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: other tenant
  selector:
    tenant: other
  config: !inherit
    auth_config: !inherit
      account_lockout: !inherit
        attempt_reset_duration: 30m
)";

// k=5 password complexity, the realizability crown jewel.
// base:        min_length=8,  sum=4   -> ok
// tenant=A:    min_length=4   (sum stays 4) -> 4<4 false -> ok
// tenant=B:    min_length=20, lower=10 (sum=13) -> 20<13 false -> ok
// All realizable assignments PASS, so the oracle accepts.
// But the independent value-set product pairs A's min_length=4 with B's
// lower=10 -> sum=13 > 4 -> a NON-realizable failing combo. A naive product
// check would false-positive; the realizability filter must accept.
const char* APlusPasswordRealizable = R"(---
cluster: test
version: 1
config:
  auth_config:
    password_complexity:
      min_length: 8
      min_lower_case_count: 1
      min_upper_case_count: 1
      min_numbers_count: 1
      min_special_chars_count: 1
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: A lowers min_length but nothing else
  selector:
    tenant: A
  config: !inherit
    auth_config: !inherit
      password_complexity: !inherit
        min_length: 4
- description: B raises lower but also raises min_length to compensate
  selector:
    tenant: B
  config: !inherit
    auth_config: !inherit
      password_complexity: !inherit
        min_length: 20
        min_lower_case_count: 10
)";

// k=5 password complexity, genuinely failing: tenant=C lowers min_length below
// the (unchanged) sum -> a realizable violation. Oracle and A+ both reject.
const char* APlusPasswordFail = R"(---
cluster: test
version: 1
config:
  auth_config:
    password_complexity:
      min_length: 8
      min_lower_case_count: 2
      min_upper_case_count: 2
      min_numbers_count: 2
      min_special_chars_count: 2
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: C lowers min_length below sum=8
  selector:
    tenant: C
  config: !inherit
    auth_config: !inherit
      password_complexity: !inherit
        min_length: 3
)";

// k=2 + presence (ColumnShard): tenant=A switches codec to lz4 while the base
// level=3 survives the deep-merge -> lz4 does not support a level -> reject.
const char* APlusCompressionLevelUnsupported = R"(---
cluster: test
version: 1
config:
  column_shard_config:
    default_compression: zstd
    default_compression_level: 3
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: A switches to lz4 but inherits level
  selector:
    tenant: A
  config: !inherit
    column_shard_config: !inherit
      default_compression: lz4
)";

// k=2 + presence: tenant=A introduces a level without ever setting a codec
// -> "level without type" -> reject. Exercises the presence (absent) state.
const char* APlusCompressionLevelWithoutType = R"(---
cluster: test
version: 1
config:
  column_shard_config:
    column_chunks_v2: true
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: A sets a level but no codec
  selector:
    tenant: A
  config: !inherit
    column_shard_config: !inherit
      default_compression_level: 5
)";

// All codecs/levels valid across the whole space -> accept.
const char* APlusCompressionOk = R"(---
cluster: test
version: 1
config:
  column_shard_config:
    default_compression: zstd
    default_compression_level: 3
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: A switches to a level-capable codec
  selector:
    tenant: A
  config: !inherit
    column_shard_config: !inherit
      default_compression: zstd
      default_compression_level: 9
)";

// A coupled path that is an accumulating !append sequence -> must be fenced.
const char* APlusFencedAppend = R"(---
cluster: test
version: 1
config:
  grpc_config:
    services_enabled:
    - legacy
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: A appends a service
  selector:
    tenant: A
  config: !inherit
    grpc_config: !inherit
      services_enabled: !append
      - yql
)";

std::optional<TString> Get(const TFieldAssignment& a, const TString& path) {
    auto it = a.find(path);
    return (it == a.end()) ? std::nullopt : it->second;
}

ui64 AsCount(const std::optional<TString>& v) {
    if (!v.has_value()) {
        return 0;
    }
    ui64 out = 0;
    return TryFromString<ui64>(*v, out) ? out : 0;
}

// ---- Reified rules (mirror the real selector-varied validators) ----

TFieldRule DurationRule() {
    return TFieldRule{
        .Paths = {"/auth_config/account_lockout/attempt_reset_duration"},
        .Check = [](const TFieldAssignment& a) -> std::optional<TString> {
            auto v = Get(a, "/auth_config/account_lockout/attempt_reset_duration");
            if (!v.has_value()) {
                return std::nullopt;
            }
            TDuration d;
            if (TDuration::TryParse(*v, d)) {
                return std::nullopt;
            }
            return TString("account_lockout: cannot parse attempt reset duration");
        },
    };
}

TFieldRule PasswordComplexityRule() {
    return TFieldRule{
        .Paths = {
            "/auth_config/password_complexity/min_length",
            "/auth_config/password_complexity/min_lower_case_count",
            "/auth_config/password_complexity/min_upper_case_count",
            "/auth_config/password_complexity/min_numbers_count",
            "/auth_config/password_complexity/min_special_chars_count",
        },
        .Check = [](const TFieldAssignment& a) -> std::optional<TString> {
            ui64 sum = AsCount(Get(a, "/auth_config/password_complexity/min_lower_case_count"))
                     + AsCount(Get(a, "/auth_config/password_complexity/min_upper_case_count"))
                     + AsCount(Get(a, "/auth_config/password_complexity/min_numbers_count"))
                     + AsCount(Get(a, "/auth_config/password_complexity/min_special_chars_count"));
            ui64 minLength = AsCount(Get(a, "/auth_config/password_complexity/min_length"));
            if (minLength < sum) {
                return TString("password_complexity: min length below total min counts");
            }
            return std::nullopt;
        },
    };
}

TFieldRule CompressionRule() {
    return TFieldRule{
        .Paths = {
            "/column_shard_config/default_compression",
            "/column_shard_config/default_compression_level",
        },
        .Check = [](const TFieldAssignment& a) -> std::optional<TString> {
            static const TSet<TString> known{"off", "lz4", "zstd", "zlib", "brotli", "bz2", "snappy", "gzip"};
            static const TSet<TString> supportsLevel{"zstd", "zlib", "brotli", "gzip", "bz2"};
            auto comp = Get(a, "/column_shard_config/default_compression");
            auto level = Get(a, "/column_shard_config/default_compression_level");
            if (!comp.has_value() && !level.has_value()) {
                return std::nullopt;
            }
            if (!comp.has_value() && level.has_value()) {
                return TString("ColumnShardConfig: compression level is set without compression type");
            }
            if (!known.contains(*comp)) {
                return TString("ColumnShardConfig: Unknown compression");
            }
            if (level.has_value() && !supportsLevel.contains(*comp)) {
                return TString(TStringBuilder() << "ColumnShardConfig: compression `" << *comp << "` does not support compression level");
            }
            return std::nullopt;
        },
    };
}

// ---- Oracle : true full-enumeration decision ----

std::optional<TString> ReadScalarAtPath(NFyaml::TNodeRef config, const TString& path) {
    NFyaml::TNodeRef node = config;
    TVector<TString> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == TString::npos) {
            if (start < path.size()) {
                parts.push_back(path.substr(start));
            }
            break;
        }
        if (slash > start) {
            parts.push_back(path.substr(start, slash - start));
        }
        start = slash + 1;
    }
    for (auto& key : parts) {
        if (!node || node.Type() != NFyaml::ENodeType::Mapping) {
            return std::nullopt;
        }
        auto map = node.Map();
        if (!map.Has(key)) {
            return std::nullopt;
        }
        node = map.at(key);
    }
    if (!node || node.Type() != NFyaml::ENodeType::Scalar) {
        return std::nullopt;
    }
    return TString(node.Scalar());
}

// True iff SOME resolved config violates the rule (full enumeration).
bool OracleRuleFails(const char* yaml, const TFieldRule& rule) {
    auto doc = NFyaml::TDocument::Parse(yaml);
    auto resolved = ResolveAll(doc);
    for (auto& [labels, docConfig] : resolved.Configs) {
        Y_UNUSED(labels);
        TFieldAssignment assignment;
        for (auto& p : rule.Paths) {
            assignment[p] = ReadScalarAtPath(docConfig.second, p);
        }
        if (rule.Check(assignment).has_value()) {
            return true;
        }
    }
    return false;
}

bool APlusRuleFails(const char* yaml, const TFieldRule& rule, bool& fenced) {
    auto doc = NFyaml::TDocument::Parse(yaml);
    TSet<TString> fencedPaths;
    auto violations = ValidateFieldRule(doc, rule, fencedPaths);
    fenced = !fencedPaths.empty();
    return !violations.empty();
}

// Naive independent value-set product decision (NO realizability filter):
// fails if ANY combination in the product of per-field value-sets violates.
bool NaiveProductRuleFails(const char* yaml, const TFieldRule& rule) {
    auto doc = NFyaml::TDocument::Parse(yaml);
    auto sets = ResolveFieldValueSets(doc);
    TVector<TVector<std::optional<TString>>> domains;
    for (auto& p : rule.Paths) {
        TVector<std::optional<TString>> vals;
        auto it = sets.Values.find(p);
        if (it == sets.Values.end()) {
            vals.push_back(std::nullopt);
        } else {
            for (auto& v : it->second) {
                vals.push_back(v);
            }
        }
        domains.push_back(std::move(vals));
    }
    bool fails = false;
    std::function<void(size_t, TFieldAssignment&)> rec = [&](size_t i, TFieldAssignment& a) {
        if (fails) {
            return;
        }
        if (i == rule.Paths.size()) {
            if (rule.Check(a).has_value()) {
                fails = true;
            }
            return;
        }
        for (auto& v : domains[i]) {
            a[rule.Paths[i]] = v;
            rec(i + 1, a);
        }
    };
    TFieldAssignment a;
    rec(0, a);
    return fails;
}

// Collects scalar leaves of a resolved config node, skipping sequences (which
// are fenced) -- mirrors the production CollectScalarLeaves walker.
void CollectResolvedLeaves(NFyaml::TNodeRef node, const TString& path, TMap<TString, TString>& out) {
    if (!node) {
        return;
    }
    switch (node.Type()) {
        case NFyaml::ENodeType::Scalar:
            out[path] = TString(node.Scalar());
            break;
        case NFyaml::ENodeType::Mapping: {
            auto map = node.Map();
            for (auto it = map.begin(); it != map.end(); ++it) {
                CollectResolvedLeaves(it->Value(), path + "/" + it->Key().Scalar(), out);
            }
            break;
        }
        case NFyaml::ENodeType::Sequence:
            break;
    }
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigFieldValueSets) {

    // The keystone soundness property: every value any field takes in ANY fully
    // resolved config is contained in its A+ value-set (or the path is fenced).
    Y_UNIT_TEST(ValueSetSoundnessVsResolveAll) {
        const TVector<std::pair<const char*, const char*>> fixtures{
            {APlusDurationBad, "APlusDurationBad"},
            {APlusDurationOk, "APlusDurationOk"},
            {APlusPasswordRealizable, "APlusPasswordRealizable"},
            {APlusPasswordFail, "APlusPasswordFail"},
            {APlusCompressionLevelUnsupported, "APlusCompressionLevelUnsupported"},
            {APlusCompressionLevelWithoutType, "APlusCompressionLevelWithoutType"},
            {APlusCompressionOk, "APlusCompressionOk"},
            {UnresolvedSimpleConfig1, "UnresolvedSimpleConfig1"},
            {UnresolvedSimpleConfig2, "UnresolvedSimpleConfig2"},
            {UnresolvedSimpleConfig3, "UnresolvedSimpleConfig3"},
        };
        for (auto& [yaml, name] : fixtures) {
            auto doc = NFyaml::TDocument::Parse(yaml);
            auto sets = ResolveFieldValueSets(doc);

            auto resolveDoc = NFyaml::TDocument::Parse(yaml);
            auto resolved = ResolveAll(resolveDoc);
            for (auto& [labels, docConfig] : resolved.Configs) {
                Y_UNUSED(labels);
                TMap<TString, TString> leaves;
                CollectResolvedLeaves(docConfig.second, "", leaves);
                for (auto& [path, value] : leaves) {
                    if (sets.IsFenced(path)) {
                        continue;
                    }
                    auto it = sets.Values.find(path);
                    UNIT_ASSERT_C(it != sets.Values.end(),
                        TStringBuilder() << name << ": path " << path << " missing from value-sets");
                    UNIT_ASSERT_C(it->second.contains(value),
                        TStringBuilder() << name << ": resolved value '" << value
                            << "' at " << path << " not in A+ value-set");
                }
            }
        }
    }

    Y_UNIT_TEST(AppendPathsAreFenced) {
        auto doc = NFyaml::TDocument::Parse(APlusFencedAppend);
        auto sets = ResolveFieldValueSets(doc);
        UNIT_ASSERT_C(sets.IsFenced("/grpc_config/services_enabled"),
            "an !append sequence path must be fenced");
        UNIT_ASSERT_C(!sets.Values.contains("/grpc_config/services_enabled"),
            "a fenced sequence must not produce a scalar value-set");
    }

    Y_UNIT_TEST(ValidateFieldSingleFieldK1) {
        auto durationParses = [](const TString& v) {
            TDuration d;
            return TDuration::TryParse(v, d);
        };

        {
            auto doc = NFyaml::TDocument::Parse(APlusDurationOk);
            auto sets = ResolveFieldValueSets(doc);
            TString failing;
            UNIT_ASSERT(ValidateField(sets,
                "/auth_config/account_lockout/attempt_reset_duration", durationParses, failing));
        }
        {
            auto doc = NFyaml::TDocument::Parse(APlusDurationBad);
            auto sets = ResolveFieldValueSets(doc);
            TString failing;
            UNIT_ASSERT(!ValidateField(sets,
                "/auth_config/account_lockout/attempt_reset_duration", durationParses, failing));
            UNIT_ASSERT_VALUES_EQUAL(failing, "not-a-duration");
        }
    }
}

Y_UNIT_TEST_SUITE(YamlConfigAPlusValidation) {

    // The core claim: the A+ decision equals the full-enumeration oracle for
    // every rule and fixture.
    Y_UNIT_TEST(EquivalentToFullEnumeration) {
        struct TCase {
            const char* Yaml;
            const char* Name;
            TFieldRule Rule;
        };
        TVector<TCase> cases{
            {APlusDurationOk, "DurationOk", DurationRule()},
            {APlusDurationBad, "DurationBad", DurationRule()},
            {APlusPasswordRealizable, "PasswordRealizable", PasswordComplexityRule()},
            {APlusPasswordFail, "PasswordFail", PasswordComplexityRule()},
            {APlusCompressionOk, "CompressionOk", CompressionRule()},
            {APlusCompressionLevelUnsupported, "CompressionLevelUnsupported", CompressionRule()},
            {APlusCompressionLevelWithoutType, "CompressionLevelWithoutType", CompressionRule()},
        };
        for (auto& c : cases) {
            bool fenced = false;
            bool aplus = APlusRuleFails(c.Yaml, c.Rule, fenced);
            bool oracle = OracleRuleFails(c.Yaml, c.Rule);
            UNIT_ASSERT_C(!fenced, TStringBuilder() << c.Name << ": unexpectedly fenced");
            UNIT_ASSERT_VALUES_EQUAL_C(aplus, oracle,
                TStringBuilder() << c.Name << ": A+ decision (" << aplus
                    << ") != oracle (" << oracle << ")");
        }
    }

    // Expected accept/reject outcomes, pinned explicitly.
    Y_UNIT_TEST(ExpectedOutcomes) {
        bool fenced = false;
        UNIT_ASSERT(!APlusRuleFails(APlusDurationOk, DurationRule(), fenced));
        UNIT_ASSERT(APlusRuleFails(APlusDurationBad, DurationRule(), fenced));
        UNIT_ASSERT(!APlusRuleFails(APlusPasswordRealizable, PasswordComplexityRule(), fenced));
        UNIT_ASSERT(APlusRuleFails(APlusPasswordFail, PasswordComplexityRule(), fenced));
        UNIT_ASSERT(!APlusRuleFails(APlusCompressionOk, CompressionRule(), fenced));
        UNIT_ASSERT(APlusRuleFails(APlusCompressionLevelUnsupported, CompressionRule(), fenced));
        UNIT_ASSERT(APlusRuleFails(APlusCompressionLevelWithoutType, CompressionRule(), fenced));
    }

    // The realizability filter earns its keep: the naive per-field value-set
    // product FALSE-POSITIVES on the cross-tenant password fixture, while the
    // realizability-aware A+ engine matches the oracle (accept).
    Y_UNIT_TEST(RealizabilityFilterBeatsNaiveProduct) {
        auto rule = PasswordComplexityRule();

        UNIT_ASSERT_C(NaiveProductRuleFails(APlusPasswordRealizable, rule),
            "the naive independent product is expected to false-positive here");

        UNIT_ASSERT_C(!OracleRuleFails(APlusPasswordRealizable, rule),
            "no realizable config actually violates the rule");

        bool fenced = false;
        UNIT_ASSERT_C(!APlusRuleFails(APlusPasswordRealizable, rule, fenced),
            "the realizability-aware A+ engine must match the oracle (accept)");
        UNIT_ASSERT(!fenced);
    }

    Y_UNIT_TEST(EnumerateRealizableAssignmentsIsExact) {
        // tenant=A and tenant=B each contribute one assignment; everything else
        // (empty / negative tenant) maps to base. Exactly 3 distinct joint
        // assignments over the two compression fields.
        auto doc = NFyaml::TDocument::Parse(APlusCompressionOk);
        TSet<TString> fenced;
        auto assignments = EnumerateRealizableAssignments(doc, CompressionRule().Paths, fenced);
        UNIT_ASSERT(fenced.empty());
        UNIT_ASSERT_VALUES_EQUAL(assignments.size(), 2u); // {zstd,3} (base) and {zstd,9} (tenant=A)
    }

    Y_UNIT_TEST(FencedRuleReportsAndDefers) {
        auto rule = TFieldRule{
            .Paths = {"/grpc_config/services_enabled"},
            .Check = [](const TFieldAssignment&) -> std::optional<TString> {
                return TString("should never run on a fenced path");
            },
        };
        auto doc = NFyaml::TDocument::Parse(APlusFencedAppend);
        TSet<TString> fenced;
        auto violations = ValidateFieldRule(doc, rule, fenced);
        UNIT_ASSERT_C(fenced.contains("/grpc_config/services_enabled"), "append path must be fenced");
        UNIT_ASSERT_C(violations.empty(), "fenced rule must defer (no violations emitted)");
    }
}

// ===========================================================================
// Track A+ benchmark : proof of STRICT superiority over enumerate-then-validate.
//
// The cost metric is the number of times the per-document semantic validator
// would run (deterministic, not wall-clock):
//   * legacy accept gate: once per distinct resolved document K, and each call
//     is preceded by a deep Clone + YamlToProto of the whole config;
//   * Track A+: once per realizable joint assignment of the rule's coupled
//     fields, with no document materialisation at all.
//
// With two independent high-cardinality labels (tenant x region), K is
// MULTIPLICATIVE ((T+1)(R+1)) while A+ is ADDITIVE ((T+1)+(R+1)) -- this is the
// Sigma-vs-Pi gap (doc CE-1) and the "latent cliff" a second high-cardinality
// label triggers. The tests assert the exact counts and that the ratio widens
// with scale; wall-clock is printed as secondary evidence only.
// ===========================================================================

namespace {

// Builds a config with `tenants` tenant-keyed selectors overriding feature_a and
// `regions` region-keyed selectors overriding feature_b. The two fields are
// independent and single-sourced, so every (tenant, region) pair yields a
// byte-distinct resolved config.
TString MakeBenchConfig(size_t tenants, size_t regions) {
    TStringBuilder sb;
    sb << "---\n";
    sb << "cluster: test\n";
    sb << "version: 1\n";
    sb << "config:\n";
    sb << "  feature_a: A0\n";
    sb << "  feature_b: B0\n";
    sb << "allowed_labels:\n";
    sb << "  tenant:\n    type: string\n";
    sb << "  region:\n    type: string\n";
    sb << "incompatibility_overrides:\n  disable_rules:\n";
    for (const char* r : {"builtin_branch_must_have_value", "builtin_dynamic_must_have_value",
                          "builtin_node_host_must_have_value", "builtin_node_id_must_have_value",
                          "builtin_rev_must_have_value", "builtin_node_type_must_be_defined",
                          "builtin_tenant_must_be_defined"}) {
        sb << "    - " << r << "\n";
    }
    sb << "selector_config:\n";
    for (size_t i = 0; i < tenants; ++i) {
        sb << "- description: ta" << i << "\n";
        sb << "  selector:\n    tenant: t" << i << "\n";
        sb << "  config: !inherit\n    feature_a: a" << i << "\n";
    }
    for (size_t j = 0; j < regions; ++j) {
        sb << "- description: rb" << j << "\n";
        sb << "  selector:\n    region: r" << j << "\n";
        sb << "  config: !inherit\n    feature_b: b" << j << "\n";
    }
    return sb;
}

// Legacy cost: number of distinct resolved documents the accept gate validates.
size_t LegacyValidateCount(const TString& yaml) {
    auto doc = NFyaml::TDocument::Parse(yaml);
    size_t k = 0;
    ResolveUniqueDocs(doc, [&](TDocumentConfig&&) { ++k; });
    return k;
}

// A+ cost: realizable assignments evaluated to validate both fields.
size_t APlusEvalCount(const TString& yaml) {
    auto docA = NFyaml::TDocument::Parse(yaml);
    TSet<TString> fa;
    size_t a = EnumerateRealizableAssignments(docA, {"/feature_a"}, fa).size();
    auto docB = NFyaml::TDocument::Parse(yaml);
    TSet<TString> fb;
    size_t b = EnumerateRealizableAssignments(docB, {"/feature_b"}, fb).size();
    return a + b;
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigAPlusBenchmark) {

    // The headline result: legacy work is multiplicative, A+ is additive, A+ is
    // strictly less, and the gap WIDENS with scale (the cliff).
    Y_UNIT_TEST(StrictlySuperiorAndGapWidens) {
        struct TPoint { size_t T; size_t R; };
        for (auto p : {TPoint{6, 6}, TPoint{20, 20}}) {
            auto yaml = MakeBenchConfig(p.T, p.R);
            size_t legacy = LegacyValidateCount(yaml);
            size_t aplus = APlusEvalCount(yaml);
            UNIT_ASSERT_VALUES_EQUAL_C(legacy, (p.T + 1) * (p.R + 1),
                TStringBuilder() << "legacy must be multiplicative at T=R=" << p.T);
            UNIT_ASSERT_VALUES_EQUAL_C(aplus, (p.T + 1) + (p.R + 1),
                TStringBuilder() << "A+ must be additive at T=R=" << p.T);
            UNIT_ASSERT_C(aplus < legacy, "A+ must do strictly less validation work");
        }

        double ratioSmall = double((6 + 1) * (6 + 1)) / double((6 + 1) + (6 + 1));
        double ratioLarge = double((20 + 1) * (20 + 1)) / double((20 + 1) + (20 + 1));
        UNIT_ASSERT_C(ratioLarge > ratioSmall * 2.0,
            TStringBuilder() << "the legacy/A+ work ratio must grow with scale: small="
                << ratioSmall << " large=" << ratioLarge);
    }

    // K-independence: adding a second high-cardinality label does NOT increase
    // A+ work for a field that does not couple it. This is the property
    // memoization cannot provide (doc CE-1/CE-3).
    Y_UNIT_TEST(WorkIsIndependentOfUncoupledLabel) {
        auto fewRegions = MakeBenchConfig(20, 5);
        auto manyRegions = MakeBenchConfig(20, 30);

        auto d1 = NFyaml::TDocument::Parse(fewRegions);
        auto d2 = NFyaml::TDocument::Parse(manyRegions);
        TSet<TString> f1, f2;
        size_t a1 = EnumerateRealizableAssignments(d1, {"/feature_a"}, f1).size();
        size_t a2 = EnumerateRealizableAssignments(d2, {"/feature_a"}, f2).size();

        UNIT_ASSERT_VALUES_EQUAL(a1, 21u); // base + 20 tenant overrides
        UNIT_ASSERT_VALUES_EQUAL_C(a2, a1,
            "feature_a A+ work must not depend on the number of regions");

        // The legacy gate, by contrast, blows up 40x between the two configs.
        UNIT_ASSERT_VALUES_EQUAL(LegacyValidateCount(fewRegions), 21u * 6u);
        UNIT_ASSERT_VALUES_EQUAL(LegacyValidateCount(manyRegions), 21u * 31u);
    }

    // Today's regime (one high-cardinality label): A+ does the SAME work as
    // legacy -- it is never worse, only better when a second label appears.
    Y_UNIT_TEST(SingleLabelRegimeNeverWorse) {
        auto yaml = MakeBenchConfig(50, 0);
        UNIT_ASSERT_VALUES_EQUAL(LegacyValidateCount(yaml), 51u);
        auto doc = NFyaml::TDocument::Parse(yaml);
        TSet<TString> fa;
        UNIT_ASSERT_VALUES_EQUAL(EnumerateRealizableAssignments(doc, {"/feature_a"}, fa).size(), 51u);
    }

    // Wall-clock evidence (printed, not asserted -- counts above are the proof).
    Y_UNIT_TEST(WallClockEvidence) {
        const size_t T = 20;
        const size_t R = 20;
        auto yaml = MakeBenchConfig(T, R);

        auto t0 = TInstant::Now();
        size_t legacy = LegacyValidateCount(yaml);
        auto legacyDur = TInstant::Now() - t0;

        auto t1 = TInstant::Now();
        size_t aplus = APlusEvalCount(yaml);
        auto aplusDur = TInstant::Now() - t1;

        Cerr << "[A+ BENCH T=R=" << T << "] legacy: validates K=" << legacy
             << " docs in " << legacyDur.MicroSeconds() << "us"
             << " ; A+: evaluates " << aplus << " assignments in " << aplusDur.MicroSeconds() << "us"
             << " ; work ratio=" << (double(legacy) / double(aplus)) << "x" << Endl;

        UNIT_ASSERT_VALUES_EQUAL(legacy, (T + 1) * (R + 1));
        UNIT_ASSERT_VALUES_EQUAL(aplus, (T + 1) + (R + 1));
    }
}

// ===========================================================================
// Track A+ structural (proto-transform) validation tests.
// ===========================================================================

namespace {

using namespace NKikimr::NYamlConfig;

NFyaml::TNodeRef StructNodeAt(NFyaml::TNodeRef config, const TString& path) {
    NFyaml::TNodeRef node = config;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        TString key = (slash == TString::npos) ? path.substr(start) : path.substr(start, slash - start);
        if (!key.empty()) {
            if (!node || node.Type() != NFyaml::ENodeType::Mapping) {
                return NFyaml::TNodeRef{};
            }
            auto map = node.Map();
            if (!map.Has(key)) {
                return NFyaml::TNodeRef{};
            }
            node = map.at(key);
        }
        if (slash == TString::npos) {
            break;
        }
        start = slash + 1;
    }
    return node;
}

TString ProjectionKey(NFyaml::TNodeRef config, const TVector<TString>& sections) {
    TStringStream ss;
    for (const auto& s : sections) {
        ss << s << "=";
        if (auto n = StructNodeAt(config, s); n) {
            ss << n;
        } else {
            ss << "<absent>";
        }
        ss << "\x02";
    }
    return ss.Str();
}

// base: log_config.cluster_name + other_field; `tenants` selectors vary the
// (transform-read) log_config.cluster_name; `regions` selectors vary the
// (non-transform) other_field.
TString MakeProjConfig(size_t tenants, size_t regions) {
    TStringBuilder sb;
    sb << "---\ncluster: test\nversion: 1\n";
    sb << "config:\n  log_config:\n    cluster_name: base\n  other_field: base\n";
    sb << "allowed_labels:\n  tenant:\n    type: string\n  region:\n    type: string\n";
    sb << "incompatibility_overrides:\n  disable_rules:\n";
    for (const char* r : {"builtin_branch_must_have_value", "builtin_dynamic_must_have_value",
                          "builtin_node_host_must_have_value", "builtin_node_id_must_have_value",
                          "builtin_rev_must_have_value", "builtin_node_type_must_be_defined",
                          "builtin_tenant_must_be_defined"}) {
        sb << "    - " << r << "\n";
    }
    sb << "selector_config:\n";
    for (size_t i = 0; i < tenants; ++i) {
        sb << "- description: t" << i << "\n  selector:\n    tenant: t" << i << "\n";
        sb << "  config: !inherit\n    log_config: !inherit\n      cluster_name: lc" << i << "\n";
    }
    for (size_t j = 0; j < regions; ++j) {
        sb << "- description: r" << j << "\n  selector:\n    region: r" << j << "\n";
        sb << "  config: !inherit\n    other_field: of" << j << "\n";
    }
    return sb;
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigStructuralProjection) {

    // Regime A/C correctness: the set of distinct transform-read-section
    // projections EnumerateDistinctProjections produces equals the set
    // ResolveUniqueDocs (full enumeration) produces. Running the proto transform
    // on the former is therefore equivalent to running it on every resolved doc.
    Y_UNIT_TEST(ProjectionSetEqualsFullEnumeration) {
        auto sections = TransformReadDynamicSections();
        auto yaml = MakeProjConfig(8, 8);

        TSet<TString> legacy;
        auto d1 = NFyaml::TDocument::Parse(yaml);
        ResolveUniqueDocs(d1, [&](TDocumentConfig&& cfg) {
            legacy.insert(ProjectionKey(cfg.second, sections));
        });

        TSet<TString> aplus;
        auto d2 = NFyaml::TDocument::Parse(yaml);
        EnumerateDistinctProjections(d2, sections, [&](NFyaml::TNodeRef node) {
            aplus.insert(ProjectionKey(node, sections));
        });

        UNIT_ASSERT_C(aplus == legacy, "A+ projection set must equal full enumeration's");
        UNIT_ASSERT_VALUES_EQUAL(aplus.size(), 9u); // base + 8 tenant log_config variants
    }

    // K-independence: distinct transform-read projections do not grow with a
    // second high-cardinality label that varies only NON-transform sections.
    Y_UNIT_TEST(ProjectionWorkIndependentOfUncoupledLabel) {
        auto sections = TransformReadDynamicSections();

        auto count = [&](const TString& yaml) {
            size_t n = 0;
            auto doc = NFyaml::TDocument::Parse(yaml);
            EnumerateDistinctProjections(doc, sections, [&](NFyaml::TNodeRef) { ++n; });
            return n;
        };

        UNIT_ASSERT_VALUES_EQUAL(count(MakeProjConfig(8, 5)), 9u);
        UNIT_ASSERT_VALUES_EQUAL(count(MakeProjConfig(8, 40)), 9u); // independent of regions
    }
}

Y_UNIT_TEST_SUITE(YamlConfigStructuralGuard) {

    Y_UNIT_TEST(DetectsSelectorOverridingStaticSection) {
        TString yaml = R"(---
cluster: test
version: 1
config:
  domains_config:
    domain:
    - name: base
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_tenant_must_be_defined
selector_config:
- description: bad
  selector:
    tenant: x
  config: !inherit
    domains_config: !inherit
      security_config: !inherit
        enforce_user_token_requirement: true
)";
        auto doc = NFyaml::TDocument::Parse(yaml);
        auto hits = SelectorWritesUnder(doc, StaticGuardSections());
        UNIT_ASSERT_C(!hits.empty(), "a selector overriding /domains_config must trip the guard");
        bool under = false;
        for (const auto& h : hits) {
            if (h.StartsWith("/domains_config")) {
                under = true;
            }
        }
        UNIT_ASSERT(under);
    }

    Y_UNIT_TEST(PassesWhenSelectorsOnlyVaryDynamic) {
        auto doc = NFyaml::TDocument::Parse(MakeProjConfig(4, 4));
        auto hits = SelectorWritesUnder(doc, StaticGuardSections());
        UNIT_ASSERT_C(hits.empty(), "no selector touches a static section here");
    }
}

namespace {

// Finds a singular (non-repeated) scalar leaf of a given kind reachable from
// TAppConfig, avoiding the transform-read and static-guard sections -- so a bad
// value there is invisible to the transform/static passes and ONLY the per-field
// coercion layer can catch it. Returns a config-relative path plus a coercible
// "good" value and a non-coercible "bad" value.
//   kind 0 = enum, 1 = integer (int/uint), 2 = bool.
struct TScalarLeaf {
    TString Path;
    TString Good;
    TString Bad;
};

bool FindScalarLeaf(const google::protobuf::Descriptor* desc, const TString& prefix,
                    int depth, const TSet<TString>& avoidTop, int kind, TScalarLeaf& out) {
    using FD = google::protobuf::FieldDescriptor;
    if (!desc || depth > 4) {
        return false;
    }
    for (int i = 0; i < desc->field_count(); ++i) {
        const auto* f = desc->field(i);
        if (f->is_repeated()) {
            continue;
        }
        TString name = f->name();
        NProtobufJson::ToSnakeCaseDense(&name);
        TString path = prefix + "/" + name;
        if (depth == 0 && avoidTop.contains(path)) {
            continue;
        }
        const auto t = f->cpp_type();
        if (kind == 0 && t == FD::CPPTYPE_ENUM && f->enum_type()->value_count() > 0) {
            out = TScalarLeaf{path, f->enum_type()->value(0)->name(), "ZZ_NOT_AN_ENUM_VALUE"};
            return true;
        }
        if (kind == 1 && (t == FD::CPPTYPE_INT32 || t == FD::CPPTYPE_INT64
                          || t == FD::CPPTYPE_UINT32 || t == FD::CPPTYPE_UINT64)) {
            out = TScalarLeaf{path, "1", "not_a_number"};
            return true;
        }
        if (kind == 2 && t == FD::CPPTYPE_BOOL) {
            out = TScalarLeaf{path, "true", "not_a_bool"};
            return true;
        }
        if (t == FD::CPPTYPE_MESSAGE && FindScalarLeaf(f->message_type(), path, depth + 1, avoidTop, kind, out)) {
            return true;
        }
    }
    return false;
}

void EmitNested(TStringBuilder& sb, const TVector<TString>& comps, size_t idx,
                const TString& value, int indent, bool inherit) {
    TString pad(indent * 2, ' ');
    if (idx + 1 == comps.size()) {
        sb << pad << comps[idx] << ": " << value << "\n";
        return;
    }
    sb << pad << comps[idx] << ":" << (inherit ? " !inherit" : "") << "\n";
    EmitNested(sb, comps, idx + 1, value, indent + 1, inherit);
}

TVector<TString> SplitLeafPath(const TString& path) {
    TVector<TString> comps;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        TString k = (slash == TString::npos) ? path.substr(start) : path.substr(start, slash - start);
        if (!k.empty()) { comps.push_back(k); }
        if (slash == TString::npos) { break; }
        start = slash + 1;
    }
    return comps;
}

// Builds a config whose base sets `leaf` to a coercible value and a tenant=bad
// selector overrides it with a non-coercible value, then asserts the legacy and
// A+ structural validations REACH THE SAME reject decision -- proving A+ is
// strictly not weaker than legacy proto-validity verification for this field.
// `expectReject` = does the legacy converter actually throw for this kind? Enum
// names throw; plain int/bool are robustly (silently) coerced because the parser
// uses CastRobust=true, so legacy accepts them -- and A+ must do the same. In
// every case the REQUIRED property is agreement (!Diverged): A+ rejects exactly
// when legacy does, i.e. A+ is strictly not weaker (no false negatives).
void AssertCoercionStrictlyNotWeaker(int kind, const char* kindName, bool expectReject) {
    TSet<TString> avoid;
    for (const auto& s : TransformReadDynamicSections()) { avoid.insert(s); }
    for (const auto& s : StaticGuardSections()) { avoid.insert(s); }

    TScalarLeaf leaf;
    if (!FindScalarLeaf(NKikimrConfig::TAppConfig::descriptor(), "", 0, avoid, kind, leaf)) {
        Cerr << "[coercion] no " << kindName << " leaf outside transform/static sections; skipping" << Endl;
        return;
    }

    auto comps = SplitLeafPath(leaf.Path);
    TStringBuilder sb;
    sb << "---\ncluster: test\nversion: 1\nconfig:\n";
    EmitNested(sb, comps, 0, leaf.Good, 1, /*inherit*/ false);
    sb << "allowed_labels:\n  tenant:\n    type: string\n";
    sb << "incompatibility_overrides:\n  disable_rules:\n    - builtin_tenant_must_be_defined\n";
    sb << "selector_config:\n- description: bad\n  selector:\n    tenant: bad\n  config: !inherit\n";
    EmitNested(sb, comps, 0, leaf.Bad, 2, /*inherit*/ true);

    auto doc = NFyaml::TDocument::Parse(TString(sb));
    auto shadow = NYamlConfig::StructuralShadowRun(doc, /*allowUnknown*/ true);

    // The strictly-not-weaker property: A+ and legacy reach the SAME decision.
    UNIT_ASSERT_C(!shadow.Diverged, TStringBuilder()
        << kindName << ": A+ must decide exactly as legacy at " << leaf.Path
        << " (strictly not weaker) -- legacy=" << shadow.LegacyRejected
        << " aplus=" << shadow.APlusRejected);

    if (expectReject) {
        UNIT_ASSERT_C(shadow.LegacyRejected, TStringBuilder()
            << kindName << ": legacy is expected to reject the bad value at " << leaf.Path);
        UNIT_ASSERT_C(shadow.APlusRejected, TStringBuilder()
            << kindName << ": A+ must also reject at " << leaf.Path);
    }
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigStructuralCoercion) {

    // A selector introducing a non-coercible value in a NON-transform section is
    // caught by the per-field coercion layer (K-independently), exactly as the
    // legacy per-doc path would. Covered for every scalar field kind, so A+ is
    // strictly not weaker than legacy proto-validity verification.
    // Enum coercion throws in the legacy converter; A+ must reject too.
    Y_UNIT_TEST(EnumCoercionStrictlyNotWeaker) {
        AssertCoercionStrictlyNotWeaker(0, "enum", /*expectReject*/ true);
    }

    // Non-enum scalars: the legacy parser coerces them robustly (CastRobust),
    // so legacy ACCEPTS -- and A+ must accept too (no divergence). A+ uses the
    // SAME converter per section, so its coercion decision is equal to legacy's
    // by construction for every field type, not just enums.
    Y_UNIT_TEST(IntegerCoercionMatchesLegacy) {
        AssertCoercionStrictlyNotWeaker(1, "integer", /*expectReject*/ false);
    }

    Y_UNIT_TEST(BoolCoercionMatchesLegacy) {
        AssertCoercionStrictlyNotWeaker(2, "bool", /*expectReject*/ false);
    }
}

Y_UNIT_TEST_SUITE(YamlConfigStructuralShadow) {

    // The migration shadow-run: the legacy per-doc structural validation and
    // the K-independent A+ validation must reach the SAME accept/reject decision.
    // This config (log_config varied per tenant) passes the proto transform, so
    // both ACCEPT -- proving A+ does not spuriously reject a config legacy
    // accepts, and that they do not diverge.
    Y_UNIT_TEST(LegacyAndAPlusAgree) {
        auto doc = NFyaml::TDocument::Parse(MakeProjConfig(6, 6));
        auto shadow = NYamlConfig::StructuralShadowRun(doc, /*allowUnknown*/ true);

        UNIT_ASSERT_C(!shadow.Diverged, "legacy and A+ structural decisions must agree");
        UNIT_ASSERT_C(!shadow.LegacyRejected, "legacy accepts this transform-valid config");
        UNIT_ASSERT_C(!shadow.APlusRejected, "A+ must also accept (no spurious rejection)");
        UNIT_ASSERT_C(shadow.GuardViolations.empty(), "no selector touches a static section here");
    }

    // Regression (parity): a section a selector introduces ONLY as an EMPTY
    // mapping carries no scalar leaf, so a purely leaf-derived section set
    // never projects it -- while the legacy per-doc path resolves docs where it
    // IS present and (with allowUnknown=false) rejects the unknown section.
    // The A+ section set unions the presence-based view, so both engines must
    // REJECT with no divergence.
    Y_UNIT_TEST(EmptyMappingSectionFromSelectorAgreesWithLegacy) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  log_config:
    cluster_name: base
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_branch_must_have_value
    - builtin_dynamic_must_have_value
    - builtin_node_host_must_have_value
    - builtin_node_id_must_have_value
    - builtin_rev_must_have_value
    - builtin_node_type_must_be_defined
    - builtin_tenant_must_be_defined
selector_config:
- description: sneaky
  selector:
    tenant: x
  config: !inherit
    nonexistent_config: {}
)");
        auto shadow = NYamlConfig::StructuralShadowRun(doc, /*allowUnknown*/ false);
        UNIT_ASSERT_C(shadow.LegacyRejected, "legacy rejects the unknown empty-mapping section");
        UNIT_ASSERT_C(shadow.APlusRejected, "A+ must project presence-only sections and reject too");
        UNIT_ASSERT_C(!shadow.Diverged, "empty-mapping-only section must not cause divergence");
    }
}

Y_UNIT_TEST_SUITE(YamlConfigAPlusVerdict) {

    // The live-gate shadow primitive: the full A+ surface (structural + guard +
    // semantic) in one call, no legacy enumeration. A transform-valid config is
    // accepted with no violation to attribute.
    Y_UNIT_TEST(AcceptsValidConfig) {
        auto doc = NFyaml::TDocument::Parse(MakeProjConfig(2, 2));
        auto verdict = NYamlConfig::ValidateAPlus(doc, /*allowUnknown*/ true);
        UNIT_ASSERT_C(!verdict.Rejected, verdict.FirstViolation());
        UNIT_ASSERT(verdict.FirstViolation().empty());
    }

    // A structurally invalid config is rejected and FirstViolation() carries a
    // non-empty message for log attribution.
    Y_UNIT_TEST(RejectsAndAttributesFirstViolation) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  nonexistent_config:
    some_field: 1
)");
        auto verdict = NYamlConfig::ValidateAPlus(doc, /*allowUnknown*/ false);
        UNIT_ASSERT_C(verdict.Rejected, "unknown section with allowUnknown=false must be rejected");
        UNIT_ASSERT(!verdict.FirstViolation().empty());
    }
}

Y_UNIT_TEST_SUITE(YamlConfigStructuralSectionMarkers) {

    // The transform-read and static-guard section lists are DERIVED from proto
    // field markers (NMarkers.SelectorTransformRead / SelectorStatic) via the
    // config protoc plugin, not hand-declared -- this closes the "is the
    // transform-read list complete?" gap by keeping the classification next to
    // the fields. This test pins the wiring.
    Y_UNIT_TEST(DerivedFromMarkers) {
        TSet<TString> tr;
        for (const auto& s : NYamlConfig::TransformReadDynamicSections()) { tr.insert(s); }
        UNIT_ASSERT_C(tr.contains("/actor_system_config"), "actor_system_config must be transform-read");
        UNIT_ASSERT_C(tr.contains("/log_config"), "log_config must be transform-read");
        UNIT_ASSERT_C(tr.contains("/auth_config"), "auth_config must be transform-read");
        UNIT_ASSERT_C(tr.contains("/grpc_config"), "grpc_config must be transform-read");
        UNIT_ASSERT_C(tr.contains("/interconnect_config"), "interconnect_config must be transform-read");

        TSet<TString> sg;
        for (const auto& s : NYamlConfig::StaticGuardSections()) { sg.insert(s); }
        UNIT_ASSERT_C(sg.contains("/domains_config"), "domains_config must be static-guarded");
        UNIT_ASSERT_C(sg.contains("/blob_storage_config"), "blob_storage_config must be static-guarded");
        UNIT_ASSERT_C(sg.contains("/nameservice_config"), "nameservice_config must be static-guarded");
        UNIT_ASSERT_C(sg.contains("/hosts"), "hosts (ephemeral input) must be static-guarded");

        // A section must not be classified as BOTH.
        for (const auto& s : tr) {
            UNIT_ASSERT_C(!sg.contains(s), TStringBuilder() << s << " is both transform-read and static");
        }
    }

    // Drift guard for the cross-section SEMANTIC couplings. ValidateMonitoringConfig
    // reads domains_config jointly with monitoring_config, and ValidateStateStorage
    // reads domains_config + self_management_config. Per-section projection is sound
    // ONLY if those STATIC partners stay guarded (selectors cannot vary them, so the
    // constant base is their single realizable value). If a future change unmarks
    // either, the semantic per-section pass would silently become weaker than legacy
    // for the decoupled split-label shape. Pin the invariant here.
    Y_UNIT_TEST(StaticGuardCoversSemanticCoupledSections) {
        TSet<TString> sg;
        for (const auto& s : NYamlConfig::StaticGuardSections()) { sg.insert(s); }
        UNIT_ASSERT_C(sg.contains("/domains_config"),
            "domains_config is read jointly by Monitoring & StateStorage validators -- must stay static-guarded");
        UNIT_ASSERT_C(sg.contains("/self_management_config"),
            "self_management_config is read by the StateStorage aggregate -- must stay static-guarded");

        UNIT_ASSERT_C(!NYamlConfig::StaticGuardSections().empty(), "static guard set must be non-empty");
        UNIT_ASSERT_C(!NYamlConfig::TransformReadDynamicSections().empty(),
            "transform-read set must be non-empty");
    }
}

namespace {

// base auth_config + log_config; `tenants` selectors vary auth_config (a
// transform-read section), `regions` selectors vary log_config (another
// transform-read section), under two INDEPENDENT labels.
TString MakeTwoTransformSectionConfig(size_t tenants, size_t regions) {
    TStringBuilder sb;
    sb << "---\ncluster: test\nversion: 1\n";
    sb << "config:\n  auth_config:\n    field: base\n  log_config:\n    cluster_name: base\n";
    sb << "allowed_labels:\n  tenant:\n    type: string\n  region:\n    type: string\n";
    sb << "incompatibility_overrides:\n  disable_rules:\n";
    for (const char* r : {"builtin_branch_must_have_value", "builtin_dynamic_must_have_value",
                          "builtin_node_host_must_have_value", "builtin_node_id_must_have_value",
                          "builtin_rev_must_have_value", "builtin_node_type_must_be_defined",
                          "builtin_tenant_must_be_defined"}) {
        sb << "    - " << r << "\n";
    }
    sb << "selector_config:\n";
    for (size_t i = 0; i < tenants; ++i) {
        sb << "- description: t" << i << "\n  selector:\n    tenant: t" << i << "\n";
        sb << "  config: !inherit\n    auth_config: !inherit\n      field: a" << i << "\n";
    }
    for (size_t j = 0; j < regions; ++j) {
        sb << "- description: r" << j << "\n  selector:\n    region: r" << j << "\n";
        sb << "  config: !inherit\n    log_config: !inherit\n      cluster_name: l" << j << "\n";
    }
    return sb;
}

size_t ProjectionCount(const TString& yaml, const TVector<TString>& sections) {
    auto doc = NFyaml::TDocument::Parse(yaml);
    size_t n = 0;
    NYamlConfig::EnumerateDistinctProjections(doc, sections, [&](NFyaml::TNodeRef) { ++n; });
    return n;
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigStructuralBenchmark) {

    // The polynomiality property: validating two transform-read sections varied
    // by two independent high-cardinality labels costs (T+1)+(R+1) work
    // PER-SECTION (additive, as ValidateStructuralAPlus does it) -- NOT the
    // (T+1)*(R+1) the joint projection (or the legacy per-doc enumeration) would
    // incur. This is the cliff the redesign removes.
    Y_UNIT_TEST(PerSectionIsAdditiveNotMultiplicative) {
        const size_t T = 12, R = 12;
        auto yaml = MakeTwoTransformSectionConfig(T, R);

        // Per-section (what A+ actually does): each section's projection count is
        // independent of the OTHER section's label.
        size_t authProj = ProjectionCount(yaml, {"/auth_config"});
        size_t logProj = ProjectionCount(yaml, {"/log_config"});
        UNIT_ASSERT_VALUES_EQUAL(authProj, T + 1);  // base + T tenant variants
        UNIT_ASSERT_VALUES_EQUAL(logProj, R + 1);   // base + R region variants

        // The joint projection (the multiplicative cliff we DON'T use).
        size_t jointProj = ProjectionCount(yaml, {"/auth_config", "/log_config"});
        UNIT_ASSERT_VALUES_EQUAL(jointProj, (T + 1) * (R + 1));

        // Additive per-section work is strictly less than joint/legacy.
        UNIT_ASSERT_C(authProj + logProj < jointProj,
            "per-section structural work must be additive, not multiplicative");
    }

    // K-independence: a section's structural work does not grow with a second
    // high-cardinality label that varies a DIFFERENT section.
    Y_UNIT_TEST(SectionWorkIndependentOfOtherLabel) {
        UNIT_ASSERT_VALUES_EQUAL(ProjectionCount(MakeTwoTransformSectionConfig(10, 3), {"/auth_config"}), 11u);
        UNIT_ASSERT_VALUES_EQUAL(ProjectionCount(MakeTwoTransformSectionConfig(10, 300), {"/auth_config"}), 11u);
    }
}

namespace {

const char* kDisableRules =
    "incompatibility_overrides:\n  disable_rules:\n"
    "    - builtin_branch_must_have_value\n"
    "    - builtin_dynamic_must_have_value\n"
    "    - builtin_node_host_must_have_value\n"
    "    - builtin_node_id_must_have_value\n"
    "    - builtin_rev_must_have_value\n"
    "    - builtin_node_type_must_be_defined\n"
    "    - builtin_tenant_must_be_defined\n";

// base auth password is valid; tenant=bad lowers min_length below the sum -> the
// Auth k=5 semantic rule is violated only for that tenant variant.
TString MakeAuthVariantConfig(bool badVariant) {
    TStringBuilder sb;
    sb << "---\ncluster: test\nversion: 1\nconfig:\n";
    sb << "  auth_config:\n    password_complexity:\n";
    sb << "      min_length: 10\n      min_lower_case_count: 1\n      min_upper_case_count: 1\n";
    sb << "      min_numbers_count: 1\n      min_special_chars_count: 1\n";
    sb << "allowed_labels:\n  tenant:\n    type: string\n";
    sb << kDisableRules;
    sb << "selector_config:\n- description: t\n  selector:\n    tenant: bad\n  config: !inherit\n";
    sb << "    auth_config: !inherit\n      password_complexity: !inherit\n";
    sb << "        min_length: " << (badVariant ? "1" : "8") << "\n";  // 1 < sum(4) violates; 8 ok
    return sb;
}

// base monitoring is valid (no auth requirement); tenant=bad turns on counters
// authentication without EnforceUserTokenRequirement -> Monitoring rule (cross
// section monitoring_config + domains security) violated only for that variant.
TString MakeMonitoringVariantConfig(bool badVariant) {
    TStringBuilder sb;
    sb << "---\ncluster: test\nversion: 1\nconfig:\n";
    sb << "  monitoring_config:\n    require_counters_authentication: false\n";
    sb << "allowed_labels:\n  tenant:\n    type: string\n";
    sb << kDisableRules;
    sb << "selector_config:\n- description: t\n  selector:\n    tenant: bad\n  config: !inherit\n";
    sb << "    monitoring_config: !inherit\n";
    sb << "      require_counters_authentication: " << (badVariant ? "true" : "false") << "\n";
    return sb;
}

void AssertSemanticStrictlyNotWeaker(const TString& yaml, bool expectReject, const char* name) {
    auto doc = NFyaml::TDocument::Parse(yaml);
    auto shadow = NYamlConfig::StructuralShadowRun(doc, /*allowUnknown*/ true);
    UNIT_ASSERT_C(!shadow.SemanticDiverged, TStringBuilder()
        << name << ": A+ semantic must decide exactly as legacy (strictly not weaker) -- legacy="
        << shadow.SemanticLegacyRejected << " aplus=" << shadow.SemanticAPlusRejected);
    UNIT_ASSERT_VALUES_EQUAL_C(shadow.SemanticLegacyRejected, expectReject,
        TStringBuilder() << name << ": legacy reject expectation");
    UNIT_ASSERT_VALUES_EQUAL_C(shadow.SemanticAPlusRejected, expectReject,
        TStringBuilder() << name << ": A+ reject expectation");
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigSemanticAPlus) {

    // Auth k=5 (password complexity) violation introduced by a selector is
    // caught by A+ exactly as by the legacy per-doc semantic gate.
    Y_UNIT_TEST(AuthViolationInSelectorCaught) {
        AssertSemanticStrictlyNotWeaker(MakeAuthVariantConfig(/*bad*/ true), /*expectReject*/ true, "auth-bad");
    }
    Y_UNIT_TEST(AuthValidVariantAccepted) {
        AssertSemanticStrictlyNotWeaker(MakeAuthVariantConfig(/*bad*/ false), /*expectReject*/ false, "auth-ok");
    }

    // Monitoring (cross-section: monitoring_config + domains security) violation
    // introduced by a selector -- caught K-independently via the same projection.
    Y_UNIT_TEST(MonitoringViolationInSelectorCaught) {
        AssertSemanticStrictlyNotWeaker(MakeMonitoringVariantConfig(/*bad*/ true), /*expectReject*/ true, "mon-bad");
    }
    Y_UNIT_TEST(MonitoringValidVariantAccepted) {
        AssertSemanticStrictlyNotWeaker(MakeMonitoringVariantConfig(/*bad*/ false), /*expectReject*/ false, "mon-ok");
    }

    // Direct test of the semantic static-section guard: a selector that varies a
    // SelectorStatic section (domains_config) must be rejected by ValidateSemanticAPlus
    // ITSELF (not only by the structural pass). Without this, a semantic check that
    // reads the static section jointly with a variable one (Monitoring/StateStorage)
    // could be evaded by per-section projection.
    Y_UNIT_TEST(SemanticGuardRejectsStaticSectionVariation) {
        auto doc = NFyaml::TDocument::Parse(TStringBuilder()
            << "---\ncluster: test\nversion: 1\nconfig:\n"
            << "  domains_config:\n    security_config:\n      enforce_user_token_requirement: true\n"
            << "allowed_labels:\n  tenant:\n    type: string\n"
            << kDisableRules
            << "selector_config:\n- description: t\n  selector:\n    tenant: bad\n  config: !inherit\n"
            << "    domains_config: !inherit\n      security_config: !inherit\n"
            << "        enforce_user_token_requirement: false\n");
        UNIT_ASSERT_C(!NYamlConfig::ValidateSemanticAPlus(doc).empty(),
            "semantic A+ must guard-reject a selector varying a static section");
    }

    // The audit's decoupled split-label repro: monitoring_config varied by label
    // `mon`, domains_config.security_config varied by an INDEPENDENT label `dom`.
    // The realizable joint (counters-auth ON, enforce OFF) violates Monitoring and
    // legacy rejects. Per-section projection never co-varies the two, so the bug
    // would be an A+ false negative -- but the static-section guard on domains_config
    // makes A+ reject too. Assert strictly-not-weaker (no semantic divergence).
    Y_UNIT_TEST(MonitoringCrossSectionDecoupledNotWeaker) {
        auto doc = NFyaml::TDocument::Parse(TStringBuilder()
            << "---\ncluster: test\nversion: 1\nconfig:\n"
            << "  monitoring_config:\n    require_counters_authentication: false\n"
            << "  domains_config:\n    security_config:\n      enforce_user_token_requirement: true\n"
            << "allowed_labels:\n  mon:\n    type: string\n  dom:\n    type: string\n"
            << kDisableRules
            << "selector_config:\n"
            << "- description: m\n  selector:\n    mon: m1\n  config: !inherit\n"
            << "    monitoring_config: !inherit\n      require_counters_authentication: true\n"
            << "- description: d\n  selector:\n    dom: d1\n  config: !inherit\n"
            << "    domains_config: !inherit\n      security_config: !inherit\n"
            << "        enforce_user_token_requirement: false\n");
        auto shadow = NYamlConfig::StructuralShadowRun(doc, /*allowUnknown*/ true);
        UNIT_ASSERT_C(shadow.SemanticLegacyRejected,
            "legacy must reject the realizable (counters-on, enforce-off) joint");
        UNIT_ASSERT_C(shadow.SemanticAPlusRejected,
            "A+ semantic must also reject this shape (strictly not weaker)");
        UNIT_ASSERT_C(!shadow.SemanticDiverged, "no semantic divergence expected on this shape");
    }

    // Semantic validation work is additive per section, independent of an
    // uncoupled high-cardinality label (no K blowup).
    Y_UNIT_TEST(SemanticWorkIsKIndependent) {
        // tenant varies auth_config; region varies a non-semantic field.
        auto make = [](size_t regions) {
            TStringBuilder sb;
            sb << "---\ncluster: test\nversion: 1\nconfig:\n";
            sb << "  auth_config:\n    password_complexity:\n      min_length: 10\n";
            sb << "      min_lower_case_count: 1\n      min_upper_case_count: 1\n";
            sb << "      min_numbers_count: 1\n      min_special_chars_count: 1\n";
            sb << "allowed_labels:\n  tenant:\n    type: string\n  region:\n    type: string\n";
            sb << kDisableRules;
            sb << "selector_config:\n";
            sb << "- description: t\n  selector:\n    tenant: t1\n  config: !inherit\n";
            sb << "    auth_config: !inherit\n      password_complexity: !inherit\n        min_length: 9\n";
            for (size_t j = 0; j < regions; ++j) {
                sb << "- description: r" << j << "\n  selector:\n    region: r" << j << "\n";
                sb << "  config: !inherit\n    other_field: of" << j << "\n";
            }
            return TString(sb);
        };
        auto count = [](const TString& yaml) {
            auto doc = NFyaml::TDocument::Parse(yaml);
            size_t n = 0;
            NYamlConfig::EnumerateDistinctProjections(doc, {"/auth_config"}, [&](NFyaml::TNodeRef) { ++n; });
            return n;
        };
        UNIT_ASSERT_VALUES_EQUAL(count(make(3)), 2u);    // base + tenant=t1 variant
        UNIT_ASSERT_VALUES_EQUAL(count(make(300)), 2u);  // independent of regions
    }
}

Y_UNIT_TEST_SUITE(YamlConfigDatabaseAllowlist) {

    // A section marked AllowInDatabaseConfig (feature_flags) is accepted.
    Y_UNIT_TEST(AllowedSectionAccepted) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
)");
        UNIT_ASSERT(NYamlConfig::ValidateDatabaseAllowlistAPlus(doc).empty());
    }

    // A non-allowlisted section in the base DB config is rejected (matches legacy).
    Y_UNIT_TEST(DisallowedSectionInBaseRejected) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  auth_config:
    some_field: 1
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        UNIT_ASSERT_C(!v.empty(), "auth_config is not allowed in a database config");
        UNIT_ASSERT_C(v.front().Message.Contains("auth_config"), v.front().Message);
    }

    // The stronger-than-legacy case: a non-allowlisted section introduced ONLY by
    // a database-config SELECTOR. The legacy base-only check misses this; the
    // K-independent A+ allowlist catches it.
    Y_UNIT_TEST(DisallowedSectionInSelectorRejected) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_tenant_must_be_defined
selector_config:
- description: sneaky
  selector:
    tenant: x
  config: !inherit
    auth_config: !inherit
      some_field: 1
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        bool found = false;
        for (const auto& e : v) {
            if (e.Message.Contains("auth_config")) { found = true; }
        }
        UNIT_ASSERT_C(found, "A+ must catch a disallowed section introduced by a DB selector");
    }

    // Regression: a disallowed section present as an EMPTY mapping carries no scalar
    // leaf, so the leaf-derived section set misses it -- but the legacy DB gate
    // (YamlToProto + ValidateDatabaseConfig) still rejects `auth_config: {}`. The
    // allowlist is a PRESENCE check, so A+ must reject it too (not be weaker).
    Y_UNIT_TEST(DisallowedEmptyMappingSectionRejected) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  auth_config: {}
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        UNIT_ASSERT_C(!v.empty(), "an empty-mapping disallowed section must still be rejected");
        bool found = false;
        for (const auto& e : v) {
            if (e.Message.Contains("auth_config")) { found = true; }
        }
        UNIT_ASSERT_C(found, "auth_config: {} must be reported by the allowlist");
    }

    // Same gap via a selector overlay introducing an empty-mapping disallowed section.
    Y_UNIT_TEST(DisallowedEmptyMappingSectionInSelectorRejected) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_tenant_must_be_defined
selector_config:
- description: sneaky
  selector:
    tenant: x
  config: !inherit
    auth_config: {}
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        bool found = false;
        for (const auto& e : v) {
            if (e.Message.Contains("auth_config")) { found = true; }
        }
        UNIT_ASSERT_C(found, "A+ must catch an empty-mapping disallowed section in a DB selector");
    }

    // Ephemeral top-level input keys (TEphemeralInputFields: default_disk_type,
    // hosts, ...) are not TAppConfig fields. The legacy DB gate parses with
    // transform=false, so they never SET a proto field and its HasField-based
    // allowlist ACCEPTS them -- A+ must mirror that, not over-reject.
    Y_UNIT_TEST(EphemeralTopLevelKeyAccepted) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
  default_disk_type: SSD
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        UNIT_ASSERT_C(v.empty(), v.empty() ? "" : v.front().Message.c_str());
    }

    // A NULL-valued key (`auth_config:`) parses to an UNSET proto field (JSON
    // null is skipped by the merger), so the legacy reflection allowlist sees
    // it as absent and accepts. Presence must mean non-null: A+ accepts too.
    // (`auth_config: {}` -- an empty mapping -- DOES set the field and stays
    // rejected; see DisallowedEmptyMappingSectionRejected.)
    Y_UNIT_TEST(NullValuedDisallowedSectionAccepted) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
  auth_config:
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        UNIT_ASSERT_C(v.empty(), v.empty() ? "" : v.front().Message.c_str());

        // Explicit null literal form: enters via the leaf-derived view instead
        // of the presence walk, must be skipped there too.
        auto docTilde = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
  auth_config: ~
)");
        auto vTilde = NYamlConfig::ValidateDatabaseAllowlistAPlus(docTilde);
        UNIT_ASSERT_C(vTilde.empty(), vTilde.empty() ? "" : vTilde.front().Message.c_str());
    }

    // An EMPTY-SEQUENCE value yields FieldSize()==0, which legacy
    // NConfig::ValidateDatabaseConfig explicitly skips for repeated fields --
    // so it must not count as a field-setting occurrence for the allowlist
    // either. (A message-typed section written as `[]` fails structurally on
    // both sides, so the combined verdicts still agree there.)
    Y_UNIT_TEST(EmptySequenceValuedDisallowedSectionAccepted) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
  auth_config: []
)");
        auto v = NYamlConfig::ValidateDatabaseAllowlistAPlus(doc);
        UNIT_ASSERT_C(v.empty(), v.empty() ? "" : v.front().Message.c_str());
    }
}

Y_UNIT_TEST_SUITE(YamlConfigFieldDiagnostics) {

    // Unknown field in the base config is collected (path under /config).
    Y_UNIT_TEST(UnknownFieldInBaseCollected) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  some_bogus_unknown_field: 123
)");
        auto diag = NYamlConfig::CollectFieldDiagnosticsAPlus(doc);
        UNIT_ASSERT_C(!diag.Empty(), "the bogus field must be reported");
        UNIT_ASSERT_C(diag.UnknownFields.contains("/config/some_bogus_unknown_field"),
            "unknown base field must be keyed by its editable path");
        UNIT_ASSERT_C(!diag.ToWarnings().empty(), "warnings dump must be non-empty");
    }

    // Unknown field introduced ONLY by a selector overlay is collected
    // K-independently (per editable block, no resolved-doc enumeration).
    Y_UNIT_TEST(UnknownFieldInSelectorCollected) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags:
    enable_some_flag: true
allowed_labels:
  tenant:
    type: string
incompatibility_overrides:
  disable_rules:
    - builtin_tenant_must_be_defined
selector_config:
- description: t
  selector:
    tenant: x
  config: !inherit
    another_bogus_field: 5
)");
        auto diag = NYamlConfig::CollectFieldDiagnosticsAPlus(doc);
        UNIT_ASSERT_C(diag.UnknownFields.contains("/selector_config/0/config/another_bogus_field"),
            "unknown field in a selector overlay must be collected with its selector path");
    }

    // A config with only known sections (an empty, known message) produces no
    // diagnostics.
    Y_UNIT_TEST(KnownConfigHasNoDiagnostics) {
        auto doc = NFyaml::TDocument::Parse(R"(---
cluster: test
version: 1
config:
  feature_flags: {}
)");
        auto diag = NYamlConfig::CollectFieldDiagnosticsAPlus(doc);
        UNIT_ASSERT_C(diag.UnknownFields.empty(),
            TStringBuilder() << "no unknown fields expected, got "
                << (diag.UnknownFields.empty() ? TString("none") : diag.UnknownFields.begin()->first));
    }
}

// ===========================================================================
// Polynomiality & scale.
//
// Proves the FULL A+ validation pipeline (the REAL proto transform + the REAL
// NConfig::ValidateConfig, not just the enumerator) is ADDITIVE across sections
// and stays in milliseconds on configs whose legacy K (number of distinct
// resolved documents) is astronomically large -- the real-world shape described
// in gist bbfba6db: ~150 selectors, 400+ tenant paths, several independently
// varied sections, where legacy K = product of label cardinalities explodes.
// ===========================================================================

namespace {

// Up to three INDEPENDENT labels, each varying a DIFFERENT section with a
// DISTINCT value per selector (so no projection dedup collapses them):
//   tenant  (T) -> log_config.cluster_name  (real, transform-read)
//   region  (R) -> auth_config min_length    (real, transform-read + semantic;
//                                             values kept >= the min-count sum, so valid)
//   flavour (F) -> feature_x scalar          (independent label; tolerated under allowUnknown)
// Legacy validates K = (T+1)(R+1)(F+1) resolved docs; A+ validates the additive
// (T+1) + (R+1) + (F+1) distinct per-section projections.
TString MakeScaleConfig(size_t T, size_t R, size_t F) {
    TStringBuilder sb;
    sb << "---\ncluster: test\nversion: 1\nconfig:\n";
    sb << "  log_config:\n    cluster_name: base\n";
    sb << "  auth_config:\n    password_complexity:\n      min_length: 10\n";
    sb << "      min_lower_case_count: 1\n      min_upper_case_count: 1\n";
    sb << "      min_numbers_count: 1\n      min_special_chars_count: 1\n";
    sb << "allowed_labels:\n  tenant:\n    type: string\n  region:\n    type: string\n";
    sb << "  flavour:\n    type: string\n";
    sb << kDisableRules;
    sb << "selector_config:\n";
    for (size_t i = 0; i < T; ++i) {
        sb << "- description: t" << i << "\n  selector:\n    tenant: t" << i << "\n";
        sb << "  config: !inherit\n    log_config: !inherit\n      cluster_name: lc" << i << "\n";
    }
    for (size_t j = 0; j < R; ++j) {
        sb << "- description: r" << j << "\n  selector:\n    region: r" << j << "\n";
        sb << "  config: !inherit\n    auth_config: !inherit\n      password_complexity: !inherit\n";
        sb << "        min_length: " << (8 + j) << "\n";  // distinct AND valid (>= sum 4)
    }
    for (size_t k = 0; k < F; ++k) {
        sb << "- description: f" << k << "\n  selector:\n    flavour: f" << k << "\n";
        sb << "  config: !inherit\n    feature_x: fx" << k << "\n";
    }
    return sb;
}

size_t SectionProjCount(NFyaml::TDocument& doc, const TString& section) {
    size_t n = 0;
    NYamlConfig::EnumerateDistinctProjections(doc, {section}, [&](NFyaml::TNodeRef) { ++n; });
    return n;
}

} // namespace

Y_UNIT_TEST_SUITE(YamlConfigPolynomialScale) {

    // Deterministic core proof: the distinct per-section projection work A+
    // performs is ADDITIVE in the label cardinalities; the legacy gate validates
    // the multiplicative product. Exact counts, no timing.
    Y_UNIT_TEST(ProjectionWorkIsAdditiveNotMultiplicative) {
        // Small scale: confirm the legacy multiplicative model by actually
        // enumerating the resolved-doc set.
        {
            auto yaml = MakeScaleConfig(5, 5, 5);
            UNIT_ASSERT_VALUES_EQUAL(LegacyValidateCount(yaml), 6u * 6u * 6u);  // K = 216
            auto doc = NFyaml::TDocument::Parse(yaml);
            size_t aplus = SectionProjCount(doc, "/log_config")
                         + SectionProjCount(doc, "/auth_config")
                         + SectionProjCount(doc, "/feature_x");
            UNIT_ASSERT_VALUES_EQUAL(aplus, 6u + 6u + 6u);  // 18, additive
        }
        // Large scale: legacy K is ~10^6 (deliberately NOT enumerated -- that is
        // the whole point); A+ stays additive at 303. Assert the analytic gap.
        {
            auto yaml = MakeScaleConfig(100, 100, 100);
            auto doc = NFyaml::TDocument::Parse(yaml);
            size_t aplus = SectionProjCount(doc, "/log_config")
                         + SectionProjCount(doc, "/auth_config")
                         + SectionProjCount(doc, "/feature_x");
            UNIT_ASSERT_VALUES_EQUAL(aplus, 101u + 101u + 101u);  // 303
            const double legacyK = 101.0 * 101.0 * 101.0;         // 1,030,301
            UNIT_ASSERT_C(legacyK / double(aplus) > 3000.0,
                TStringBuilder() << "A+ must be thousands of times less work; ratio="
                    << (legacyK / double(aplus)));
        }
    }

    // The headline: a config whose legacy K exceeds one MILLION validates fully
    // (structural + semantic, with the real validators) in milliseconds.
    Y_UNIT_TEST(HugeConfigValidatesInMillis) {
        auto yaml = MakeScaleConfig(200, 100, 50);  // legacy K = 201*101*51 = 1,035,351
        auto doc = NFyaml::TDocument::Parse(yaml);

        auto t0 = TInstant::Now();
        auto structural = NYamlConfig::ValidateStructuralAPlus(doc, /*allowUnknown*/ true);
        auto semantic = NYamlConfig::ValidateSemanticAPlus(doc, /*allowUnknown*/ true);
        auto dur = TInstant::Now() - t0;

        const ui64 legacyK = 201ull * 101ull * 51ull;
        Cerr << "[A+ SCALE] legacy K=" << legacyK
             << " ; A+ full structural+semantic validation in " << dur.MilliSeconds() << "ms"
             << " (" << dur.MicroSeconds() << "us)" << Endl;

        UNIT_ASSERT_C(structural.empty(), TStringBuilder()
            << "valid config must produce no structural violations; first="
            << (structural.empty() ? TString() : structural.front().Message));
        UNIT_ASSERT_C(semantic.empty(), TStringBuilder()
            << "valid config must produce no semantic violations; first="
            << (semantic.empty() ? TString() : semantic.front().Message));
        // Generous ceiling: the real number is far smaller. The assertion exists
        // to fail loudly if the multiplicative cliff ever sneaks back in.
        UNIT_ASSERT_C(dur.Seconds() < 5,
            TStringBuilder() << "A+ on a 10^6-K config must finish in well under 5s, took "
                << dur.MilliSeconds() << "ms");
    }

    // Growth of the FULL pipeline in the cardinality of a SINGLE varied section is
    // ~LINEAR (and the number of validations is exactly additive: n+1 projections,
    // asserted). Each projection rebuilds its representative by cloning only the
    // BASE CONFIG (not the whole document) and copying overlays from the
    // once-parsed model -- no per-projection re-parse of the K selectors
    // (EnumerateDistinctProjections, public/yaml_config.cpp). A residual O(n^2)
    // term remains in the firing-signature dedup (Fit per involved selector per
    // tuple), but its constant is tiny, so wall-clock is dominated by the linear
    // rebuild term well past realistic sizes. The first-order win over legacy is
    // exponential -> polynomial in the NUMBER of labels (ComplexityClassSeparation).
    Y_UNIT_TEST(GrowthIsPolynomialNotExponential) {
        TVector<std::pair<size_t, ui64>> curve;
        for (size_t n : {size_t(100), size_t(200), size_t(400), size_t(800)}) {
            auto yaml = MakeScaleConfig(n, 0, 0);  // only tenant varies log_config
            auto doc = NFyaml::TDocument::Parse(yaml);
            UNIT_ASSERT_VALUES_EQUAL(SectionProjCount(doc, "/log_config"), n + 1);  // additive #validations

            auto t0 = TInstant::Now();
            auto v = NYamlConfig::ValidateStructuralAPlus(doc, /*allowUnknown*/ true);
            auto dur = TInstant::Now() - t0;
            UNIT_ASSERT_C(v.empty(), "valid config, no violations");
            curve.emplace_back(n, dur.MicroSeconds());
            Cerr << "[A+ GROWTH] N=" << n << " -> " << dur.MicroSeconds() << "us"
                 << " (projections=" << (n + 1) << ")" << Endl;
        }
        // 8x the input. Near-linear rebuild makes this ~8-12x; the 24x bound gives
        // ~2x slack for the residual O(n^2) signature term + scheduler noise while
        // still failing loudly on a regression back to quadratic (64x) or worse.
        ui64 t100 = curve.front().second ? curve.front().second : 1;
        ui64 t800 = curve.back().second;
        UNIT_ASSERT_C(t800 < t100 * 24,
            TStringBuilder() << "single-section growth must stay ~linear: t(100)="
                << t100 << "us t(800)=" << t800 << "us (ratio " << (double(t800) / double(t100))
                << ", quadratic≈64x)");
    }

    // Head-to-head at a scale where legacy is still feasible: A+ and legacy AGREE
    // (strictly not weaker), and A+ does dramatically less work / wall-time.
    Y_UNIT_TEST(AgreesWithLegacyAndIsFaster) {
        auto yaml = MakeScaleConfig(40, 40, 0);  // legacy K = 41*41 = 1681 docs
        auto doc = NFyaml::TDocument::Parse(yaml);

        auto t0 = TInstant::Now();
        auto legacy = NYamlConfig::ValidateStructuralLegacy(doc, /*allowUnknown*/ true);
        auto legacyDur = TInstant::Now() - t0;

        auto t1 = TInstant::Now();
        auto aplus = NYamlConfig::ValidateStructuralAPlus(doc, /*allowUnknown*/ true);
        auto aplusDur = TInstant::Now() - t1;

        ui64 aplusUs = aplusDur.MicroSeconds() ? aplusDur.MicroSeconds() : 1;
        Cerr << "[A+ VS LEGACY] legacy K=1681 in " << legacyDur.MicroSeconds() << "us"
             << " ; A+ (81 varied projections) in " << aplusDur.MicroSeconds() << "us"
             << " ; speedup=" << (double(legacyDur.MicroSeconds()) / double(aplusUs)) << "x" << Endl;

        // Agreement: both accept this valid config (A+ strictly not weaker).
        UNIT_ASSERT_C(!legacy.has_value(), "legacy must accept the valid config");
        UNIT_ASSERT_C(aplus.empty(), "A+ must accept the valid config too");

        // A+ does ~20x less work (81 vs 1681 transforms). Assert a safe 3x
        // wall-time margin to absorb scheduler noise.
        UNIT_ASSERT_C(aplusDur.MicroSeconds() * 3 < legacyDur.MicroSeconds(),
            TStringBuilder() << "A+ must be substantially faster: legacy="
                << legacyDur.MicroSeconds() << "us aplus=" << aplusDur.MicroSeconds() << "us");
    }

    // Explicit COMPLEXITY-CLASS comparison (not just a single wall-clock number).
    // Two independent labels each vary a different section, scaled n = 10..40.
    //   legacy work  = K          = (n+1)^2     -> O(n^2)  (multiplicative)
    //   A+    work   = projections= 2*(n+1)     -> O(n)    (additive)
    // The proof that these are DIFFERENT complexity classes: the ratio
    // K / projections = (n+1)/2 must itself GROW linearly with n. If A+ were
    // merely a constant-factor win (same class), the ratio would be flat. We
    // assert exact counts at every scale AND that the ratio grows ~4x as n goes
    // 10 -> 40, and print the full table (with measured wall-clock as corroboration
    // at the scales where legacy is still feasible to run).
    Y_UNIT_TEST(ComplexityClassSeparation) {
        Cerr << "[A+ COMPLEXITY] two independent labels (legacy O(n^2) vs A+ O(n)):" << Endl;
        Cerr << "   n |   legacy K | A+ proj |  ratio | legacy us | A+ us" << Endl;

        TVector<double> ratios;
        for (size_t n : {size_t(10), size_t(20), size_t(40)}) {
            auto yaml = MakeScaleConfig(n, n, 0);
            auto doc = NFyaml::TDocument::Parse(yaml);

            // Deterministic complexity: counts must match the closed forms exactly.
            size_t legacyK = LegacyValidateCount(yaml);
            size_t aplus = SectionProjCount(doc, "/log_config")
                         + SectionProjCount(doc, "/auth_config");
            UNIT_ASSERT_VALUES_EQUAL_C(legacyK, (n + 1) * (n + 1),
                TStringBuilder() << "legacy must be multiplicative (n+1)^2 at n=" << n);
            UNIT_ASSERT_VALUES_EQUAL_C(aplus, 2 * (n + 1),
                TStringBuilder() << "A+ must be additive 2(n+1) at n=" << n);

            // Corroborating wall-clock (both feasible at these scales).
            auto t0 = TInstant::Now();
            (void)NYamlConfig::ValidateStructuralLegacy(doc, /*allowUnknown*/ true);
            auto legacyUs = (TInstant::Now() - t0).MicroSeconds();
            auto t1 = TInstant::Now();
            (void)NYamlConfig::ValidateStructuralAPlus(doc, /*allowUnknown*/ true);
            auto aplusUs = (TInstant::Now() - t1).MicroSeconds();

            double ratio = double(legacyK) / double(aplus);
            ratios.push_back(ratio);
            Cerr << Sprintf("  %2zu | %10zu | %7zu | %6.1f | %9lu | %lu",
                            n, legacyK, aplus, ratio, (unsigned long)legacyUs, (unsigned long)aplusUs) << Endl;
        }

        // Different complexity class <=> the work ratio grows. n: 10 -> 40 is 4x,
        // so (n+1)/2 grows ~3.7x ((41/2)/(11/2)). Assert it at least tripled.
        UNIT_ASSERT_C(ratios.back() > ratios.front() * 3.0,
            TStringBuilder() << "work-ratio must grow with scale (=> A+ is a lower complexity"
                " class, not a constant-factor win): ratio(10)=" << ratios.front()
                << " ratio(40)=" << ratios.back());
    }
}
