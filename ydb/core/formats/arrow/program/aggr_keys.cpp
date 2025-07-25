#include "aggr_keys.h"
#include "collection.h"
#include "execution.h"

#include <ydb/core/formats/arrow/accessor/plain/accessor.h>

#include <ydb/library/formats/arrow/switch/switch_type.h>

#include <util/string/join.h>

#ifndef WIN32
#ifdef NO_SANITIZE_THREAD
#undef NO_SANITIZE_THREAD
#endif
#include <AggregateFunctions/IAggregateFunction.h>
#else
namespace CH {
enum class AggFunctionId {
    AGG_UNSPECIFIED = 0,
    AGG_ANY = 1,
    AGG_COUNT = 2,
    AGG_MIN = 3,
    AGG_MAX = 4,
    AGG_SUM = 5,
    AGG_AVG = 6,
    //AGG_VAR = 7,
    //AGG_COVAR = 8,
    //AGG_STDDEV = 9,
    //AGG_CORR = 10,
    //AGG_ARG_MIN = 11,
    //AGG_ARG_MAX = 12,
    //AGG_COUNT_DISTINCT = 13,
    //AGG_QUANTILES = 14,
    //AGG_TOP_COUNT = 15,
    //AGG_TOP_SUM = 16,
    AGG_NUM_ROWS = 17,
};
struct GroupByOptions: public arrow::compute::ScalarAggregateOptions {
    struct Assign {
        AggFunctionId function = AggFunctionId::AGG_UNSPECIFIED;
        std::string result_column;
        std::vector<std::string> arguments;
    };

    std::shared_ptr<arrow::Schema> schema;
    std::vector<Assign> assigns;
    bool has_nullable_key = true;
};
}   // namespace CH
#endif

