#include "mem/dx100/ALU.hh"
#include "mem/dx100/IF.hh"
#include "mem/dx100/IndirectAccess.hh"
#include "mem/dx100/Invalidator.hh"
#include "mem/dx100/RangeFuser.hh"
#include "mem/dx100/SPD.hh"
#include "mem/dx100/StreamAccess.hh"
#include "mem/dx100/MAA.hh"

#include "base/addr_range.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "mem/packet.hh"
#include "params/MAA.hh"
#include "debug/MAA.hh"
#include "debug/MAAMemPort.hh"
#include "sim/cur_tick.hh"
#include <cassert>
#include <cstdint>

#ifndef TRACING_ON
#define TRACING_ON 1
#endif

/*
 * DX100 port to CHI: the data port. It replaces the fork's MemSidePort
 * (one request port per DRAM channel into a non-coherent crossbar in front
 * of Ramulator2), its CacheSidePort (one per core into the LLC bus) and
 * the routing half of Port.cc (an express snoop that chose between them).
 * Each data port enters the sequencer of a CHI DMA requester, so every
 * access the Stream and Indirect units make is a coherent CHI transaction
 * that the home node's directory orders against the cores' caches.
 * Port.cc keeps its per-channel and per-core queues; both kinds leave
 * through the data port of the line's DRAM channel.
 */

namespace gem5 {

bool MAA::DataPort::recvTimingResp(PacketPtr pkt) {
    DPRINTF(MAAMemPort, "%s: channel %d received %s\n", __func__, channel_id,
            pkt->print());
    maa.recvTimingResp(pkt);
    delete pkt;
    return true;
}

void MAA::DataPort::recvReqRetry() {
    DPRINTF(MAAMemPort, "%s: channel %d\n", __func__, channel_id);
    panic_if(!blocked, "%s: retry on an unblocked data port\n", name());
    blocked = false;
    maa.dataPortRetry(channel_id);
}

bool MAA::DataPort::sendPacket(PacketPtr pkt) {
    // A refused port takes no new request before its retry.
    if (blocked) {
        DPRINTF(MAAMemPort, "%s: channel %d blocked, holding %s\n", __func__,
                channel_id, pkt->print());
        return false;
    }
    panic_if(!pkt->isRead() && !pkt->isWrite(),
             "%s: %s is neither a read nor a write\n", name(), pkt->print());
    if (!sendTimingReq(pkt)) {
        DPRINTF(MAAMemPort, "%s: channel %d refused %s\n", __func__,
                channel_id, pkt->print());
        blocked = true;
        return false;
    }
    DPRINTF(MAAMemPort, "%s: channel %d sent %s\n", __func__, channel_id,
            pkt->print());
    return true;
}

bool MAA::sendPacketMem(PacketPtr pkt) {
    int pkt_channel_id = channel_addr(pkt->getAddr());
    return dataPorts[pkt_channel_id]->sendPacket(pkt);
}

bool MAA::sendPacketCache(PacketPtr pkt) {
    // The fork's cache path (core_addr picks the core's LLC-bus port)
    // leaves through the same data port as the memory path.
    int pkt_channel_id = channel_addr(pkt->getAddr());
    return dataPorts[pkt_channel_id]->sendPacket(pkt);
}

void MAA::dataPortRetry(int channel_id) {
    // A refused send blocked either the channel's queue or a core's
    // queue (Port.cc); unblock each so it tries again, and any send that
    // meets a still-blocked port blocks its queue anew.
    if (mem_channels_blocked[channel_id]) {
        unblockMemChannel(channel_id);
    }
    for (int core = 0; core < num_cores; core++) {
        if (cache_bus_blocked[core]) {
            unblockCache(core);
        }
    }
}

MAA::DataPort::DataPort(const std::string &_name, MAA &_maa, int _channel_id)
    : RequestPort(_name), maa(_maa), channel_id(_channel_id), blocked(false) {
}
} // namespace gem5
