/*
 * Copyright (c) 2017,2019-2022 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2011-2014 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/slicc_interface/AbstractController.hh"

#include <algorithm>
#include <iterator>

#include "RubySlicc_Util.hh"
// MYSTUFF
#include "debug/IndirectAccess.hh"
#include "debug/MSDebug.hh"
// FFUTSYM
#include "debug/ProtocolTrace.hh"
#include "debug/RubyQueue.hh"
// MYSTUFF
#include "mem/request.hh"
// FFUTSYM
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/protocol/MemoryMsg.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/system/Sequencer.hh"
#include "sim/system.hh"

namespace gem5
{

namespace ruby
{

AbstractController::AbstractController(const Params &p)
    : ClockedObject(p), Consumer(this), m_version(p.version),
      m_clusterID(p.cluster_id),
      m_id(p.system->getRequestorId(this)), m_is_blocking(false),
      m_number_of_TBEs(p.number_of_TBEs),
      m_transitions_per_cycle(p.transitions_per_cycle),
      m_buffer_size(p.buffer_size), m_recycle_latency(p.recycle_latency),
      m_mandatory_queue_latency(p.mandatory_queue_latency),
      m_waiting_mem_retry(false),
      m_mem_ctrl_waiting_retry(false),
      memoryPort(csprintf("%s.memory", name()), this),
      addrRanges(p.addr_ranges.begin(), p.addr_ranges.end()),
      mRetryRespEvent{*this, false},
      stats(this)
{
    if (m_version == 0) {
        // Combine the statistics from all controllers
        // of this particular type.
        statistics::registerDumpCallback([this]() { collateStats(); });
    }
    upstreamSequencers = p.upstream_sequencers;
}

void
AbstractController::init()
{
    stats.delayHistogram.init(10);
    stats.idrCoalesceGroupSize.init(8);
    stats.hovRaAccuracy.init(10);
    stats.hovRaDistance.init(16);
    uint32_t size = Network::getNumberOfVirtualNetworks();
    for (uint32_t i = 0; i < size; i++) {
        stats.delayVCHistogram.push_back(new statistics::Histogram(this));
        stats.delayVCHistogram[i]->init(10);
    }

    if (getMemReqQueue()) {
        getMemReqQueue()->setConsumer(this);
    }

    downstreamDestinations.setRubySystem(m_ruby_system);
    upstreamDestinations.setRubySystem(m_ruby_system);

    // Initialize the addr->downstream machine mappings. Multiple machines
    // in downstream_destinations can have the same address range if they have
    // different types. If this is the case, mapAddressToDownstreamMachine
    // needs to specify the machine type
    downstreamDestinations.resize();
    for (auto abs_cntrl : params().downstream_destinations) {
        MachineID mid = abs_cntrl->getMachineID();
        const AddrRangeList &ranges = abs_cntrl->getAddrRanges();
        for (const auto &addr_range : ranges) {
            auto i = downstreamAddrMap.find(mid.getType());
            if ((i != downstreamAddrMap.end()) &&
                (i->second.intersects(addr_range) != i->second.end())) {
                fatal("%s: %s mapped to multiple machines of the same type\n",
                    name(), addr_range.to_string());
            }
            downstreamAddrMap[mid.getType()].insert(addr_range, mid);
        }
        downstreamDestinations.add(mid);
    }
    // Initialize the addr->upstream machine list.
    // We do not need to map address -> upstream machine,
    // so we don't examine the address ranges
    upstreamDestinations.resize();
    for (auto abs_cntrl : params().upstream_destinations) {
        upstreamDestinations.add(abs_cntrl->getMachineID());
    }
}

void
AbstractController::resetStats()
{
    stats.delayHistogram.reset();
    uint32_t size = Network::getNumberOfVirtualNetworks();
    for (uint32_t i = 0; i < size; i++) {
        stats.delayVCHistogram[i]->reset();
    }
    ClockedObject::resetStats();
}

void
AbstractController::regStats()
{
    ClockedObject::regStats();
}

void
AbstractController::profileMsgDelay(uint32_t virtualNetwork, Cycles delay)
{
    assert(virtualNetwork < stats.delayVCHistogram.size());
    stats.delayHistogram.sample(delay);
    stats.delayVCHistogram[virtualNetwork]->sample(delay);
}

void
AbstractController::stallBuffer(MessageBuffer* buf, Addr addr)
{
    if (m_waiting_buffers.count(addr) == 0) {
        MsgVecType* msgVec = new MsgVecType;
        msgVec->resize(m_in_ports, NULL);
        m_waiting_buffers[addr] = msgVec;
    }
    DPRINTF(RubyQueue, "stalling %s port %d addr %#x\n", buf, m_cur_in_port,
            addr);
    assert(m_in_ports > m_cur_in_port);
    (*(m_waiting_buffers[addr]))[m_cur_in_port] = buf;
}

void
AbstractController::wakeUpBuffer(MessageBuffer* buf, Addr addr)
{
    auto iter = m_waiting_buffers.find(addr);
    if (iter != m_waiting_buffers.end()) {
        bool has_other_msgs = false;
        MsgVecType* msgVec = iter->second;
        for (unsigned int port = 0; port < msgVec->size(); ++port) {
            if ((*msgVec)[port] == buf) {
                buf->reanalyzeMessages(addr, clockEdge());
                (*msgVec)[port] = NULL;
            } else if ((*msgVec)[port] != NULL) {
                has_other_msgs = true;
            }
        }
        if (!has_other_msgs) {
            delete msgVec;
            m_waiting_buffers.erase(iter);
        }
    }
}

void
AbstractController::wakeUpBuffers(Addr addr)
{
    if (m_waiting_buffers.count(addr) > 0) {
        //
        // Wake up all possible lower rank (i.e. lower priority) buffers that could
        // be waiting on this message.
        //
        for (int in_port_rank = m_cur_in_port - 1;
             in_port_rank >= 0;
             in_port_rank--) {
            if ((*(m_waiting_buffers[addr]))[in_port_rank] != NULL) {
                (*(m_waiting_buffers[addr]))[in_port_rank]->
                    reanalyzeMessages(addr, clockEdge());
            }
        }
        delete m_waiting_buffers[addr];
        m_waiting_buffers.erase(addr);
    }
}

void
AbstractController::wakeUpAllBuffers(Addr addr)
{
    if (m_waiting_buffers.count(addr) > 0) {
        //
        // Wake up all possible buffers that could be waiting on this message.
        //
        for (int in_port_rank = m_in_ports - 1;
             in_port_rank >= 0;
             in_port_rank--) {
            if ((*(m_waiting_buffers[addr]))[in_port_rank] != NULL) {
                (*(m_waiting_buffers[addr]))[in_port_rank]->
                    reanalyzeMessages(addr, clockEdge());
            }
        }
        delete m_waiting_buffers[addr];
        m_waiting_buffers.erase(addr);
    }
}

void
AbstractController::wakeUpAllBuffers()
{
    //
    // Wake up all possible buffers that could be waiting on any message.
    //

    std::vector<MsgVecType*> wokeUpMsgVecs;
    MsgBufType wokeUpMsgBufs;

    if (m_waiting_buffers.size() > 0) {
        for (WaitingBufType::iterator buf_iter = m_waiting_buffers.begin();
             buf_iter != m_waiting_buffers.end();
             ++buf_iter) {
             for (MsgVecType::iterator vec_iter = buf_iter->second->begin();
                  vec_iter != buf_iter->second->end();
                  ++vec_iter) {
                  //
                  // Make sure the MessageBuffer has not already be reanalyzed
                  //
                  if (*vec_iter != NULL &&
                      (wokeUpMsgBufs.count(*vec_iter) == 0)) {
                      (*vec_iter)->reanalyzeAllMessages(clockEdge());
                      wokeUpMsgBufs.insert(*vec_iter);
                  }
             }
             wokeUpMsgVecs.push_back(buf_iter->second);
        }

        for (std::vector<MsgVecType*>::iterator wb_iter = wokeUpMsgVecs.begin();
             wb_iter != wokeUpMsgVecs.end();
             ++wb_iter) {
             delete (*wb_iter);
        }

        m_waiting_buffers.clear();
    }
}

bool
AbstractController::serviceMemoryQueue()
{
    auto mem_queue = getMemReqQueue();
    assert(mem_queue);
    if (m_waiting_mem_retry || !mem_queue->isReady(clockEdge())) {
        return false;
    }

    const MemoryMsg *mem_msg = (const MemoryMsg*)mem_queue->peek();
    unsigned int req_size = m_ruby_system->getBlockSizeBytes();
    if (mem_msg->m_Len > 0) {
        req_size = mem_msg->m_Len;
    }

    RequestPtr req
        = std::make_shared<Request>(mem_msg->m_addr, req_size, 0, m_id);
    PacketPtr pkt;
    if (mem_msg->getType() == MemoryRequestType_MEMORY_WB) {
        pkt = Packet::createWrite(req);
        pkt->allocate();
        pkt->setData(mem_msg->m_DataBlk.getData(getOffset(mem_msg->m_addr),
            req_size));
    } else if (mem_msg->getType() == MemoryRequestType_MEMORY_READ) {
        pkt = Packet::createRead(req);
        uint8_t *newData = new uint8_t[req_size];
        pkt->dataDynamic(newData);
    } else {
        panic("Unknown memory request type (%s) for addr %p",
              MemoryRequestType_to_string(mem_msg->getType()),
              mem_msg->m_addr);
    }

    SenderState *s = new SenderState(mem_msg->m_Sender);
    pkt->pushSenderState(s);

    if (m_ruby_system->getWarmupEnabled()) {
        // Use functional rather than timing accesses during warmup
        mem_queue->dequeue(clockEdge());
        memoryPort.sendFunctional(pkt);
        // Since the queue was popped the controller may be able
        // to make more progress. Make sure it wakes up
        scheduleEvent(Cycles(1));
        recvTimingResp(pkt);
    } else if (memoryPort.sendTimingReq(pkt)) {
        mem_queue->dequeue(clockEdge());
        // Since the queue was popped the controller may be able
        // to make more progress. Make sure it wakes up
        scheduleEvent(Cycles(1));
    } else {
        scheduleEvent(Cycles(1));
        m_waiting_mem_retry = true;
        delete pkt;
        delete s;
    }

    return true;
}

void
AbstractController::blockOnQueue(Addr addr, MessageBuffer* port)
{
    m_is_blocking = true;
    m_block_map[addr] = port;
}

bool
AbstractController::isBlocked(Addr addr) const
{
    return m_is_blocking && (m_block_map.find(addr) != m_block_map.end());
}

void
AbstractController::unblock(Addr addr)
{
    m_block_map.erase(addr);
    if (m_block_map.size() == 0) {
       m_is_blocking = false;
    }
}

bool
AbstractController::isBlocked(Addr addr)
{
    return (m_block_map.count(addr) > 0);
}

Port &
AbstractController::getPort(const std::string &if_name, PortID idx)
{
    return memoryPort;
}

void
AbstractController::functionalMemoryRead(PacketPtr pkt)
{
    // read from mem. req. queue if write data is pending there
    MessageBuffer *req_queue = getMemReqQueue();
    if (!req_queue || !req_queue->functionalRead(pkt))
        memoryPort.sendFunctional(pkt);
}

int
AbstractController::functionalMemoryWrite(PacketPtr pkt)
{
    int num_functional_writes = 0;

    // Update memory itself.
    memoryPort.sendFunctional(pkt);
    return num_functional_writes + 1;
}

bool
AbstractController::recvTimingResp(PacketPtr pkt)
{
    auto* memRspQueue = getMemRespQueue();
    gem5_assert(memRspQueue);
    gem5_assert(pkt->isResponse());

    if (!memRspQueue->areNSlotsAvailable(1, curTick())) {
        m_mem_ctrl_waiting_retry = true;
        return false;
    }

    int blk_size = m_ruby_system->getBlockSizeBytes();

    std::shared_ptr<MemoryMsg> msg =
        std::make_shared<MemoryMsg>(clockEdge(), blk_size, m_ruby_system);
    (*msg).m_addr = pkt->getAddr();
    (*msg).m_Sender = m_machineID;

    SenderState *s = dynamic_cast<SenderState *>(pkt->senderState);
    (*msg).m_OriginalRequestorMachId = s->id;
    delete s;

    if (pkt->isRead()) {
        (*msg).m_Type = MemoryRequestType_MEMORY_READ;
        (*msg).m_MessageSize = MessageSizeType_Response_Data;

        // Copy data from the packet
        (*msg).m_DataBlk.setData(pkt->getPtr<uint8_t>(), 0,
                                 m_ruby_system->getBlockSizeBytes());
    } else if (pkt->isWrite()) {
        (*msg).m_Type = MemoryRequestType_MEMORY_WB;
        (*msg).m_MessageSize = MessageSizeType_Writeback_Control;
    } else {
        panic("Incorrect packet type received from memory controller!");
    }

    memRspQueue->enqueue(msg, clockEdge(), cyclesToTicks(Cycles(1)),
        m_ruby_system->getRandomization(), m_ruby_system->getWarmupEnabled());
    delete pkt;
    return true;
}

Tick
AbstractController::recvAtomic(PacketPtr pkt)
{
   return ticksToCycles(memoryPort.sendAtomic(pkt));
}

MachineID
AbstractController::mapAddressToMachine(Addr addr, MachineType mtype) const
{
    NodeID node = m_net_ptr->addressToNodeID(addr, mtype);
    MachineID mach = {mtype, node};
    return mach;
}

bool
AbstractController::mappedToMachine(Addr addr, MachineID mid)
{
    return m_net_ptr->mappedToMachine(addr, mid);
}

MachineID
AbstractController::mapAddressToDownstreamMachine(Addr addr, MachineType mtype)
const
{
    if (mtype == MachineType_NUM) {
        // map to the first match
        for (const auto &i : downstreamAddrMap) {
            const auto mapping = i.second.contains(addr);
            if (mapping != i.second.end())
                return mapping->second;
        }
    }
    else {
        const auto i = downstreamAddrMap.find(mtype);
        if (i != downstreamAddrMap.end()) {
            const auto mapping = i->second.contains(addr);
            if (mapping != i->second.end())
                return mapping->second;
        }
    }
    fatal("%s: couldn't find mapping for address %x mtype=%s\n",
        name(), addr, mtype);
}


void
AbstractController::memRespQueueDequeued() {
    if (m_mem_ctrl_waiting_retry && !mRetryRespEvent.scheduled()) {
        schedule(mRetryRespEvent, clockEdge(Cycles{1}));
    }
}

void
AbstractController::dequeueMemRespQueue() {
    auto* q = getMemRespQueue();
    gem5_assert(q);
    q->dequeue(clockEdge());
    memRespQueueDequeued();
}

void
AbstractController::sendRetryRespToMem() {
    if (m_mem_ctrl_waiting_retry) {
        m_mem_ctrl_waiting_retry = false;
        memoryPort.sendRetryResp();
    }
}

Addr
AbstractController::getOffset(Addr addr) const
{
    return ruby::getOffset(addr, m_ruby_system->getBlockSizeBits());
}

Addr
AbstractController::makeLineAddress(Addr addr) const
{
    return ruby::makeLineAddress(addr, m_ruby_system->getBlockSizeBits());
}

std::string
AbstractController::printAddress(Addr addr) const
{
    return ruby::printAddress(addr, m_ruby_system->getBlockSizeBits());
}

NetDest
AbstractController::broadcast(MachineType type)
{
    assert(m_ruby_system != nullptr);
    NodeID type_count = m_ruby_system->MachineType_base_count(type);

    NetDest dest(m_ruby_system);
    for (NodeID i = 0; i < type_count; i++) {
        MachineID mach = {type, i};
        dest.add(mach);
    }
    return dest;
}

int
AbstractController::machineCount(MachineType machType)
{
    assert(m_ruby_system != nullptr);
    return m_ruby_system->MachineType_base_count(machType);
}

// MYSTUFF
bool
AbstractController::canOverride(std::string label, Addr address)
{
    if (upstreamSequencers.size() == 0) {
        // If there is only one sequencer, it is a private cache controller
        // and the label cannot be overridden.
        return true;
    } else {
        bool ret = false;
        for (auto sequencer : upstreamSequencers) {
            ret |= sequencer->canOverride(label, address);
        }
        return ret;
    }
}

uint64_t
AbstractController::indexValue(const DependentAccessGen &dag,
                               DataBlock &index_data, int &offset,
                               size_t &index_size)
{
    assert(!dag.isMerged());
    offset = getOffset(dag.descriptor(0).indexAddr);
    index_size = dag.indexSize();
    panic_if(index_size == 0, "%s: DAG has no indexSize set.", __func__);

    const uint8_t *raw = index_data.getData(offset, index_size);
    switch (index_size) {
        case 4: return *reinterpret_cast<const uint32_t *>(raw);
        case 8: return *reinterpret_cast<const uint64_t *>(raw);
        default:
            panic("%s: unsupported index element size %u bytes", __func__,
                  index_size);
    }
}

void
AbstractController::markIndexRead(const DependentAccessGen &dag,
                                  DataBlock &index_data, Addr line_addr)
{
    // The Gather Unit reads the index element as a core's load would: mark
    // its bytes used in the block the IDR is built from (the TBE's, merged
    // into the cache entry at Finalize_UpdateCacheFromTBE), as the
    // Sequencer's hit callback does for core accesses. Without it the
    // index lines at the Gather Unit show no use at all.
    if (index_data.isAlloc()) {
        index_data.setReadUsefulness(getOffset(dag.descriptor(0).indexAddr),
                                     dag.indexSize(), name(), line_addr);
    }
}

RequestPtr
AbstractController::getDependentReq(Addr line_addr, DataBlock &index_data)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "getDependentReq: no cached entries for line_addr %#x. "
             "Ensure extractExtensions() was called when the TBE was "
             "allocated.", line_addr);

    // Pop the front entry (a single-descriptor DAG).
    auto entry = std::move(it->second.entries.front());
    it->second.entries.erase(it->second.entries.begin());
    auto dag = std::move(entry.dag);

    panic_if(!dag, "getDependentReq: cached DependentAccessGen is null for "
            "line_addr %#x.", line_addr);

    const auto& desc = dag->descriptor(0);
    // IssueIDR sends this entry now: a run-ahead IDR counts for its pair.
    if (entry.spec) {
        hovRaIssue(dag->requestorId(), desc.pair);
    }
    // The IDR's intent: a write, or the DAG's own when nothing coalesced.
    bool read = !entry.write;
    int offset;
    size_t index_size;
    DPRINTF(MSDebug, "%s: Generating dependent request from %s.\n",
            __func__, index_data);
    uint64_t index_value = indexValue(*dag, index_data, offset, index_size);
    DPRINTF(MSDebug, "%s: line_addr=%#x offset=%d index_size=%d "
            "index_value=%" PRIu64 " read=%d absorbed=%u\n", __func__,
            line_addr, offset, index_size, index_value, read,
            entry.absorbed);

    // Generate the Phase 2 (data-fetch) request using the cached DAG.
    RequestPtr phase2 = dag->genNextRequest(index_value);

    // Attach DepAccessType so .sm files can determine ReadValue vs WriteValue.
    auto dat = std::make_shared<DepAccessType>(read);
    phase2->setExtension<DepAccessType>(dat);

    // Each descriptor carries the alias of the index access it came from
    // (a merged DAG mixes several); the IDR uses that alias.
    phase2->setExtension<IndirectAccessAlias>(
        std::make_shared<IndirectAccessAlias>(desc.alias));

    // The IDR is an access to the pair's data array: name it as uop1 names
    // its own (the array's registered name, else hov<p>_data), so the
    // caches' per-name stats add them up.
    phase2->setExtension<MemAccessName>(std::make_shared<MemAccessName>(
        desc.dataName.empty() ? csprintf("hov%u_data", desc.pair)
                              : desc.dataName));

    DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %#x %s idx_line=%#x "
             "offset=%d idx_val=%" PRIu64 "\n",
             curTick(), m_version, "GatherU", "IDR_Begin", "", "",
             phase2->getPaddr(),
             read ? "ReadValue" : "WriteValue",
             line_addr, offset, index_value);

    return phase2;
}

void
AbstractController::extractExtensions(Addr line_addr, RequestPtr req,
                                      DataBlock &index_data)
{
    auto dag = req->getExtension<DependentAccessGen>();
    assert(dag && "extractExtensions called without a DependentAccessGen");

    // Unwrap the DAG into individual single-descriptor DAGs.
    // This works for both single and merged DAGs (single returns
    // a vector of 1). Each unwrapped DAG is independent of the
    // original Request's lifetime — no clone needed.
    auto unwrapped = dag->unwrap();
    for (auto& single : unwrapped) {
        auto shared = std::shared_ptr<DependentAccessGen>(single.release());

        bool write = shared->descriptor(0).intent ==
                     DependentAccessGen::AccessIntent::Write;

        // The core's position on the pair's index, covered or not.
        hovRaDemand(*shared);

        // A speculated IDR already stands for this element: drop the DAG,
        // but the Gather Unit read its index element for it.
        int elem = getOffset(shared->descriptor(0).indexAddr) /
                   shared->indexSize();
        if (hovCovers(line_addr, shared->requestorId(),
                      shared->descriptor(0).pair, elem, write)) {
            markIndexRead(*shared, index_data, line_addr);
            stats.hovDagsCoveredAtGU++;
            DPRINTF(IndirectAccess, "DAG for index line %#x element %d "
                    "(pair %u, write %d) covered by speculation.\n",
                    line_addr, elem, shared->descriptor(0).pair, write);
            continue;
        }

        DPRINTF(MSDebug, "%s: Cached DAG for line_addr %#x "
                "(read=%d, offset=%d, dag=%p)\n",
                __func__, line_addr, !write,
                getOffset(shared->descriptor(0).indexAddr),
                shared.get());

        auto &line = m_cachedEntries[line_addr];
        line.entries.push_back({std::move(shared), write});
        // New entries may coalesce with the pending ones.
        line.coalesced = false;
    }
}

void
AbstractController::coalesceExtensions(Addr line_addr, DataBlock &index_data)
{
    auto it = m_cachedEntries.find(line_addr);
    if (it == m_cachedEntries.end() || it->second.coalesced) {
        return;
    }
    auto &entries = it->second.entries;
    std::vector<DependentEntry> kept;
    std::vector<Addr> kept_lines;
    for (auto &entry : entries) {
        int offset;
        size_t index_size;
        uint64_t index_value =
            indexValue(*entry.dag, index_data, offset, index_size);
        Addr data_line = makeLineAddress(entry.dag->genAddress(index_value));
        auto pos = std::find(kept_lines.begin(), kept_lines.end(), data_line);
        if (pos == kept_lines.end()) {
            kept.push_back(std::move(entry));
            kept_lines.push_back(data_line);
            continue;
        }
        // Absorbed: the kept IDR fetches its data line (a write makes that
        // IDR a WriteValue); the Gather Unit still read its index element.
        auto &survivor = kept[pos - kept_lines.begin()];
        survivor.write |= entry.write;
        survivor.absorbed++;
        // A speculated element is read only when a core asks for it.
        if (entry.spec) {
            stats.hovSpecCoalesced++;
        } else {
            markIndexRead(*entry.dag, index_data, line_addr);
        }
        stats.idrCoalescedSameLine++;
        DPRINTF(IndirectAccess, "DAG for data line %#x (offset %d, write %d) "
                "coalesced into the IDR of index line %#x.\n", data_line,
                offset, entry.write, line_addr);
    }
    for (const auto &entry : kept) {
        if (entry.absorbed > 0) {
            stats.idrCoalesceGroupSize.sample(entry.absorbed + 1);
        }
    }
    entries = std::move(kept);
    it->second.coalesced = true;
}

bool
AbstractController::hasDependentWork(Addr line_addr)
{
    auto it = m_cachedEntries.find(line_addr);
    return it != m_cachedEntries.end() && !it->second.entries.empty();
}

Addr
AbstractController::peekDependentAddr(Addr line_addr, DataBlock &index_data)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "peekDependentAddr: no cached entries for line_addr %#x.",
             line_addr);

    const auto &front = it->second.entries.front();  // peek, don't pop
    const auto& dag = front.dag;
    panic_if(!dag, "peekDependentAddr: cached DAG is null for "
             "line_addr %#x.", line_addr);

    int offset;
    size_t index_size;
    uint64_t index_value = indexValue(*dag, index_data, offset, index_size);
    // A speculated element is read only when a core asks for it (its DAG
    // is then dropped as covered and marks it).
    if (!front.spec) {
        markIndexRead(*dag, index_data, line_addr);
    }
    return dag->genAddress(index_value);
}

bool
AbstractController::peekDependentIsSpec(Addr line_addr)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "peekDependentIsSpec: no cached entries for line_addr %#x.",
             line_addr);
    return it->second.entries.front().spec;
}

void
AbstractController::countSpecIdr(int what)
{
    if (what == 0) {
        stats.hovSpecIdrsSent++;
    } else {
        stats.hovSpecIdrsDropped[what - 1]++;
    }
}

void
AbstractController::hovRaConfigure(int spec_mode, int streams, int conf,
                                   int seq_window, int epoch, int acc_low)
{
    m_hovRa = HovRaParams{spec_mode, streams, conf, seq_window, epoch,
                          acc_low};
}

bool
AbstractController::peekDependentDroppable(Addr line_addr)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "peekDependentDroppable: no cached entries for line_addr %#x.",
             line_addr);
    const auto &front = it->second.entries.front();
    return front.spec && front.absorbed == 0;
}

void
AbstractController::dropRunAheadReq(Addr line_addr)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "dropRunAheadReq: no cached entries for line_addr %#x.",
             line_addr);
    auto &entries = it->second.entries;
    const auto &front = entries.front();
    panic_if(!front.spec || front.absorbed != 0,
             "dropRunAheadReq: the next entry of line_addr %#x is not a "
             "run-ahead one.", line_addr);
    const auto &d = front.dag->descriptor(0);
    RequestorID rid = front.dag->requestorId();
    int elem = getOffset(d.indexAddr) / front.dag->indexSize();
    // Uncover the element, so a real DAG for it is sent rather than
    // dropped as covered by an IDR that never left.
    auto cover = m_hovCover.find(line_addr);
    if (cover != m_hovCover.end() && elem < 64) {
        uint64_t bit = uint64_t(1) << elem;
        for (auto &row : cover->second) {
            if (row.rid == rid && row.pair == d.pair) {
                row.covered &= ~bit;
                row.writes &= ~bit;
                row.raBits &= ~bit;
            }
        }
    }
    stats.hovSpecIdrsDropped[2]++;
    entries.erase(entries.begin());
}

AbstractController::HovStream *
AbstractController::findStream(RequestorID rid, unsigned pair)
{
    for (auto &row : m_hovStreams) {
        if (row.rid == rid && row.pair == pair) {
            return &row;
        }
    }
    return nullptr;
}

void
AbstractController::hovRaDemand(const DependentAccessGen &dag)
{
    if (m_hovRa.specMode <= 0) {
        return;
    }
    const auto &d = dag.descriptor(0);
    if (d.pair < hovPairs) {
        stats.hovRaDemands[d.pair]++;
    }
    if (m_hovRa.streams <= 0) {
        return;
    }
    RequestorID rid = dag.requestorId();
    Addr q = d.indexAddr;
    Cycles now = curCycle();
    HovStream *row = findStream(rid, d.pair);
    if (!row) {
        // A new walk: the least recently used row gives way when full.
        if ((int)m_hovStreams.size() < m_hovRa.streams) {
            m_hovStreams.emplace_back();
            row = &m_hovStreams.back();
        } else {
            row = &*std::min_element(m_hovStreams.begin(),
                                     m_hovStreams.end(),
                                     [](const HovStream &a,
                                        const HovStream &b) {
                                         return a.lastUse < b.lastUse;
                                     });
            stats.hovRaRowsReplaced++;
        }
        *row = HovStream();
        row->rid = rid;
        row->pair = d.pair;
        row->demand = q;
        row->frontier = q;
        row->lastUse = now;
        stats.hovRaRowsAllocated++;
        return;
    }
    row->lastUse = now;
    Addr span = (Addr)m_hovRa.seqWindow * dag.indexSize();
    if (q > row->demand && q - row->demand <= span) {
        // A forward step: the core walks the index.
        if (row->conf < 3) {
            row->conf++;
        }
        row->demand = q;
    } else if (q <= row->demand && row->demand - q <= span) {
        // A DAG of an element just behind: a merged uop0's notification
        // arrives after the next line's index request. Nothing changes.
    } else {
        row->conf = 0;
        row->demand = q;
        row->frontier = q;
        stats.hovRaRebases++;
    }
    if (row->offFor > 0) {
        row->offFor--;
    }
}

bool
AbstractController::hovRaGate(RequestorID rid, unsigned pair)
{
    if (m_hovRa.streams <= 0) {
        return true;
    }
    HovStream *row = findStream(rid, pair);
    return row && row->conf >= m_hovRa.conf && row->offFor == 0;
}

void
AbstractController::hovRaIssue(RequestorID rid, unsigned pair)
{
    if (pair < hovPairs) {
        stats.hovRaIssued[pair]++;
    }
    HovStream *row = findStream(rid, pair);
    if (!row) {
        return;
    }
    row->issued++;
    if (m_hovRa.epoch <= 0 || row->issued < (uint32_t)m_hovRa.epoch) {
        return;
    }
    // The epoch ends: below hov_ra_acc_low the row backs off for
    // `backoff` demands and the next back-off doubles (at most 4096);
    // otherwise the back-off returns to 256.
    uint32_t accuracy = row->hits * 100 / row->issued;
    stats.hovRaAccuracy.sample(accuracy);
    if (m_hovRa.accLow > 0 && accuracy < (uint32_t)m_hovRa.accLow) {
        row->offFor = row->backoff;
        row->backoff = std::min<uint32_t>(2 * row->backoff, 4096);
        if (pair < hovPairs) {
            stats.hovRaThrottled[pair]++;
        }
    } else {
        row->backoff = 256;
    }
    row->issued = 0;
    row->hits = 0;
}

AbstractController::HovCover *
AbstractController::findCover(Addr line_addr, RequestorID rid,
                              unsigned pair, Cycles now)
{
    auto it = m_hovCover.find(line_addr);
    if (it == m_hovCover.end()) {
        return nullptr;
    }
    for (auto &row : it->second) {
        if (row.rid == rid && row.pair == pair) {
            return now - row.made < m_hovSpecWindow ? &row : nullptr;
        }
    }
    return nullptr;
}

AbstractController::HovCover &
AbstractController::makeCover(Addr line_addr, RequestorID rid,
                              unsigned pair, int leader_elem, Cycles now)
{
    auto it = m_hovCover.find(line_addr);
    if (it == m_hovCover.end()) {
        // A new line: the oldest lines give up their rows first.
        while (m_hovSpecLines > 0 &&
               (int)m_hovCoverFifo.size() >= m_hovSpecLines) {
            auto old = m_hovCover.find(m_hovCoverFifo.front());
            if (old != m_hovCover.end()) {
                stats.hovCoverReplaced += old->second.size();
                m_hovCover.erase(old);
            }
            m_hovCoverFifo.pop_front();
        }
        m_hovCoverFifo.push_back(line_addr);
        it = m_hovCover.emplace(line_addr, std::vector<HovCover>()).first;
    }
    for (auto &row : it->second) {
        if (row.rid == rid && row.pair == pair) {
            // A stale row of the same pair starts over.
            row = HovCover{rid, static_cast<uint8_t>(pair), leader_elem,
                           0, 0, now};
            return row;
        }
    }
    it->second.push_back(HovCover{rid, static_cast<uint8_t>(pair),
                                  leader_elem, 0, 0, now});
    return it->second.back();
}

bool
AbstractController::hovCovers(Addr line_addr, RequestorID rid,
                              unsigned pair, int elem, bool write)
{
    if (elem < 0 || elem >= 64) {
        return false;
    }
    HovCover *row = findCover(line_addr, rid, pair, curCycle());
    if (!row) {
        return false;
    }
    uint64_t bit = uint64_t(1) << elem;
    bool covers = (row->covered & bit) && (!write || (row->writes & bit));
    // A real DAG covered by a run-ahead IDR: a hit for the pair, counted
    // once per run-ahead IDR.
    if (covers && (row->raBits & bit)) {
        row->raBits &= ~bit;
        if (pair < hovPairs) {
            stats.hovRaHits[pair]++;
        }
        if (HovStream *stream = findStream(rid, pair)) {
            stream->hits++;
        }
    }
    return covers;
}

void
AbstractController::speculateLine(Addr line_addr, DataBlock &index_data,
                                  int mode, bool spec_writes, int max_lines,
                                  Cycles window)
{
    m_hovSpecLines = max_lines;
    m_hovSpecWindow = window;
    if (mode == 0) {
        return;
    }
    auto it = m_cachedEntries.find(line_addr);
    if (it == m_cachedEntries.end() || it->second.entries.empty()) {
        return;
    }
    Cycles now = curCycle();
    int block_size = m_ruby_system->getBlockSizeBytes();
    auto &entries = it->second.entries;
    std::vector<DependentEntry> made;
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].spec) {
            continue;
        }
        const auto &leader = *entries[i].dag;
        const auto &ld = leader.descriptor(0);
        RequestorID rid = leader.requestorId();
        unsigned pair = ld.pair;
        if (findCover(line_addr, rid, pair, now)) {
            continue;  // this pair is already speculated on this line
        }
        // The stream row's gate (hov_ra_streams): a pair that does not
        // walk its index forward, or whose run-ahead went unused, makes
        // neither entries nor a cover row; its next real DAG on the line
        // is a leader again.
        if (!hovRaGate(rid, pair)) {
            if (pair < hovPairs) {
                stats.hovRaGated[pair]++;
            }
            continue;
        }
        int index_size = leader.indexSize();
        int elems = std::min(block_size / index_size, 64);
        int k_leader = getOffset(ld.indexAddr) / index_size;

        // The real entries of this pair hold their own elements.
        HovCover &row = makeCover(line_addr, rid, pair, k_leader, now);
        for (const auto &other : entries) {
            const auto &od = other.dag->descriptor(0);
            if (other.spec || other.dag->requestorId() != rid ||
                od.pair != pair) {
                continue;
            }
            int k = getOffset(od.indexAddr) / index_size;
            if (k < 64) {
                row.covered |= uint64_t(1) << k;
                if (other.write) {
                    row.writes |= uint64_t(1) << k;
                }
            }
        }

        bool write = ld.intent == DependentAccessGen::AccessIntent::Write;
        if (write && !spec_writes) {
            write = false;
        }
        int first = mode == 1 ? k_leader + 1 : 0;
        for (int k = first; k < elems; k++) {
            uint64_t bit = uint64_t(1) << k;
            if (row.covered & bit) {
                continue;
            }
            auto clones = leader.unwrap();
            std::shared_ptr<DependentAccessGen> spec(clones.front().release());
            auto &sd = spec->mutableDescriptor(0);
            sd.indexAddr = line_addr + k * index_size;
            sd.alias = ld.alias + (Addr)((int64_t)(k - k_leader) *
                                         (int64_t)ld.dataSize);
            if (ld.intent == DependentAccessGen::AccessIntent::Write &&
                !write) {
                sd.intent = DependentAccessGen::AccessIntent::Read;
                stats.hovSpecWritesDemoted++;
            }
            made.push_back({std::move(spec), write, 0, true});
            row.covered |= bit;
            row.raBits |= bit;
            if (write) {
                row.writes |= bit;
            }
            stats.hovSpecEntries++;
            if (k > k_leader) {
                stats.hovRaDistance.sample(k - k_leader);
            }
            if (HovStream *stream = findStream(rid, pair)) {
                stream->frontier = std::max(stream->frontier,
                                            line_addr + k * index_size);
            }
        }
        if (mode == 1) {
            stats.hovSpecLinesForward++;
        } else {
            stats.hovSpecLinesLine++;
        }
        DPRINTF(IndirectAccess, "Speculated index line %#x from element %d "
                "(pair %u, rid %u): covered %#lx.\n", line_addr, k_leader,
                pair, rid, row.covered);
    }
    if (!made.empty()) {
        // After the real entries, so real IDRs go first.
        for (auto &entry : made) {
            entries.push_back(std::move(entry));
        }
        it->second.coalesced = false;
    }
}

bool
AbstractController::peekDependentIsWrite(Addr line_addr)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "peekDependentIsWrite: no cached entries for line_addr %#x.",
             line_addr);
    return it->second.entries.front().write;
}

void
AbstractController::dropDependentReq(Addr line_addr)
{
    auto it = m_cachedEntries.find(line_addr);
    panic_if(it == m_cachedEntries.end() || it->second.entries.empty(),
             "dropDependentReq: no cached entries for line_addr %#x.",
             line_addr);

    DPRINTF(MSDebug, "%s: Dropping front DAG entry for line_addr %#x "
            "(collision skip)\n", __func__, line_addr);
    it->second.entries.erase(it->second.entries.begin());
}

int
AbstractController::idrVerdict(Addr data_line, bool write)
{
    auto in_flight = m_idrInFlight.find(data_line);
    if (in_flight != m_idrInFlight.end()) {
        if (write && !in_flight->second.write) {
            if (in_flight->second.parked) {
                in_flight->second.write = true;
                stats.idrUpgradedInRetry++;
                return IDR_UPGRADE;
            }
            stats.idrIntentLost++;
        }
        stats.idrDroppedInFlight++;
        return IDR_DROP;
    }
    auto recent = m_idrRecent.find(data_line);
    if (recent != m_idrRecent.end()) {
        if (write && !recent->second) {
            stats.idrIntentLost++;
        }
        stats.idrDroppedRecent++;
        return IDR_DROP;
    }
    return IDR_SEND;
}

Addr
AbstractController::parkedIdrHovId(Addr data_line)
{
    auto it = m_idrInFlight.find(data_line);
    panic_if(it == m_idrInFlight.end() || !it->second.parked,
             "parkedIdrHovId: no parked IDR to data line %#x.", data_line);
    return it->second.hovId;
}

void
AbstractController::noteIdrSent(Addr data_line, Addr hov_id, bool write,
                                int recent_lines)
{
    auto inserted = m_idrInFlight.emplace(data_line,
                                          IdrInFlight{hov_id, write});
    panic_if(!inserted.second, "noteIdrSent: an IDR to data line %#x is "
             "already in flight.", data_line);
    if (recent_lines <= 0) {
        return;
    }
    if (m_idrRecent.emplace(data_line, write).second) {
        m_idrRecentFifo.push_back(data_line);
        while (m_idrRecentFifo.size() > (size_t)recent_lines) {
            m_idrRecent.erase(m_idrRecentFifo.front());
            m_idrRecentFifo.pop_front();
        }
    } else {
        m_idrRecent[data_line] |= write;
    }
}

void
AbstractController::noteIdrAcked(Addr data_line)
{
    panic_if(m_idrInFlight.erase(data_line) == 0,
             "noteIdrAcked: no IDR in flight to data line %#x.", data_line);
}

void
AbstractController::noteIdrParked(Addr data_line)
{
    auto it = m_idrInFlight.find(data_line);
    panic_if(it == m_idrInFlight.end(),
             "noteIdrParked: no IDR in flight to data line %#x.", data_line);
    it->second.parked = true;
}

void
AbstractController::noteIdrResent(Addr data_line)
{
    auto it = m_idrInFlight.find(data_line);
    panic_if(it == m_idrInFlight.end() || !it->second.parked,
             "noteIdrResent: no parked IDR to data line %#x.", data_line);
    it->second.parked = false;
}

void
AbstractController::clearExtensions(Addr line_addr)
{
    DPRINTF(MSDebug, "%s: Clearing cached entries for line_addr %#x\n", __func__, line_addr);
    m_cachedEntries.erase(line_addr);
}

bool
AbstractController::isMappedDownstream(Addr addr) const
{
    for (const auto &i : downstreamAddrMap) {
        if (i.second.contains(addr) != i.second.end())
            return true;
    }
    return false;
}

bool
AbstractController::hasDAG(RequestPtr req)
{
    return req && req->getExtension<DependentAccessGen>() != nullptr;
}

void
AbstractController::countDAGCarrier(int carrier, RequestPtr req)
{
    assert(carrier >= 0 && carrier < 3);
    auto dag = req->getExtension<DependentAccessGen>();
    assert(dag);
    stats.hovDAGsByCarrier[carrier] += dag->numDescriptors();
}

void
AbstractController::stashPostEvictDAG(Addr line_addr, RequestPtr req)
{
    auto dag = req->getExtension<DependentAccessGen>();
    assert(dag && "stashPostEvictDAG called without a DependentAccessGen");
    auto &entry = m_postEvicts[line_addr];
    if (entry.dag) {
        // The stash holds its own copy, so merging never changes the
        // request's DAG, which the CPU may still share.
        entry.dag->merge(*dag);
    } else {
        entry.dag = std::shared_ptr<DependentAccessGen>(
            static_cast<DependentAccessGen *>(dag->clone().release()));
    }
    DPRINTF(MSDebug, "%s: stashed %u descriptor(s) for line %#x (%u total)\n",
            __func__, dag->numDescriptors(), line_addr,
            entry.dag->numDescriptors());
}

bool
AbstractController::hasStash(Addr line_addr)
{
    auto it = m_postEvicts.find(line_addr);
    return it != m_postEvicts.end() && it->second.dag != nullptr;
}

RequestPtr
AbstractController::takePostEvictDAG(Addr line_addr)
{
    auto it = m_postEvicts.find(line_addr);
    panic_if(it == m_postEvicts.end() || !it->second.dag,
             "takePostEvictDAG: no stashed DAG for line %#x", line_addr);
    // A fresh line-sized request carries the merged DAG on the
    // write-back, as the Sequencer's merged notification does.
    RequestPtr req = std::make_shared<Request>(
        line_addr, m_ruby_system->getBlockSizeBytes(), 0, m_id);
    req->setExtension<DependentAccessGen>(it->second.dag);
    m_postEvicts.erase(it);
    return req;
}

bool
AbstractController::isReplEnqueued(Addr line_addr)
{
    auto it = m_postEvicts.find(line_addr);
    return it != m_postEvicts.end() && it->second.replEnqueued;
}

void
AbstractController::markReplEnqueued(Addr line_addr)
{
    m_postEvicts[line_addr].replEnqueued = true;
}

void
AbstractController::clearReplEnqueued(Addr line_addr)
{
    auto it = m_postEvicts.find(line_addr);
    if (it == m_postEvicts.end())
        return;
    it->second.replEnqueued = false;
    if (!it->second.dag)
        m_postEvicts.erase(it);
}
// FFUTSYM

bool
AbstractController::MemoryPort::recvTimingResp(PacketPtr pkt)
{
    return controller->recvTimingResp(pkt);
}

void
AbstractController::MemoryPort::recvReqRetry()
{
    controller->m_waiting_mem_retry = false;
    controller->serviceMemoryQueue();
}

AbstractController::MemoryPort::MemoryPort(const std::string &_name,
                                           AbstractController *_controller,
                                           PortID id)
    : RequestPort(_name, id), controller(_controller)
{
}

AbstractController::
ControllerStats::ControllerStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(fullyBusyCycles,
               "cycles for which number of transistions == max transitions"),
      ADD_STAT(delayHistogram, "delay_histogram"),
      ADD_STAT(idrDroppedUnmapped,
               "IDRs dropped because the data address has no downstream "
               "mapping"),
      ADD_STAT(idrCoalescedSameLine,
               "DAGs absorbed into another DAG's IDR to the same data line "
               "(one index line)"),
      ADD_STAT(idrCoalesceGroupSize,
               "DAGs per coalesced IDR (groups of two or more)"),
      ADD_STAT(idrDroppedInFlight,
               "IDRs dropped because one to their data line was in flight"),
      ADD_STAT(idrDroppedRecent,
               "IDRs dropped because one to their data line was sent "
               "recently"),
      ADD_STAT(idrIntentLost,
               "write IDRs dropped over a read IDR to the same data line "
               "(uop1's store upgrades the line)"),
      ADD_STAT(idrUpgradedInRetry,
               "read IDRs parked in retry lifted to WriteValue by a later "
               "write"),
      ADD_STAT(hovStashOrphaned,
               "post-evict stashes turned into notifications because the "
               "line left without a replacement"),
      ADD_STAT(hovDAGsByCarrier,
               "DAG descriptors extracted at the Gather Unit, by the "
               "message that carried them"),
      ADD_STAT(hovSpecEntries,
               "dependent entries speculated from a leader's index line"),
      ADD_STAT(hovSpecLinesForward,
               "leaders speculated forward (their element to the line end)"),
      ADD_STAT(hovSpecLinesLine,
               "leaders speculated over the whole index line"),
      ADD_STAT(hovSpecIdrsSent, "speculated IDRs sent"),
      ADD_STAT(hovSpecIdrsDropped, "speculated IDRs dropped, by reason"),
      ADD_STAT(hovDagsCoveredAtGU,
               "real DAG descriptors dropped at the Gather Unit because a "
               "cover row already held their element"),
      ADD_STAT(hovCoverReplaced,
               "cover rows dropped by the hov_spec_lines limit"),
      ADD_STAT(hovSpecWritesDemoted,
               "speculated entries made reads from a write leader "
               "(hov_spec_writes off)"),
      ADD_STAT(hovSpecCoalesced,
               "speculated entries absorbed into another IDR to the same "
               "data line (one index line)"),
      ADD_STAT(hovSpecIdrsDroppedAtIU,
               "run-ahead IDRs the Indirect Unit dropped with HovAck for "
               "want of a TBE (hov_ra_iu_drop)"),
      ADD_STAT(hovSpecIdrsRetriedAtIU,
               "run-ahead IDRs the Indirect Unit refused with RetryAck"),
      ADD_STAT(hovRaRowsAllocated, "run-ahead stream rows allocated"),
      ADD_STAT(hovRaRowsReplaced,
               "run-ahead stream rows replaced (least recently used)"),
      ADD_STAT(hovRaRebases,
               "real DAGs off their stream row's forward walk (re-based)"),
      ADD_STAT(hovRaDemands,
               "real DAGs seen at the Gather Unit, per pair entry"),
      ADD_STAT(hovRaIssued, "run-ahead IDRs sent, per pair entry"),
      ADD_STAT(hovRaHits,
               "real DAGs a run-ahead IDR covered, per pair entry"),
      ADD_STAT(hovRaGated,
               "leaders the stream rows kept from running ahead, per pair "
               "entry"),
      ADD_STAT(hovRaThrottled,
               "accuracy epochs that backed a stream row off, per pair "
               "entry"),
      ADD_STAT(hovRaAccuracy,
               "run-ahead accuracy per stream row epoch (percent)"),
      ADD_STAT(hovRaDistance,
               "lead of each run-ahead entry made ahead of its leader "
               "(index elements)")
{
    fullyBusyCycles
        .flags(statistics::nozero);
    delayHistogram
        .flags(statistics::nozero);
    idrDroppedUnmapped
        .flags(statistics::nozero);
    idrCoalescedSameLine
        .flags(statistics::nozero);
    idrCoalesceGroupSize
        .flags(statistics::nozero);
    idrDroppedInFlight
        .flags(statistics::nozero);
    idrDroppedRecent
        .flags(statistics::nozero);
    idrIntentLost
        .flags(statistics::nozero);
    idrUpgradedInRetry
        .flags(statistics::nozero);
    hovStashOrphaned
        .flags(statistics::nozero);
    hovDAGsByCarrier
        .init(3)
        .subname(0, "request")
        .subname(1, "notification")
        .subname(2, "postEvict")
        .flags(statistics::nozero);
    hovSpecEntries.flags(statistics::nozero);
    hovSpecLinesForward.flags(statistics::nozero);
    hovSpecLinesLine.flags(statistics::nozero);
    hovSpecIdrsSent.flags(statistics::nozero);
    hovSpecIdrsDropped
        .init(3)
        .subname(0, "unmapped")
        .subname(1, "coalesced")
        .subname(2, "full")
        .flags(statistics::nozero);
    hovDagsCoveredAtGU.flags(statistics::nozero);
    hovCoverReplaced.flags(statistics::nozero);
    hovSpecWritesDemoted.flags(statistics::nozero);
    hovSpecCoalesced.flags(statistics::nozero);
    hovSpecIdrsDroppedAtIU.flags(statistics::nozero);
    hovSpecIdrsRetriedAtIU.flags(statistics::nozero);
    hovRaRowsAllocated.flags(statistics::nozero);
    hovRaRowsReplaced.flags(statistics::nozero);
    hovRaRebases.flags(statistics::nozero);
    for (auto *per_pair : {&hovRaDemands, &hovRaIssued, &hovRaHits,
                           &hovRaGated, &hovRaThrottled}) {
        per_pair->init(hovPairs).flags(statistics::nozero);
    }
    hovRaAccuracy.flags(statistics::nozero);
    hovRaDistance.flags(statistics::nozero);
}

} // namespace ruby
} // namespace gem5