namespace NKikimr::NArrow::NSSA::NAggregation {

CH::AggFunctionId TWithKeysAggregationOption::GetHouseFunction(const EAggregate op) {
    switch (op) {
        case EAggregate::Some:
            return CH::AggFunctionId::AGG_ANY;
        case EAggregate::Count:
            return CH::AggFunctionId::AGG_COUNT;
        case EAggregate::Min:
            return CH::AggFunctionId::AGG_MIN;
        case EAggregate::Max:
            return CH::AggFunctionId::AGG_MAX;
        case EAggregate::Sum:
            return CH::AggFunctionId::AGG_SUM;
        case EAggregate::NumRows:
            return CH::AggFunctionId::AGG_NUM_ROWS;
        default:
            break;
    }
    return CH::AggFunctionId::AGG_UNSPECIFIED;
}

TString TWithKeysAggregationOption::DebugString() const {
    TStringBuilder sb;
    std::vector<ui32> ids;
    for (auto&& i : Inputs) {
        ids.emplace_back(i.GetColumnId());
    }
    sb << "{" << Output.GetColumnId() << "(" << AggregationId << ")" << ":[" << JoinSeq(",", ids) << "]}";
    return sb;
}

TConclusion<IResourceProcessor::EExecutionResult> TWithKeysAggregationProcessor::DoExecute(
    const TProcessorContext& context, const TExecutionNodeContext& /*nodeContext*/) const {
    CH::GroupByOptions funcOpts;
    funcOpts.assigns.reserve(AggregationKeys.size() + Aggregations.size());
    funcOpts.has_nullable_key = false;

    std::vector<arrow::Datum> batch;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::set<ui32> fieldsUsage;
    for (auto& key : AggregationKeys) {
        AFL_VERIFY(fieldsUsage.emplace(key.GetColumnId()).second);
        batch.emplace_back(context.GetResources()->GetArrayVerified(key.GetColumnId()));
        fields.emplace_back(context.GetResources()->GetFieldVerified(key.GetColumnId()));
        funcOpts.assigns.emplace_back(CH::GroupByOptions::Assign{ .result_column = ::ToString(key.GetColumnId()) });

        if (!funcOpts.has_nullable_key) {
            arrow::Datum res = batch.back();
            if (res.is_array()) {
                funcOpts.has_nullable_key = res.array()->MayHaveNulls();
            } else {
                return TConclusionStatus::Fail("GROUP BY may be for record batch only.");
            }
        }
    }
    for (auto& aggr : Aggregations) {
        const CH::GroupByOptions::Assign gbAssign = [&aggr]() {
            CH::GroupByOptions::Assign descr;
            descr.function = TWithKeysAggregationOption::GetHouseFunction(aggr.GetAggregationId());
            descr.result_column = ::ToString(aggr.GetOutput().GetColumnId());
            descr.arguments.reserve(aggr.GetInputs().size());

            for (auto& colName : aggr.GetInputs()) {
                descr.arguments.push_back(::ToString(colName.GetColumnId()));
            }
            return descr;
        }();

        funcOpts.assigns.emplace_back(gbAssign);
        for (auto&& i : aggr.GetInputs()) {
            if (fieldsUsage.emplace(i.GetColumnId()).second) {
                batch.emplace_back(context.GetResources()->GetArrayVerified(i.GetColumnId()));
                fields.emplace_back(context.GetResources()->GetFieldVerified(i.GetColumnId()));
            }
        }
    }

    funcOpts.schema = std::make_shared<arrow::Schema>(fields);

    auto gbRes = arrow::compute::CallFunction(GetHouseGroupByName(), batch, &funcOpts, GetCustomExecContext());
    if (!gbRes.ok()) {
        return TConclusionStatus::Fail(gbRes.status().ToString());
    }
    auto gbBatch = (*gbRes).record_batch();
    context.GetResources()->Remove(AggregationKeys);

    for (auto& assign : funcOpts.assigns) {
        auto column = gbBatch->GetColumnByName(assign.result_column);
        if (!column) {
            return TConclusionStatus::Fail("No expected column in GROUP BY result.");
        }
        if (auto columnId = TryFromString<ui32>(assign.result_column)) {
            context.GetResources()->AddVerified(*columnId, column, false);
        } else {
            return TConclusionStatus::Fail("Incorrect column id from name: " + assign.result_column);
        }
    }
    return IResourceProcessor::EExecutionResult::Success;
}

TConclusion<std::shared_ptr<TWithKeysAggregationProcessor>> TWithKeysAggregationProcessor::TBuilder::Finish() {
    AFL_VERIFY(!Finished);
    Finished = true;
    if (Keys.empty()) {
        return TConclusionStatus::Fail("no keys for aggregation");
    }
    if (Aggregations.empty()) {
        return TConclusionStatus::Fail("no aggregations");
    }
    std::set<ui32> input;
    std::set<ui32> output;
    for (auto&& i : Keys) {
        input.emplace(i.GetColumnId());
    }
    for (auto&& i : Aggregations) {
        for (auto&& inp : i.GetInputs()) {
            input.emplace(inp.GetColumnId());
        }
        output.emplace(i.GetOutput().GetColumnId());
    }
    std::vector<TColumnChainInfo> inputChainColumns;
    for (auto&& i : input) {
        inputChainColumns.emplace_back(i);
    }
    std::vector<TColumnChainInfo> outputChainColumns;
    for (auto&& i : output) {
        outputChainColumns.emplace_back(i);
    }
    return std::shared_ptr<TWithKeysAggregationProcessor>(new TWithKeysAggregationProcessor(
        std::move(inputChainColumns), std::move(outputChainColumns), std::move(Keys), std::move(Aggregations)));
}

TConclusionStatus TWithKeysAggregationProcessor::TBuilder::AddGroupBy(
    const std::vector<TColumnChainInfo>& input, const TColumnChainInfo& output, const EAggregate aggrType) {
    if (input.size() > 1) {
        return TConclusionStatus::Fail("a lot of columns for aggregation: " + JoinSeq(", ", input));
    }
    AFL_VERIFY(!Finished);
    Aggregations.emplace_back(input, output, aggrType);
    return TConclusionStatus::Success();
}

TConclusion<arrow::Datum> TAggregateFunction::Call(
    const TExecFunctionContext& context, const std::shared_ptr<TAccessorsCollection>& resources) const {
    if (context.GetColumns().size() == 0 && AggregationType == NAggregation::EAggregate::NumRows) {
        auto rc = resources->GetRecordsCountActualOptional();
        if (!rc) {
            return TConclusionStatus::Fail("resources hasn't info about records count actual");
        } else {
            return arrow::Datum(std::make_shared<arrow::UInt64Scalar>(*rc));
        }
    } else {
        return TBase::Call(context, resources);
    }
}

namespace {
class TResultsAggregator: public IResourcesAggregator {
private:
    const TColumnChainInfo ColumnInfo;
    const EAggregate AggregationType;
    virtual TConclusionStatus DoExecute(const std::vector<std::shared_ptr<TAccessorsCollection>>& sources,
        const std::shared_ptr<TAccessorsCollection>& collectionResult) const override {
        std::vector<const IChunkedArray*> arrays;
        std::optional<arrow::Type::type> type;
        for (auto&& i : sources) {
            AFL_VERIFY(i);
            const auto& acc = i->GetAccessorVerified(ColumnInfo.GetColumnId());
            AFL_VERIFY(acc->GetRecordsCount() == 1)("count", acc->GetRecordsCount());
            arrays.emplace_back(acc.get());
            if (!type) {
                type = acc->GetDataType()->id();
            } else {
                AFL_VERIFY(*type == acc->GetDataType()->id());
            }
        }
        TString errorMessage;
        if (!NArrow::SwitchType(*type, [&](const auto& type) {
                using TWrap = std::decay_t<decltype(type)>;
                using TArrayType = typename TWrap::TArray;
                std::optional<ui32> arrResultIndex;
                std::optional<typename TWrap::ValueType> result;
                ui32 idx = 0;
                for (auto&& i : arrays) {
                    auto addr = i->GetChunkSlow(0);
                    const typename TWrap::ValueType value = type.GetValue(*static_cast<const TArrayType*>(addr.GetArray().get()), 0);
                    if (!result) {
                        arrResultIndex = idx;
                        result = value;
                    } else {
                        switch (AggregationType) {
                            case EAggregate::Some:
                                break;
                            case EAggregate::Unspecified:
                            case EAggregate::Count:
                            case EAggregate::NumRows:
                                AFL_VERIFY(false);
                            case EAggregate::Sum:
                                if constexpr (TWrap::IsCType) {
                                    *result += value;
                                    arrResultIndex.reset();
                                }
                                if constexpr (TWrap::IsStringView) {
                                    errorMessage = "cannot sum string views";
                                    return false;
                                }
                                break;
                            case EAggregate::Max:
                                if (*result < value) {
                                    arrResultIndex = idx;
                                    result = value;
                                }
                                break;
                            case EAggregate::Min:
                                if (value < *result) {
                                    arrResultIndex = idx;
                                    result = value;
                                }
                                break;
                        }
                    }
                    ++idx;
                }
                if (arrResultIndex) {
                    collectionResult->AddVerified(
                        ColumnInfo.GetColumnId(), sources[*arrResultIndex]->GetAccessorVerified(ColumnInfo.GetColumnId()), false);
                } else {
                    collectionResult->AddVerified(ColumnInfo.GetColumnId(),
                        NAccessor::TTrivialArray::BuildArrayFromScalar(type.BuildScalar(*result, arrays.front()->GetDataType())), false);
                }
                return true;
            })) {
            return TConclusionStatus::Fail(errorMessage);
        }
        collectionResult->TakeSequenceFrom(*sources.front());
        return TConclusionStatus::Success();
    }

public:
    TResultsAggregator(const TColumnChainInfo& column, const EAggregate aggrType)
        : ColumnInfo(column)
        , AggregationType(aggrType) {
    }
};

}   // namespace

std::shared_ptr<IResourcesAggregator> TAggregateFunction::BuildResultsAggregator(const TColumnChainInfo& output) const {
    AFL_VERIFY(!GetFunctionOptions());
    return std::make_shared<TResultsAggregator>(output, TAggregationsHelper::GetSecondaryAggregationId(AggregationType));
}

}   // namespace NKikimr::NArrow::NSSA::NAggregation
