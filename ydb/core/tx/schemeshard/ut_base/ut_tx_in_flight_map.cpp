#include <ydb/core/tx/schemeshard/schemeshard_tx_infly.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr;
using namespace NSchemeShard;

namespace {

// A null shard makes acquire/release no-ops (TPathDbRef guards on it), so the
// container's own semantics can be tested without standing up a TSchemeShard.
// Reference counting is covered by the reboot and split/merge suites.
TOperationId OpId(ui64 txId, ui32 partId = 0) {
    return TOperationId(TTxId(txId), TSubTxId(partId));
}

TPathId Path(ui64 localId) {
    return TPathId(TOwnerId(1), TLocalPathId(localId));
}

template <class T, class = void>
struct TCanAcquirePathRefs: std::false_type {};

template <class T>
struct TCanAcquirePathRefs<
    T,
    std::void_t<decltype(std::declval<T&>().AcquirePathRefs(nullptr, true))>>: std::true_type {};

} // namespace

Y_UNIT_TEST_SUITE(TTxInFlightMapTest) {

    // The point of the type: nothing outside can arm an entry, and the map cannot be
    // duplicated into a second set of references.
    Y_UNIT_TEST(ArmingIsNotReachableFromOutside) {
        static_assert(!TCanAcquirePathRefs<TTxState>::value,
            "AcquirePathRefs must stay private to TTxInFlightMap");

        static_assert(!std::is_copy_constructible_v<TTxInFlightMap>);
        static_assert(!std::is_copy_assignable_v<TTxInFlightMap>);
        static_assert(!std::is_move_constructible_v<TTxInFlightMap>);
        static_assert(!std::is_move_assignable_v<TTxInFlightMap>);
    }

    Y_UNIT_TEST(EmplaceInsertsAndKeepsState) {
        TTxInFlightMap map(nullptr);
        UNIT_ASSERT(map.empty());

        TTxState& stored = map.Emplace(
            OpId(1),
            TTxState(TTxState::TxCreateTable, Path(10), Path(20)));

        UNIT_ASSERT_VALUES_EQUAL(map.size(), 1u);
        UNIT_ASSERT(map.contains(OpId(1)));
        UNIT_ASSERT_EQUAL(stored.TxType, TTxState::TxCreateTable);
        UNIT_ASSERT_EQUAL(stored.TargetPathId, Path(10));
        UNIT_ASSERT_EQUAL(stored.SourcePathId, Path(20));
    }

    Y_UNIT_TEST(EmplaceReturnsReferenceIntoMap) {
        TTxInFlightMap map(nullptr);

        TTxState& stored = map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));
        stored.State = TTxState::Propose;

        UNIT_ASSERT_EQUAL(map.at(OpId(1)).State, TTxState::Propose);
        UNIT_ASSERT_EQUAL(map.FindPtr(OpId(1)), &stored);
    }

    Y_UNIT_TEST(DistinctPartIdsAreDistinctEntries) {
        TTxInFlightMap map(nullptr);

        map.Emplace(OpId(1, 0), TTxState(TTxState::TxCreateTable, Path(10)));
        map.Emplace(OpId(1, 1), TTxState(TTxState::TxCreateTable, Path(11)));

        UNIT_ASSERT_VALUES_EQUAL(map.size(), 2u);
        UNIT_ASSERT_EQUAL(map.at(OpId(1, 0)).TargetPathId, Path(10));
        UNIT_ASSERT_EQUAL(map.at(OpId(1, 1)).TargetPathId, Path(11));
    }

    Y_UNIT_TEST(FindPtrAndContainsMissMissingKeys) {
        TTxInFlightMap map(nullptr);
        map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));

        UNIT_ASSERT(!map.contains(OpId(2)));
        UNIT_ASSERT_EQUAL(map.FindPtr(OpId(2)), nullptr);
        UNIT_ASSERT_EQUAL(map.FindPtr(OpId(1, 1)), nullptr);
    }

    Y_UNIT_TEST(EraseRemovesOnlyItsEntry) {
        TTxInFlightMap map(nullptr);
        map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));
        map.Emplace(OpId(2), TTxState(TTxState::TxDropTable, Path(11)));

        UNIT_ASSERT_VALUES_EQUAL(map.erase(OpId(1)), 1u);
        UNIT_ASSERT_VALUES_EQUAL(map.erase(OpId(1)), 0u);

        UNIT_ASSERT(!map.contains(OpId(1)));
        UNIT_ASSERT(map.contains(OpId(2)));
        UNIT_ASSERT_VALUES_EQUAL(map.size(), 1u);
    }

    // A tx with no source must not carry one, or RemoveTx would later release
    // against a path nothing acquired.
    Y_UNIT_TEST(AbsentSourceStaysInvalid) {
        TTxInFlightMap map(nullptr);

        TTxState& stored = map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));

        UNIT_ASSERT_EQUAL(stored.SourcePathId, InvalidPathId);
        UNIT_ASSERT(!stored.SourcePathId);
    }

    // The orphaned-restore path: inserted referencing its target only.
    Y_UNIT_TEST(EmplaceAcceptsMissingSource) {
        TTxInFlightMap map(nullptr);

        TTxState& stored = map.Emplace(
            OpId(1),
            TTxState(TTxState::TxCopyTable, Path(10), Path(20)),
            /* sourceExists */ false);

        UNIT_ASSERT_EQUAL(stored.TargetPathId, Path(10));
        UNIT_ASSERT_EQUAL(stored.SourcePathId, Path(20));
    }

    Y_UNIT_TEST(IterationVisitsEveryEntry) {
        TTxInFlightMap map(nullptr);
        map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));
        map.Emplace(OpId(2), TTxState(TTxState::TxDropTable, Path(11)));

        THashSet<TOperationId> seen;
        for (const auto& [opId, txState] : map) {
            seen.insert(opId);
        }

        UNIT_ASSERT_VALUES_EQUAL(seen.size(), 2u);
        UNIT_ASSERT(seen.contains(OpId(1)));
        UNIT_ASSERT(seen.contains(OpId(2)));
    }

    Y_UNIT_TEST(ClearDropsEverything) {
        TTxInFlightMap map(nullptr);
        map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));
        map.Emplace(OpId(2), TTxState(TTxState::TxDropTable, Path(11)));

        map.clear();

        UNIT_ASSERT(map.empty());
        UNIT_ASSERT_VALUES_EQUAL(map.size(), 0u);
    }

    Y_UNIT_TEST(EraseDisarmedRemovesEntry) {
        TTxInFlightMap map(nullptr);
        map.Emplace(OpId(1), TTxState(TTxState::TxCreateTable, Path(10)));

        UNIT_ASSERT_VALUES_EQUAL(map.EraseDisarmed(OpId(1)), 1u);
        UNIT_ASSERT_VALUES_EQUAL(map.EraseDisarmed(OpId(1)), 0u);
        UNIT_ASSERT(map.empty());
    }
}
