#pragma once

#include <ydb/library/actors/core/actor.h>
#include <ydb/core/tx/replication/service/table_writer.h>

#include <ydb/core/tx/replication/service/worker.h>
#include <ydb/core/tx/replication/ydb_proxy/ydb_proxy.h>
#include <ydb/core/tx/replication/service/worker.h>
#include <ydb/core/persqueue/write_meta.h>

constexpr static char OFFLOAD_ACTOR_CLIENT_ID[] = "__OFFLOAD_ACTOR__";

#define LOG_T(stream) LOG_TRACE_S (*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)
#define LOG_D(stream) LOG_DEBUG_S (*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)
#define LOG_I(stream) LOG_INFO_S  (*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)
#define LOG_N(stream) LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)
#define LOG_W(stream) LOG_WARN_S  (*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)
#define LOG_E(stream) LOG_ERROR_S (*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)
#define LOG_C(stream) LOG_CRIT_S  (*TlsActivationContext, NKikimrServices::CONTINUOUS_BACKUP, GetLogPrefix() << stream)

using namespace NKikimr::NReplication::NService;
using namespace NKikimr::NReplication;

namespace NKikimr::NPQ {

class TLocalPartitionReader
    : public TActor<TLocalPartitionReader>
{
private:
    const TActorId PQTablet;
    const ui32 Partition;
    mutable TMaybe<TString> LogPrefix;

    TActorId Worker;
    ui64 Offset = 0;
    ui64 SentOffset = 0;

    TStringBuf GetLogPrefix() const {
        if (!LogPrefix) {
            LogPrefix = TStringBuilder()
                << "[LocalPartitionReader]"
                << "[" << PQTablet << "]"
                << "[" << Partition << "]"
                << SelfId() << " ";
        }

        return LogPrefix.GetRef();
    }

    THolder<TEvPersQueue::TEvRequest> CreateGetOffsetRequest() {
        THolder<TEvPersQueue::TEvRequest> request(new TEvPersQueue::TEvRequest);

        auto& req = *request->Record.MutablePartitionRequest();
        req.SetPartition(Partition);
        auto& offset = *req.MutableCmdGetClientOffset();
        offset.SetClientId(OFFLOAD_ACTOR_CLIENT_ID);

        return request;
    }

    void HandleInit(TEvWorker::TEvHandshake::TPtr& ev) {
        Worker = ev->Sender;
        LOG_D("Handshake"
            << ": worker# " << Worker);

        Send(PQTablet, CreateGetOffsetRequest().Release());
    }

    void HandleInit(TEvPersQueue::TEvResponse::TPtr& ev) {
        LOG_D("Handle " << ev->Get()->ToString());
        auto& record = ev->Get()->Record;
        if (record.GetErrorCode() == NPersQueue::NErrorCode::INITIALIZING) {
            // TODO reschedule
            Y_ABORT("uh-oh");
        }
        Y_VERIFY_S(record.GetErrorCode() == NPersQueue::NErrorCode::OK, "uh-oh");
        auto resp = record.GetPartitionResponse().GetCmdGetClientOffsetResult();
        Offset = resp.GetOffset();
        SentOffset = Offset;

        Y_ABORT_UNLESS(Worker);
        Send(Worker, new TEvWorker::TEvHandshake());

        Become(&TLocalPartitionReader::StateWork);
    }

    THolder<TEvPersQueue::TEvRequest> CreateReadRequest() {
        THolder<TEvPersQueue::TEvRequest> request(new TEvPersQueue::TEvRequest);

        auto& req = *request->Record.MutablePartitionRequest();
        req.SetPartition(Partition);
        auto& read = *req.MutableCmdRead();
        read.SetOffset(Offset);
        read.SetClientId(OFFLOAD_ACTOR_CLIENT_ID);
        read.SetTimeoutMs(0);
        read.SetBytes(1_MB);

        return request;
    }

    void Handle(TEvWorker::TEvPoll::TPtr& ev) {
        LOG_D("Handle " << ev->Get()->ToString());

        Offset = SentOffset;
        // TODO: commit offset to PQ

        Y_ABORT_UNLESS(PQTablet);
        Send(PQTablet, CreateReadRequest().Release());
    }

    void Handle(TEvPersQueue::TEvResponse::TPtr& ev) {
        auto& record = ev->Get()->Record;
        LOG_D("Handle " << ev->Get()->ToString());

        const auto& readResult = record.GetPartitionResponse().GetCmdReadResult();

        auto gotOffset = Offset;
        TVector<TEvWorker::TEvData::TRecord> records(::Reserve(readResult.ResultSize()));

        for (auto& result : readResult.GetResult()) {
            gotOffset = std::max(gotOffset, result.GetOffset());
            records.emplace_back(result.GetOffset(), GetDeserializedData(result.GetData()).GetData());
        }
        SentOffset = gotOffset + 1;

        Send(Worker, new TEvWorker::TEvData(ToString(Partition), std::move(records), /*proto = */ true));
    }

    void Leave(TEvWorker::TEvGone::EStatus status) {
        LOG_I("Leave");
        Send(Worker, new TEvWorker::TEvGone(status));
        PassAway();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::BACKUP_LOCAL_PARTITION_READER;
    }

    explicit TLocalPartitionReader(const TActorId& PQTabletMbox, ui32 partition)
        : TActor(&TThis::StateInit)
        , PQTablet(PQTabletMbox)
        , Partition(partition)
    {}

    STATEFN(StateInit) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvWorker::TEvHandshake, HandleInit);
            hFunc(TEvPersQueue::TEvResponse, HandleInit);
        default:
            Y_VERIFY_S(false, "Unhandled event type: " << ev->GetTypeRewrite()
                        << " event: " << ev->ToString());
        }
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvPersQueue::TEvResponse, Handle);
            hFunc(TEvWorker::TEvPoll, Handle);
            sFunc(TEvents::TEvPoison, PassAway);
        default:
            Y_VERIFY_S(false, "Unhandled event type: " << ev->GetTypeRewrite()
                        << " event: " << ev->ToString());
        }
    }
}; // TLocalPartitionReader

