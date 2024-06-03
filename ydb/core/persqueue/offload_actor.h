#pragma once

#include <ydb/library/actors/core/actor.h>

constexpr static char OFFLOAD_ACTOR_CLIENT_ID[] = "__OFFLOAD_ACTOR__";

namespace NKikimr::NPQ {

class TOffloadActor : public TActorBootstrapped<TOffloadActor> {
private:
    TActorId ParentTablet;
    ui32 Partition;
    ui64 Offset = 0;
    TPathId DstPathId;
public:
    TOffloadActor(TActorId parentTablet, ui32 partition)
        : ParentTablet(parentTablet)
        , Partition(partition)
    {}

    void Bootstrap() {
        {
            THolder<TEvPersQueue::TEvRequest> request(new TEvPersQueue::TEvRequest);

            auto& req = *request->Record.MutablePartitionRequest();
            req.SetPartition(Partition);
            auto& offset = *req.MutableCmdGetClientOffset();
            offset.SetClientId(OFFLOAD_ACTOR_CLIENT_ID);

            Send(ParentTablet, request.Release());
        }

        Become(&TOffloadActor::Init);
    }

    STATEFN(Init) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvPersQueue::TEvResponse, HandleInit);
        default:
            Y_VERIFY_S(false, "Unhandled event type: " << ev->GetTypeRewrite()
                        << " event: " << ev->ToString());
        }
    }

    void HandleInit(TEvPersQueue::TEvResponse::TPtr& ev) {
        auto& record = ev->Get()->Record;
        if (record.GetErrorCode() == NPersQueue::NErrorCode::INITIALIZING) {
            // TODO reschedule
            Y_ABORT("uh-oh");
        }
        Y_VERIFY_S(record.GetErrorCode() == NPersQueue::NErrorCode::OK, "uh-oh");
        auto resp = record.GetPartitionResponse().GetCmdGetClientOffsetResult();
        Offset = resp.GetOffset();

        THolder<TEvPersQueue::TEvRequest> request(new TEvPersQueue::TEvRequest);

        auto& req = *request->Record.MutablePartitionRequest();
        req.SetPartition(Partition);
        auto& read = *req.MutableCmdRead();
        read.SetOffset(Offset);
        read.SetClientId(OFFLOAD_ACTOR_CLIENT_ID);
        read.SetTimeoutMs(30000);

        Send(ParentTablet, request.Release());

        Become(&TOffloadActor::Working);
    }

    void HandleWorking(TEvPersQueue::TEvResponse::TPtr& ev) {
        Y_UNUSED(ev);
    }

    STATEFN(Working) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvPersQueue::TEvResponse, HandleWorking);
        default:
            Y_VERIFY_S(false, "Unhandled event type: " << ev->GetTypeRewrite()
                        << " event: " << ev->ToString());
        }
    }
};

} // namespace NKikimr::NPQ
