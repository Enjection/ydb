#include "yql_yt_spec.h"

#include <yt/yql/providers/yt/common/yql_configuration.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>
#include <yql/essentials/utils/log/log.h>

#include <yt/cpp/mapreduce/interface/config.h>
#include <yt/cpp/mapreduce/common/helpers.h>

#include <library/cpp/yson/writer.h>
#include <library/cpp/digest/md5/md5.h>

#include <util/stream/str.h>
#include <util/system/env.h>
#include <util/system/execpath.h>
#include <util/generic/size_literals.h>


namespace NYql {

namespace NNative {

namespace {

ui64 GetCombiningDataSizePerJob(ui64 dataSizePerJob, TMaybe<ui64> minChunkSize) {
    static const ui64 DefaultCombineChunkSize = 1_GB;
    ui64 result = dataSizePerJob;
    if (!minChunkSize.Defined() || *minChunkSize == 0) {
        result = Max(result, DefaultCombineChunkSize);
    } else {
        result = Max(result, *minChunkSize);
    }
    return result;
}

const TString& GetPersistentExecPathMd5()
{
    static TString md5 = MD5::File(GetPersistentExecPath());
    return md5;
}

void MergeAnnotations(
    const NYT::TNode::TMapType& attrs,
    const TString& attribute,
    NYT::TNode& annotations)
{
    if (auto attrAnnotations = attrs.FindPtr(attribute)) {
        if (!attrAnnotations->IsMap()) {
            throw yexception() << "Operation attribute " << attribute.Quote() << " should be a map";
        }
        for (const auto& [k, v] : attrAnnotations->AsMap()) {
            auto it = annotations.AsMap().find(k);
            if (it == annotations.AsMap().end()) {
                annotations[k] = v;
            }
        }
    }
}

}

TMaybe<TString> GetPool(
    const TExecContextBase& execCtx,
    const TYtSettings::TConstPtr& settings)
{
    TMaybe<TString> pool;

    if (auto val = settings->Pool.Get(execCtx.Cluster_)) {
        pool = *val;
    }
    else if (auto val = settings->StaticPool.Get(execCtx.Cluster_)) {
        pool = *val;
    }
    else if (settings->Auth.Get().GetOrElse(TString()).empty()) {
        pool = execCtx.Session_->UserName_;
    }

    return pool;
}

void FillSpec(NYT::TNode& spec,
    const TExecContextBase& execCtx,
    const TYtSettings::TConstPtr& settings,
    const TTransactionCache::TEntry::TPtr& entry,
    double extraCpu,
    const TMaybe<double>& secondExtraCpu,
    EYtOpProps opProps,
    const TSet<TString>& addSecTags)
{
    auto& cluster = execCtx.Cluster_;

    if (auto val = settings->OperationSpec.Get(cluster)) {
        NYT::TNode tmpSpec = *val;
        NYT::MergeNodes(tmpSpec, spec);
        spec = std::move(tmpSpec);
    }

    auto& sampling = execCtx.Sampling;
    auto maxRowWeight = settings->MaxRowWeight.Get(cluster);
    auto maxKeyWeight = settings->MaxKeyWeight.Get(cluster);
    auto bufferRowCount = settings->BufferRowCount.Get(cluster);

    if (maxRowWeight || maxKeyWeight || bufferRowCount || (sampling && opProps.HasFlags(EYtOpProp::AllowSampling))) {
        NYT::TNode jobIO;
        if (maxRowWeight) {
            jobIO["table_writer"]["max_row_weight"] = static_cast<i64>(*maxRowWeight);
        }
        if (maxKeyWeight) {
            jobIO["table_writer"]["max_key_weight"] = static_cast<i64>(*maxKeyWeight);
        }
        if (bufferRowCount) {
            jobIO["buffer_row_count"] = static_cast<i64>(*bufferRowCount);
        }
        if (!jobIO.IsUndefined() && opProps.HasFlags(EYtOpProp::IntermediateData)) {
            // Both Sort and MapReduce
            spec["sort_job_io"] = jobIO;
            if (opProps.HasFlags(EYtOpProp::WithUserJobs)) {
                // MapReduce
                spec["reduce_job_io"] = jobIO;
            }
            else {
                // Sort
                spec["partition_job_io"] = jobIO;
                spec["merge_job_io"] = jobIO;
            }
        }

        // Set sampling only for input jobs
        if (sampling && opProps.HasFlags(EYtOpProp::AllowSampling)) {
            if (sampling->Mode == EYtSampleMode::System) {
                NYT::TNode systemSamplingParams = NYT::TNode::CreateMap();
                systemSamplingParams["sampling_rate"] = sampling->Percentage / 100.;
                if (auto blockSize = settings->SamplingIoBlockSize.Get(cluster)) {
                    systemSamplingParams["io_block_size"] = static_cast<i64>(*blockSize);
                }
                spec["sampling"] = systemSamplingParams;
            } else if (sampling->Mode == EYtSampleMode::Bernoulli) {
                jobIO["table_reader"]["sampling_rate"] = sampling->Percentage / 100.;
                if (sampling->Repeat) {
                    jobIO["table_reader"]["sampling_seed"] = static_cast<i64>(sampling->Repeat);
                }
            }
        }
        if (!jobIO.IsUndefined()) {
            if (!opProps.HasFlags(EYtOpProp::IntermediateData)) {
                // Merge, Map, Reduce
                spec["job_io"] = jobIO;
            } else if (opProps.HasFlags(EYtOpProp::WithUserJobs)) {
                // MapReduce
                spec["map_job_io"] = jobIO;
            }
        }
    }

    if (opProps.HasFlags(EYtOpProp::IntermediateData)) {
        const auto intermediateMedium = settings->IntermediateDataMedium.Get(cluster);
        if (intermediateMedium) {
            spec["intermediate_data_medium"] = *intermediateMedium;
        }
    }

    ui64 dataSizePerJob = settings->DataSizePerJob.Get(cluster).GetOrElse(0);
    if (opProps.HasFlags(EYtOpProp::PublishedChunkCombine)) {
        spec["enable_job_splitting"] = false;
        dataSizePerJob = GetCombiningDataSizePerJob(dataSizePerJob, settings->MinPublishedAvgChunkSize.Get());
    } else if (opProps.HasFlags(EYtOpProp::TemporaryChunkCombine)) {
        spec["enable_job_splitting"] = false;
        dataSizePerJob = GetCombiningDataSizePerJob(dataSizePerJob, settings->MinTempAvgChunkSize.Get());
    }

    if (opProps.HasFlags(EYtOpProp::IntermediateData) && opProps.HasFlags(EYtOpProp::WithUserJobs)) { // MapReduce
        ui64 dataSizePerMapJob = dataSizePerJob;
        ui64 dataSizePerPartition = dataSizePerJob;
        if (auto val = settings->DataSizePerMapJob.Get(cluster).GetOrElse(0)) {
            dataSizePerMapJob = val;
        }

        if (auto val = settings->DataSizePerPartition.Get(cluster).GetOrElse(0)) {
            dataSizePerPartition = val;
        }

        if (dataSizePerMapJob) {
            if (extraCpu != 0.) {
                dataSizePerMapJob /= extraCpu;
            }
            spec["data_size_per_map_job"] = static_cast<i64>(Max<ui64>(dataSizePerMapJob, 1));
        }
        if (dataSizePerPartition) {
            auto secondExtraCpuVal = secondExtraCpu.GetOrElse(extraCpu);
            if (secondExtraCpuVal != 0) {
                dataSizePerPartition /= secondExtraCpuVal;
            }
            spec["partition_data_size"] = static_cast<i64>(Max<ui64>(dataSizePerPartition, 1));
        }

        if (auto val = settings->DataSizePerSortJob.Get(cluster)) {
            spec["data_size_per_sort_job"] = static_cast<i64>(*val);
        }

    } else if (!opProps.HasFlags(EYtOpProp::IntermediateData)) { // Exclude Sort
        if (dataSizePerJob) {
            if (extraCpu != 0.) {
                dataSizePerJob /= extraCpu;
            }
            spec["data_size_per_job"] = static_cast<i64>(Max<ui64>(dataSizePerJob, 1));
        }
    }

    NYT::TNode annotations;
    if (auto val = settings->Annotations.Get(cluster)) {
        annotations = NYT::TNode::CreateMap(val.Get()->AsMap());
    } else {
        annotations = NYT::TNode::CreateMap();
    }

    // merge annotations from attributes
    if (auto attrs = execCtx.Session_->OperationOptions_.AttrsYson.GetOrElse(TString())) {
        NYT::TNode attributes = NYT::NodeFromYsonString(attrs);
        MergeAnnotations(attributes.AsMap(), "yt_annotations", annotations);
        MergeAnnotations(attributes.AsMap(), "nirvana_yt_annotations", annotations);
    }

    if (!annotations.Empty()) {
        spec["annotations"] = std::move(annotations);
    }

    if (auto val = settings->StartedBy.Get(cluster)) {
        spec["started_by"] = *val;
    }

    if (auto val = settings->Description.Get(cluster)) {
        spec["description"] = *val;
    }

    if (!opProps.HasFlags(EYtOpProp::IntermediateData)) {
        if (auto val = settings->MaxJobCount.Get(cluster)) {
            spec["max_job_count"] = static_cast<i64>(*val);
        }
    }

    if (auto val = settings->UserSlots.Get(cluster)) {
        spec["resource_limits"]["user_slots"] = static_cast<i64>(*val);
    }

    if (auto pool = GetPool(execCtx, settings)) {
        spec["pool"] = *pool;
    }

    if (auto val = settings->SchedulingTag.Get(cluster)) {
        spec["scheduling_tag"] = *val;
    }

    if (auto val = settings->SchedulingTagFilter.Get(cluster)) {
        spec["scheduling_tag_filter"] = *val;
    }

    if (auto val = settings->PoolTrees.Get(cluster)) {
        NYT::TNode trees = NYT::TNode::CreateList();
        for (auto& tree : *val) {
            trees.AsList().push_back(tree);
        }
        spec["pool_trees"] = trees;
    }

    if (auto val = settings->TentativePoolTrees.Get(cluster)) {
        NYT::TNode trees = NYT::TNode::CreateList();
        NYT::TNode tree_eligibility = NYT::TNode::CreateMap();

        for (auto& tree : *val) {
            trees.AsList().push_back(tree);
        }

        if (auto v = settings->TentativeTreeEligibilitySampleJobCount.Get(cluster)) {
            tree_eligibility["sample_job_count"] = *v;
        }

        if (auto v = settings->TentativeTreeEligibilityMaxJobDurationRatio.Get(cluster)) {
            tree_eligibility["max_tentative_job_duration_ratio"] = *v;
        }

        if (auto v = settings->TentativeTreeEligibilityMinJobDuration.Get(cluster)) {
            tree_eligibility["min_job_duration"] = *v;
        }

        spec["tentative_pool_trees"] = trees;
        spec["tentative_tree_eligibility"] = tree_eligibility;
    }

    if (auto val = settings->UseDefaultTentativePoolTrees.Get(cluster)) {
        spec["use_default_tentative_pool_trees"] = *val;
    }

    if (auto val = settings->DefaultOperationWeight.Get(cluster)) {
        spec["weight"] = *val;
    }

    if (auto val = settings->DefaultMapSelectivityFactor.Get(cluster)) {
        spec["map_selectivity_factor"] = *val;
    }

    NYT::TNode aclList;
    TSet<TString> ownersSet = settings->Owners.Get(cluster).GetOrElse(TSet<TString>());
    if (!ownersSet.empty()) {
        NYT::TNode owners = NYT::TNode::CreateList();
        for (auto& o : ownersSet) {
            owners.Add(o);
        }

        NYT::TNode acl = NYT::TNode::CreateMap();
        acl["subjects"] = owners;
        acl["action"] = "allow";
        acl["permissions"] = NYT::TNode::CreateList().Add("read").Add("manage");

        aclList.Add(std::move(acl));
    }
    if (auto val = settings->OperationReaders.Get(cluster)) {
        NYT::TNode readers;
        for (auto& o : *val) {
            if (!ownersSet.contains(o)) {
                readers.Add(o);
            }
        }
        if (!readers.IsUndefined()) {
            NYT::TNode acl = NYT::TNode::CreateMap();
            acl["subjects"] = readers;
            acl["action"] = "allow";
            acl["permissions"] = NYT::TNode::CreateList().Add("read");

            aclList.Add(std::move(acl));
        }
    }
    if (!aclList.IsUndefined()) {
        spec["acl"] = std::move(aclList);
    }

    if (opProps.HasFlags(EYtOpProp::IntermediateData)) {
        if (auto val = settings->IntermediateAccount.Get(cluster)) {
            spec["intermediate_data_account"] = *val;
        }
        else if (auto tmpFolder = GetTablesTmpFolder(*settings, cluster)) {
            auto attrs = entry->Tx->Get(tmpFolder + "/@", NYT::TGetOptions().AttributeFilter(NYT::TAttributeFilter().AddAttribute(TString("account"))));
            if (attrs.HasKey("account")) {
                spec["intermediate_data_account"] = attrs["account"];
            }
        }

        // YT merges this ACL with operation ACL
        // By passing empty list, we allow only user+owners accessing the intermediate data
        // (note: missing "intermediate_data_acl" actually implies "everyone=read")
        spec["intermediate_data_acl"] = NYT::TNode::CreateList();

        if (auto val = settings->IntermediateReplicationFactor.Get(cluster)) {
            spec["intermediate_data_replication_factor"] = static_cast<i64>(*val);
        }

    }

    if (opProps.HasFlags(EYtOpProp::TemporaryAutoMerge)) {
        if (auto val = settings->TemporaryAutoMerge.Get(cluster)) {
            spec["auto_merge"]["mode"] = *val;
        }
    }

    if (opProps.HasFlags(EYtOpProp::PublishedAutoMerge)) {
        if (auto val = settings->PublishedAutoMerge.Get(cluster)) {
            spec["auto_merge"]["mode"] = *val;
        }
    }

    if (settings->UseTmpfs.Get(cluster).GetOrElse(false)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["tmpfs_path"] = TString("_yql_tmpfs");
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["tmpfs_path"] = TString("_yql_tmpfs");
        }
    }
    if (GetEnv(TString("YQL_DETERMINISTIC_MODE"))) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["environment"]["YQL_DETERMINISTIC_MODE"] = TString("1");
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["environment"]["YQL_DETERMINISTIC_MODE"] = TString("1");
        }
    }
    if (auto envMap = settings->JobEnv.Get(cluster)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            for (auto& p: envMap->AsMap()) {
                spec["mapper"]["environment"][p.first] = p.second;
            }
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            for (auto& p: envMap->AsMap()) {
                spec["reducer"]["environment"][p.first] = p.second;
            }
        }
    }

    if (settings->EnforceJobUtc.Get(cluster).GetOrElse(false)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["environment"]["TZ"] = TString("UTC0");
        }

        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["environment"]["TZ"] = TString("UTC0");
        }
    }

    auto probabily = TString(std::to_string(settings->_EnforceRegexpProbabilityFail.Get().GetOrElse(0)));
    if (opProps.HasFlags(EYtOpProp::WithMapper)) {
        spec["mapper"]["environment"]["YQL_RE2_REGEXP_PROBABILITY_FAIL"] = probabily;
    }

    if (opProps.HasFlags(EYtOpProp::WithReducer)) {
        spec["reducer"]["environment"]["YQL_RE2_REGEXP_PROBABILITY_FAIL"] = probabily;
    }

    if (settings->SuspendIfAccountLimitExceeded.Get(cluster).GetOrElse(false)) {
        spec["suspend_operation_if_account_limit_exceeded"] = true;
    }

    if (settings->DisableJobSplitting.Get(cluster).GetOrElse(false)) {
        spec["enable_job_splitting"] = false;
    }

    if (auto val = settings->DefaultMemoryReserveFactor.Get(cluster)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["memory_reserve_factor"] = *val;
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["memory_reserve_factor"] = *val;
        }
    }

    if (auto val = settings->DefaultMemoryDigestLowerBound.Get(cluster)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["user_job_memory_digest_lower_bound"] = *val;
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["user_job_memory_digest_lower_bound"] = *val;
        }
    }

    if (auto val = settings->DefaultLocalityTimeout.Get(cluster)) {
        spec["locality_timeout"] = static_cast<i64>((*val).Seconds());
    }

    if (auto val = settings->MapLocalityTimeout.Get(cluster)) {
        spec["map_locality_timeout"] = static_cast<i64>((*val).Seconds());
    }

    if (auto val = settings->ReduceLocalityTimeout.Get(cluster)) {
        spec["reduce_locality_timeout"] = static_cast<i64>((*val).Seconds());
    }

    if (auto val = settings->SortLocalityTimeout.Get(cluster)) {
        spec["sort_locality_timeout"] = static_cast<i64>((*val).Seconds());
    }

    if (auto val = settings->MinLocalityInputDataWeight.Get(cluster)) {
        spec["min_locality_input_data_weight"] = static_cast<i64>(*val);
    }

    if (auto val = settings->UseColumnarStatistics.Get(cluster)) {
        bool flag = true;
        switch (*val) {
        case EUseColumnarStatisticsMode::Force:
            break;
        case EUseColumnarStatisticsMode::Disable:
            flag = false;
            break;
        case EUseColumnarStatisticsMode::Auto:
            if (AnyOf(execCtx.InputTables_, [](const auto& input) { return input.Lookup; })) {
                flag = false;
            }
            break;
        }
        spec["input_table_columnar_statistics"]["enabled"] = flag;
    }

    if (opProps.HasFlags(EYtOpProp::WithUserJobs)) {
        spec["user_file_columnar_statistics"]["enabled"] = settings->TableContentColumnarStatistics.Get(cluster).GetOrElse(true);
    }

    if (auto val = settings->LayerPaths.Get(cluster)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            NYT::TNode& layersNode = spec["mapper"]["layer_paths"];
            for (auto& path: *val) {
                layersNode.Add(NYT::AddPathPrefix(path, NYT::TConfig::Get()->Prefix));
            }
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            NYT::TNode& layersNode = spec["reducer"]["layer_paths"];
            for (auto& path: *val) {
                layersNode.Add(NYT::AddPathPrefix(path, NYT::TConfig::Get()->Prefix));
            }
        }
    }

    if (auto val = settings->DockerImage.Get(cluster)) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["docker_image"] = *val;
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["docker_image"] = *val;
        }
    }

    if (auto val = settings->MaxSpeculativeJobCountPerTask.Get(cluster)) {
        spec["max_speculative_job_count_per_task"] = i64(*val);
    }

    if (auto val = settings->NetworkProject.Get(cluster).OrElse(settings->StaticNetworkProject.Get(cluster))) {
        if (opProps.HasFlags(EYtOpProp::WithMapper)) {
            spec["mapper"]["network_project"] = *val;
        }
        if (opProps.HasFlags(EYtOpProp::WithReducer)) {
            spec["reducer"]["network_project"] = *val;
        }
    }
    if (!opProps.HasFlags(EYtOpProp::IntermediateData)) {
        if (auto val = settings->ForceJobSizeAdjuster.Get(cluster)) {
            spec["force_job_size_adjuster"] = *val;
        }
    }

    if (opProps.HasFlags(EYtOpProp::WithMapper)) {
        spec["mapper"]["environment"]["TMPDIR"] = ".";
    }

    if (opProps.HasFlags(EYtOpProp::WithReducer)) {
        spec["reducer"]["environment"]["TMPDIR"] = ".";
    }

    if (!addSecTags.empty()) {
        auto secTagsNode = NYT::TNode::CreateList();
        for (const auto& tag : addSecTags) {
            secTagsNode.Add(tag);
        }
        spec["additional_security_tags"] = std::move(secTagsNode);
    }
}