class TOffloadActor
    : public TActorBootstrapped<TOffloadActor>
{
private:
    const TActorId ParentTablet;
    const ui32 Partition;
    const NKikimrPQ::TOffloadConfig Config;
    const TPathId DstPathId;

    mutable TMaybe<TString> LogPrefix;
    TActorId Worker;

    TStringBuf GetLogPrefix() const {
        if (!LogPrefix) {
            LogPrefix = TStringBuilder()
                << "[OffloadActor]"
                << "[" << ParentTablet << "]"
                << "[" << Partition << "]"
                << SelfId() << " ";
        }

        return LogPrefix.GetRef();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::BACKUP_PQ_OFFLOAD_ACTOR;
    }

    explicit TOffloadActor(TActorId parentTablet, ui32 partition, const NKikimrPQ::TOffloadConfig& config)
        : ParentTablet(parentTablet)
        , Partition(partition)
        , Config(config)
        , DstPathId(PathIdFromPathId(config.GetIncrementalBackup().GetDstPathId()))
    {}

    void Bootstrap() {
        auto* workerActor = CreateWorker(
            SelfId(),
            [=]() -> IActor* { return new TLocalPartitionReader(ParentTablet, Partition); },
            [=]() -> IActor* { return CreateLocalTableWriter(DstPathId); });
        Worker = TActivationContext::Register(workerActor);

        Become(&TOffloadActor::StateWork);
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
        default:
            Y_VERIFY_S(false, "Unhandled event type: " << ev->GetTypeRewrite()
                        << " event: " << ev->ToString());
        }
    }
};

} // namespace NKikimr::NPQ
