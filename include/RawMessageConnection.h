#pragma once
#ifndef __RAWMESSAGECONNECTION_H__
#define __RAWMESSAGECONNECTION_H__

#include <thread>

#include "AbstractMessageConnection.h"
#include "GlobalAddress.h"

enum RpcType : uint8_t
{
    MALLOC,
    FREE,
    NEW_ROOT,
    NOP,
};

struct RawMessage
{
    RpcType type;
    uint64_t alloc_size;
    uint16_t alloc_la{};  // alignment to 1ull << la

    uint16_t node_id;
    uint16_t app_id;

    GlobalAddress addr;  // for malloc
    int level;
    char inlined_buffer[32];
} __attribute__((packed));

static_assert(sizeof(RawMessage) + 40 < MESSAGE_SIZE, "failed");

std::ostream &operator<<(std::ostream &, const RawMessage &msg);

class RawMessageConnection : public AbstractMessageConnection
{
public:
    RawMessageConnection(RdmaContext &ctx, ibv_cq *cq, uint32_t messageNR);
    static std::shared_ptr<RawMessageConnection> newInstance(
        RdmaContext &ctx, ibv_cq *cq, uint32_t messageNR);

    void initSend();
    void sendRawMessage(RawMessage *m, uint32_t remoteQPN, ibv_ah *ah);
    void syncSendRawMessage(RawMessage *m, uint32_t remoteQPN, ibv_ah *ah);
    void destroy();
};

#endif /* __RAWMESSAGECONNECTION_H__ */