void CheckSpecForSecretsImpl(
    const NYT::TNode& spec,
    const ISecretMasker::TPtr& secretMasker,
    const TYtSettings::TConstPtr& settings
) {
    if (!settings->_ForbidSensitiveDataInOperationSpec.Get().GetOrElse(DEFAULT_FORBID_SENSITIVE_DATA_IN_OPERATION_SPEC)) {
        return;
    }

    YQL_ENSURE(secretMasker);

    // Secure vault is guaranteed not to be exposed by YT
    auto cleanSpec = spec.AsMap();
    cleanSpec.erase("secure_vault");
    auto maskedSpecStr = NYT::NodeToYsonString(cleanSpec);

    auto secrets = secretMasker->Mask(maskedSpecStr);
    if (!secrets.empty()) {
        auto maskedSpecStrBuf = TStringBuf(maskedSpecStr);

        TVector<TString> maskedSecrets;
        for (auto& secret : secrets) {
            maskedSecrets.push_back(TStringBuilder() << "\"" << maskedSpecStrBuf.substr(secret.From, secret.Len) << "\"");
        }

        YQL_LOG_CTX_THROW TErrorException(TIssuesIds::YT_OP_SPEC_CONTAINS_SECRETS)
            << "YT operation spec contains sensitive data (masked): "
            << JoinSeq(", ", maskedSecrets);
    }
}

