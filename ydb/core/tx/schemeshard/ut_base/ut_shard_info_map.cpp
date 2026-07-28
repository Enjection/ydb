#include <ydb/core/tx/schemeshard/schemeshard_info_types.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr;
using namespace NSchemeShard;

namespace {

// A null shard makes acquire/release no-ops (TPathDbRef guards on it), so the
// container's own semantics can be tested without standing up a TSchemeShard.
// Reference counting is covered by the reboot and split/merge suites.
TShardIdx Idx(ui64 localId) {
    return TShardIdx(TOwnerId(1), TLocalShardIdx(localId));
}

TPathId Path(ui64 localId) {
    return TPathId(TOwnerId(1), TLocalPathId(localId));
}

TShardInfo Info(ui64 pathLocalId) {
    return TShardInfo(TTxId(1), Path(pathLocalId), ETabletType::DataShard);
}

template <class T, class = void>
struct TCanSubscript: std::false_type {};

template <class T>
struct TCanSubscript<
    T,
    std::void_t<decltype(std::declval<T&>()[std::declval<TShardIdx>()])>>: std::true_type {};

} // namespace

Y_UNIT_TEST_SUITE(TShardInfoMapTest) {

    Y_UNIT_TEST(IsNotCopyableOrMovable) {
        static_assert(!std::is_copy_constructible_v<TShardInfoMap>);
        static_assert(!std::is_copy_assignable_v<TShardInfoMap>);
        static_assert(!std::is_move_constructible_v<TShardInfoMap>);
        static_assert(!std::is_move_assignable_v<TShardInfoMap>);
    }

    Y_UNIT_TEST(EmplaceInsertsAndKeepsState) {
        TShardInfoMap map(nullptr);
        UNIT_ASSERT(map.empty());

        TShardInfo& stored = map.Emplace(Idx(1), Info(10));

        UNIT_ASSERT_VALUES_EQUAL(map.size(), 1u);
        UNIT_ASSERT(map.contains(Idx(1)));
        UNIT_ASSERT_EQUAL(stored.PathId, Path(10));
        UNIT_ASSERT_EQUAL(map.FindPtr(Idx(1)), &stored);
    }

    // There is no operator[]: a default-inserted entry would hold no reference, so the
    // only way in is Emplace.
    Y_UNIT_TEST(HasNoSubscriptOperator) {
        static_assert(!TCanSubscript<TShardInfoMap>::value,
            "TShardInfoMap must not offer operator[]; a default-insert would be an "
            "entry holding no reference");
    }

    Y_UNIT_TEST(AtGivesMutableAccess) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));

        map.at(Idx(1)).CurrentTxId = TTxId(7);

        UNIT_ASSERT_EQUAL(map.at(Idx(1)).CurrentTxId, TTxId(7));
        UNIT_ASSERT_VALUES_EQUAL(map.size(), 1u);
    }

    Y_UNIT_TEST(EraseRemovesOnlyItsEntry) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));
        map.Emplace(Idx(2), Info(11));

        UNIT_ASSERT_VALUES_EQUAL(map.erase(Idx(1)), 1u);
        UNIT_ASSERT_VALUES_EQUAL(map.erase(Idx(1)), 0u);

        UNIT_ASSERT(!map.contains(Idx(1)));
        UNIT_ASSERT(map.contains(Idx(2)));
    }

    Y_UNIT_TEST(ReassignPathMovesTheEntry) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));

        map.ReassignPath(Idx(1), Path(20));

        UNIT_ASSERT_EQUAL(map.at(Idx(1)).PathId, Path(20));
    }

    Y_UNIT_TEST(ReassignPathToSamePathIsANoop) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));

        map.ReassignPath(Idx(1), Path(10));

        UNIT_ASSERT_EQUAL(map.at(Idx(1)).PathId, Path(10));
    }

    Y_UNIT_TEST(RollbackHelpersDropAndRestore) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));

        const TShardInfo snapshot = map.at(Idx(1));

        UNIT_ASSERT_VALUES_EQUAL(map.EraseWithoutRelease(Idx(1)), 1u);
        UNIT_ASSERT(!map.contains(Idx(1)));

        map.RestoreWithoutAcquire(Idx(1), snapshot);
        UNIT_ASSERT(map.contains(Idx(1)));
        UNIT_ASSERT_EQUAL(map.at(Idx(1)).PathId, Path(10));
    }

    Y_UNIT_TEST(IterationVisitsEveryEntry) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));
        map.Emplace(Idx(2), Info(11));

        THashSet<TShardIdx> seen;
        for (const auto& [shardIdx, shardInfo] : map) {
            seen.insert(shardIdx);
        }

        UNIT_ASSERT_VALUES_EQUAL(seen.size(), 2u);
        UNIT_ASSERT(seen.contains(Idx(1)));
        UNIT_ASSERT(seen.contains(Idx(2)));
    }

    Y_UNIT_TEST(ClearDropsEverything) {
        TShardInfoMap map(nullptr);
        map.Emplace(Idx(1), Info(10));
        map.Emplace(Idx(2), Info(11));

        map.clear();

        UNIT_ASSERT(map.empty());
    }
}
