import yatest.common
from ydb.tests.library.harness.kikimr_runner import KiKiMR
from ydb.tests.library.harness.kikimr_config import KikimrConfigGenerator
from ydb.tests.olap.lib.ydb_cluster import YdbCluster
from ydb.tests.olap.lib.ydb_cli import YdbCliHelper


class FunctionalTestBase:
    cluster = None

    @classmethod
    def setup_cluster(cls, table_service_config: dict = {}, memory_controller_config: dict = {}) -> None:
        config_generator = KikimrConfigGenerator(
            domain_name='local',
            extra_feature_flags=["enable_resource_pools"],
            use_in_memory_pdisks=True,
        )
        if table_service_config:
            config_generator.yaml_config["table_service_config"] = table_service_config

        if memory_controller_config:
            config_generator.yaml_config["memory_controller_config"] = memory_controller_config

        cls.cluster = KiKiMR(configurator=config_generator)
        cls.cluster.start()
        node = cls.cluster.nodes[1]
        YdbCluster.reset(
            ydb_endpoint=f'grpc://{node.host}:{node.grpc_port}',
            ydb_database=f'{cls.cluster.domain_name}/test_db',
            ydb_mon_port=node.mon_port,
            dyn_nodes_count=1
        )
        db = f'/{YdbCluster.ydb_database}'
        cls.cluster.create_database(
            db,
            storage_pool_units_count={
                'hdd': 1
            }
        )
        cls.cluster.register_and_start_slots(db, count=YdbCluster.get_dyn_nodes_count())
        cls.cluster.wait_tenant_up(db)

    def setup_method(self, method):
        for node in self.cluster.nodes.values():
            node.start()
        for slot in self.cluster.slots.values():
            slot.start()
        self.cluster.wait_tenant_up(f'/{YdbCluster.ydb_database}')

    def teardown_method(self, method):
        for node in self.cluster.nodes.values():
            if not node.is_alive():
                node.stop()
        for slot in self.cluster.slots.values():
            if not slot.is_alive():
                slot.stop()

    @classmethod
    def run_cli(cls, argv: list[str]) -> yatest.common.process._Execution:
        return yatest.common.execute(YdbCliHelper.get_cli_command() + argv)