void FillSecureVault(NYT::TNode& spec, const IYtGateway::TSecureParams& secureParams) {
    if (secureParams.empty()) {
        return;
    }
    TStringStream out;
    NYson::TYsonWriter writer(&out, NYson::EYsonFormat::Text);
    writer.OnBeginMap();
    for (const auto& it : secureParams) {
        writer.OnKeyedItem(it.first);
        writer.OnStringScalar(it.second);
    }
    writer.OnEndMap();
    spec["secure_vault"]["secure_params"] = out.Str();
}

void FillUserJobSpecImpl(NYT::TUserJobSpec& spec,
    const TExecContextBase& execCtx,
    const TYtSettings::TConstPtr& settings,
    const TExpressionResorceUsage& extraUsage,
    ui64 fileMemUsage,
    ui64 llvmMemUsage,
    bool localRun,
    const TString& cmdPrefix)
{
    auto cluster = execCtx.Cluster_;
    auto mrJobBin = execCtx.Config_->GetMrJobBin();
    TMaybe<TString> mrJobBinMd5;
    if (!mrJobBin.empty()) {
        if (execCtx.Config_->HasMrJobBinMd5()) {
            mrJobBinMd5 = execCtx.Config_->GetMrJobBinMd5();
        } else {
            YQL_CLOG(WARN, ProviderYt) << "MrJobBin without MD5";
        }
    }

    TVector<std::pair<TString, TString>> mrJobSystemLibs;
    if (execCtx.Config_->MrJobSystemLibsWithMd5Size() > 0) {
        mrJobSystemLibs.reserve(execCtx.Config_->MrJobSystemLibsWithMd5Size());

        for (const auto& systemLib : execCtx.Config_->GetMrJobSystemLibsWithMd5()) {
            mrJobSystemLibs.push_back({systemLib.GetFile(), systemLib.GetMd5()});

            const auto libSize = TFileStat(systemLib.GetFile()).Size;
            YQL_ENSURE(libSize != 0);
            fileMemUsage += libSize;
        }

        spec.AddEnvironment("LD_LIBRARY_PATH", ".");
    }

    if (settings->UseDefaultArrowAllocatorInJobs.Get().GetOrElse(false)) {
        spec.AddEnvironment("YQL_USE_DEFAULT_ARROW_ALLOCATOR", "1");
    }

    if (!localRun) {
        for (size_t i = 0; i < mrJobSystemLibs.size(); i++) {
            if (!mrJobSystemLibs[i].second) {
                if (GetEnv("YQL_LOCAL") == "1") {
                    // do not calculate heavy md5 in local mode (YQL-15353)
                    mrJobSystemLibs[i].second = MD5::Calc(mrJobSystemLibs[i].first);
                } else {
                    mrJobSystemLibs[i].second = MD5::File(mrJobSystemLibs[i].first);
                }
            }
        }

        if (mrJobBin.empty()) {
            mrJobBinMd5 = GetPersistentExecPathMd5();
        } else if (!mrJobBinMd5) {
            if (GetEnv("YQL_LOCAL") == "1") {
                // do not calculate heavy md5 in local mode (YQL-15353)
                mrJobBinMd5 = MD5::Calc(mrJobBin);
            } else {
                mrJobBinMd5 = MD5::File(mrJobBin);
            }
        }
    }

    const TString binTmpFolder = settings->BinaryTmpFolder.Get(cluster).GetOrElse(TString());
    const TString binCacheFolder = settings->_BinaryCacheFolder.Get(cluster).GetOrElse(TString());
    if (!localRun && (binTmpFolder || binCacheFolder)) {
        TString bin = mrJobBin.empty() ? GetPersistentExecPath() : mrJobBin;
        const auto binSize = TFileStat(bin).Size;
        YQL_ENSURE(binSize != 0);
        fileMemUsage += binSize;
        TTransactionCache::TEntry::TPtr entry = execCtx.GetOrCreateEntry(settings);
        bool useBinCache = false;
        if (binCacheFolder) {
            if (auto snapshot = entry->GetBinarySnapshotFromCache(binCacheFolder, *mrJobBinMd5, "mrjob")) {
                spec.JobBinaryCypressPath(snapshot->first, snapshot->second);
                useBinCache = true;
            }
        }
        if (!useBinCache) {
            if (binTmpFolder) {
                const TDuration binExpiration = settings->BinaryExpirationInterval.Get().GetOrElse(TDuration());
                auto mrJobSnapshot = entry->GetBinarySnapshot(binTmpFolder, *mrJobBinMd5, bin, binExpiration);
                spec.JobBinaryCypressPath(mrJobSnapshot.first, mrJobSnapshot.second);
            } else if (!mrJobBin.empty()) {
                spec.JobBinaryLocalPath(mrJobBin, mrJobBinMd5);
            }
        }

        for (size_t i = 0; i < mrJobSystemLibs.size(); i++) {
            bool useBinCache = false;
            if (binCacheFolder) {
                if (auto snapshot = entry->GetBinarySnapshotFromCache(binCacheFolder, mrJobSystemLibs[i].second, mrJobSystemLibs[i].first)) {
                    spec.AddFile(snapshot->first);
                    useBinCache = true;
                }
            }
            if (!useBinCache) {
                if (binTmpFolder) {
                    const TDuration binExpiration = settings->BinaryExpirationInterval.Get().GetOrElse(TDuration());
                    auto libSnapshot = entry->GetBinarySnapshot(binTmpFolder, mrJobSystemLibs[i].second, mrJobSystemLibs[i].first, binExpiration);
                    spec.AddFile(libSnapshot.first);
                } else {
                    NYT::TAddLocalFileOptions opts;
                    opts.MD5CheckSum(mrJobSystemLibs[i].second);
                    spec.AddLocalFile(mrJobSystemLibs[i].first, opts);
                }
            }
        }

    }
    else if (!mrJobBin.empty()) {
        const auto binSize = TFileStat(mrJobBin).Size;
        YQL_ENSURE(binSize != 0);
        spec.JobBinaryLocalPath(mrJobBin, mrJobBinMd5);
        for (auto file : mrJobSystemLibs) {
            NYT::TAddLocalFileOptions opts;
            opts.MD5CheckSum(file.second);
            spec.AddLocalFile(file.first, opts);
        }
        fileMemUsage += binSize;
    }

    auto defaultMemoryLimit = settings->DefaultMemoryLimit.Get(cluster).GetOrElse(0);
    ui64 tmpFsSize = settings->UseTmpfs.Get(cluster).GetOrElse(false)
        ? (ui64)settings->ExtraTmpfsSize.Get(cluster).GetOrElse(8_MB)
        : ui64(0);

    if (defaultMemoryLimit || fileMemUsage || llvmMemUsage || extraUsage.Memory || tmpFsSize) {
        const ui64 memIoBuffers = YQL_JOB_CODEC_MEM * (static_cast<size_t>(!execCtx.InputTables_.empty()) + execCtx.OutTables_.size());
        const ui64 arrowMemoryPoolReserve = (execCtx.BlockStatus != TOperationProgress::EOpBlockStatus::None ? YQL_ARROW_MEMORY_POOL_RESERVE : 0);
        const ui64 finalMemLimit = Max<ui64>(
            defaultMemoryLimit,
            128_MB + fileMemUsage + extraUsage.Memory + tmpFsSize + memIoBuffers + arrowMemoryPoolReserve,
            llvmMemUsage + memIoBuffers // LLVM consumes memory only once on job start, but after IO initialization
        );
        YQL_CLOG(DEBUG, ProviderYt) << "Job memory limit: " << finalMemLimit
            << " (from options: " << defaultMemoryLimit
            << ", files: " << fileMemUsage
            << ", llvm: " << llvmMemUsage
            << ", extra: " << extraUsage.Memory
            << ", extra tmpfs: " << tmpFsSize
            << ", I/O buffers: " << memIoBuffers
            << ", Arrow pool reserve: " << arrowMemoryPoolReserve
            << ")";
        spec.MemoryLimit(static_cast<i64>(finalMemLimit));
    }

    if (cmdPrefix) {
        spec.JobCommandPrefix(cmdPrefix);
    }
}

void FillOperationOptionsImpl(NYT::TOperationOptions& opOpts,
    const TYtSettings::TConstPtr& settings,
    const TTransactionCache::TEntry::TPtr& entry)
{
    opOpts.UseTableFormats(true);
    opOpts.CreateOutputTables(false);
    if (TString tmpFolder = settings->TmpFolder.Get(entry->Cluster).GetOrElse(TString())) {
        opOpts.FileStorage(tmpFolder);

        if (!entry->CacheTxId.IsEmpty()) {
            opOpts.FileStorageTransactionId(entry->CacheTxId);

            // We need to switch to random-path-upload cache mode because of
            // specified 'FileStorageTransactionId' (see https://st.yandex-team.ru/YT-8462).
            opOpts.FileCacheMode(NYT::TOperationOptions::EFileCacheMode::CachelessRandomPathUpload);
        }
    }
    if (auto ttl = settings->FileCacheTtl.Get().GetOrElse(TDuration::Days(7))) {
        opOpts.FileExpirationTimeout(ttl);
    }
}

} // NNative

} // NYql
