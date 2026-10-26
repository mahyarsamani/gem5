/*
 * Copyright (c) 2017,2019-2023 ARM Limited
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
 * Copyright (c) 2009-2014 Mark D. Hill and David A. Wood
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

#ifndef __MEM_RUBY_SLICC_INTERFACE_ABSTRACTCONTROLLER_HH__
#define __MEM_RUBY_SLICC_INTERFACE_ABSTRACTCONTROLLER_HH__

#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/addr_range.hh"
#include "base/addr_range_map.hh"
#include "base/callback.hh"
#include "mem/packet.hh"
#include "mem/qport.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/common/Histogram.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/protocol/AccessPermission.hh"
#include "mem/ruby/system/CacheRecorder.hh"
#include "params/RubyController.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

namespace ruby
{

class Network;
class GPUCoalescer;
class DMASequencer;
class RubySystem;

// used to communicate that an in_port peeked the wrong message type
class RejectException: public std::exception
{
    virtual const char* what() const throw()
    { return "Port rejected message based on type"; }
};

class AbstractController : public ClockedObject, public Consumer
{
  // MYSTUFF
  private:
    std::unordered_map<Addr, Addr> addrToAliasMap;
    std::unordered_map<Addr, Addr> aliasToAddrMap;

    // Per-line cache of DAGs extracted from seqReq on arrival.
    // Keyed by index line address. Supports multiple sub-line entries per
    // line (e.g., index[i] and index[i+1] in the same cache line); each
    // entry is one pending dependent access: its single-descriptor DAG,
    // the IDR's intent (a write absorbs the reads coalesced into it) and
    // how many DAGs it absorbed. `coalesced` says coalesceExtensions ran
    // on the current entries; extractExtensions clears it.
    // `spec`: made by speculateLine, not by a core's uop0.
    struct DependentEntry
    {
        std::shared_ptr<DependentAccessGen> dag;
        bool write;
        unsigned absorbed = 0;
        bool spec = false;
    };
    struct DependentEntries
    {
        std::vector<DependentEntry> entries;
        bool coalesced = false;
    };
    std::unordered_map<Addr, DependentEntries> m_cachedEntries;

    // The IDRs this Gather Unit sent, by data line, for coalescing across
    // index lines (idrVerdict): one in flight per line (a hov TBE, from
    // IssueIDR to HovAck), parked while a RetryAck holds it here (a later
    // write may still lift it to WriteValue), and the last idr_recent_lines
    // sent, kept after HovAck. A dropped IDR costs only the early fill:
    // uop1 is a real access and fetches the line itself.
    struct IdrInFlight
    {
        Addr hovId;
        bool write;
        bool parked = false;
    };
    std::unordered_map<Addr, IdrInFlight> m_idrInFlight;
    std::deque<Addr> m_idrRecentFifo;
    std::unordered_map<Addr, bool> m_idrRecent;  // data line -> write

    // In-line speculation (speculateLine). A cover row says which elements
    // of an index line this Gather Unit has an IDR for, per (core, pair):
    // bit k of `covered` is element k (`writes`: that IDR is a write).
    // A row is a hint: it lives for hov_spec_window cycles from `made`,
    // and at most hov_spec_lines index lines keep rows (FIFO).
    // `raBits`: the covered elements whose IDR the run-ahead made (not a
    // real entry's); a hit clears its bit, so each counts at most once.
    struct HovCover
    {
        RequestorID rid;
        uint8_t pair;
        int leaderElem;
        uint64_t covered = 0;
        uint64_t writes = 0;
        Cycles made;
        uint64_t raBits = 0;
    };
    std::unordered_map<Addr, std::vector<HovCover>> m_hovCover;
    std::deque<Addr> m_hovCoverFifo;
    // The configuration speculateLine last ran with (the SLICC params).
    int m_hovSpecLines = 0;
    Cycles m_hovSpecWindow = Cycles(0);
    // The live row of (rid, pair) on an index line, or null.
    HovCover *findCover(Addr line_addr, RequestorID rid, unsigned pair,
                        Cycles now);
    // A fresh row of (rid, pair) on an index line (reusing a stale one).
    HovCover &makeCover(Addr line_addr, RequestorID rid, unsigned pair,
                        int leader_elem, Cycles now);

    // Run-ahead stream rows (hov_ra_streams of them, replaced least
    // recently used): one per (core, pair), the core's walk of the pair's
    // index and the use of the run-ahead IDRs it caused. A leader runs
    // ahead only while its row is confident (conf >= hov_ra_conf) and not
    // backing off (offFor == 0). The row is the place later look-ahead
    // state attaches to (a distance, late counts and a template DAG for a
    // window across lines; follower links for co-iteration).
    struct HovStream
    {
        RequestorID rid = 0;
        uint8_t pair = 0;
        Addr demand = 0;        // highest index PA of a real DAG
        Addr frontier = 0;      // highest index PA run ahead to
        uint8_t conf = 0;       // forward-walk confidence, 0 to 3
        uint32_t issued = 0;    // run-ahead IDRs sent, this epoch
        uint32_t hits = 0;      // of them, covering a real DAG
        uint32_t offFor = 0;    // demands left in the back-off
        uint32_t backoff = 256; // the next back-off, in demands
        Cycles lastUse = Cycles(0);
    };
    std::vector<HovStream> m_hovStreams;
    // The SLICC parameters of the rows (hovRaConfigure).
    struct HovRaParams
    {
        int specMode = 0;
        int streams = 0;
        int conf = 2;
        int seqWindow = 32;
        int epoch = 64;
        int accLow = 40;
    };
    HovRaParams m_hovRa;
    // Per-pair statistics have one element per side-car pair entry.
    static constexpr unsigned hovPairs = 32;
    // The row of (rid, pair), or null.
    HovStream *findStream(RequestorID rid, unsigned pair);
    // A real DAG reached the Gather Unit: update its pair's row (forward
    // step, late notification, or re-base) and the back-off.
    void hovRaDemand(const DependentAccessGen &dag);
    // Whether a leader of (rid, pair) may run ahead.
    bool hovRaGate(RequestorID rid, unsigned pair);
    // A run-ahead IDR of (rid, pair) is sent: count it, close the epoch.
    void hovRaIssue(RequestorID rid, unsigned pair);

    // The index element a DAG reads, from the index line's data.
    uint64_t indexValue(const DependentAccessGen &dag,
                        DataBlock &index_data, int &offset,
                        size_t &index_size);
    // Mark that element read in the block (the Gather Unit's own use).
    void markIndexRead(const DependentAccessGen &dag, DataBlock &index_data,
                       Addr line_addr);

    // Per-line post-evict obligation of a cache above the Gather Unit that
    // holds an index line: the DAGs that reached it while it held the line
    // (an index hit, an absorbed notification, a propagated write-back),
    // merged into one, and whether the delayed post-evict replacement is
    // already enqueued. Whichever replacement writes the line back next
    // takes the DAG (Initiate_Replacement) and carries it to the Gather
    // Unit with the current data.
    struct PostEvictEntry
    {
        std::shared_ptr<DependentAccessGen> dag;
        bool replEnqueued = false;
    };
    std::unordered_map<Addr, PostEvictEntry> m_postEvicts;

  protected:
    // NOTE: Pointer to all sequencers that are upstream to this controller.
    // The list should be of length 1 for a private cache controller, and
    // of length >= 1 for a shared cache controller.
    std::vector<Sequencer*> upstreamSequencers;

    bool canOverride(std::string label, Addr address);

    std::string getName() { return name(); }

    // Pop the front entry for lineAddr, use DataBlock to extract the
    // index value, and return the generated Phase 2 RequestPtr.
    // Attaches a DepAccessType extension to the returned request.
    RequestPtr getDependentReq(Addr line_addr, DataBlock &index_data);

    // Extract extensions from a Request and append to m_cachedEntries[lineAddr].
    // A DAG whose element a live cover row already covers is dropped
    // (hovDagsCoveredAtGU) and its index element marked read in
    // `index_data` (the speculated IDR stands for it).
    void extractExtensions(Addr line_addr, RequestPtr req,
                           DataBlock &index_data);

    // In-line speculation, with the index line's data here: for the first
    // real DAG of each (core, pair) on the line with no live cover row
    // (the leader), append speculated entries for the line's elements
    // after the leader's (mode 1, forward) or all of them (mode 2, line)
    // that no real entry holds, cloned from the leader's DAG at element k
    // (alias moved by (k - leader) data elements; a write demoted to a
    // read unless spec_writes), and make the row. Mode 0 does nothing.
    void speculateLine(Addr line_addr, DataBlock &index_data, int mode,
                       bool spec_writes, int max_lines, Cycles window);
    // Whether the next dependent request was speculated.
    bool peekDependentIsSpec(Addr line_addr);
    // A speculated IDR: 0 sent, 1 dropped (unmapped), 2 dropped
    // (coalesced across index lines).
    void countSpecIdr(int what);

    // The stream rows' parameters (the SLICC hov_spec_mode and hov_ra_*),
    // given before every extraction of DAGs.
    void hovRaConfigure(int spec_mode, int streams, int conf,
                        int seq_window, int epoch, int acc_low);
    // Whether the next dependent entry may be dropped as run-ahead: made
    // by the run-ahead and standing for no real DAG (none coalesced in).
    bool peekDependentDroppable(Addr line_addr);
    // Drop that entry (the pool is short): clear its element in its
    // pair's cover row, so a real DAG for it is sent, and count it.
    void dropRunAheadReq(Addr line_addr);
    // At the Indirect Unit: a run-ahead IDR dropped with HovAck for want
    // of a TBE, or refused with RetryAck.
    void countSpecIdrDroppedAtIU() { stats.hovSpecIdrsDroppedAtIU++; }
    void countSpecIdrRetriedAtIU() { stats.hovSpecIdrsRetriedAtIU++; }

    // Coalesce the pending DAGs of an index line whose dependent accesses
    // fall in the same data line into one IDR each: the first in program
    // order is kept, a write among them makes it a WriteValue, and the
    // absorbed DAGs' index elements are marked read. Needs the index data
    // for the addresses. Runs once per batch of entries.
    void coalesceExtensions(Addr line_addr, DataBlock &index_data);

    // Check if there are remaining entries for this lineAddr.
    bool hasDependentWork(Addr line_addr);

    // Peek at the next dependent request's physical address without
    // popping; the Gather Unit reads the index element here, so it is
    // marked read whether the IDR is then sent or dropped.
    Addr peekDependentAddr(Addr line_addr, DataBlock &index_data);
    // Whether the next dependent request is a write (WriteValue).
    bool peekDependentIsWrite(Addr line_addr);

    // Drop the front dependent entry without generating a request.
    void dropDependentReq(Addr line_addr);

    // Clear all entries for lineAddr.
    void clearExtensions(Addr line_addr);

    // Coalescing across index lines (see m_idrInFlight). idrVerdict says
    // what to do with an IDR to `data_line`: IDR_SEND, IDR_DROP (one is
    // in flight, or was sent recently; a write dropped over a read is
    // counted as lost intent), or IDR_UPGRADE (a read is parked in retry:
    // the caller lifts hov TBE parkedIdrHovId to WriteValue and drops
    // this one). note* keep the tables from IssueIDR, HovAck, RetryAck and
    // the retry's resend.
    static constexpr int IDR_SEND = 0;
    static constexpr int IDR_DROP = 1;
    static constexpr int IDR_UPGRADE = 2;
    int idrVerdict(Addr data_line, bool write);
    Addr parkedIdrHovId(Addr data_line);
    void noteIdrSent(Addr data_line, Addr hov_id, bool write,
                     int recent_lines);
    void noteIdrAcked(Addr data_line);
    void noteIdrParked(Addr data_line);
    void noteIdrResent(Addr data_line);

    // Whether a line address maps to some downstream machine (an IDR to an
    // unmapped address is dropped instead of becoming a fatal mapping).
    bool isMappedDownstream(Addr addr) const;
    void countIdrDroppedUnmapped() { stats.idrDroppedUnmapped++; }
    void countStashOrphaned() { stats.hovStashOrphaned++; }
    // DAG descriptors extracted at the Gather Unit, by carrier: 0 an
    // index request, 1 a notification, 2 a post-evict write-back.
    void countDAGCarrier(int carrier, RequestPtr req);

    // The post-evict stash (see m_postEvicts).
    bool hasDAG(RequestPtr req);
    void stashPostEvictDAG(Addr line_addr, RequestPtr req);
    bool hasStash(Addr line_addr);
    RequestPtr takePostEvictDAG(Addr line_addr);
    bool isReplEnqueued(Addr line_addr);
    void markReplEnqueued(Addr line_addr);
    void clearReplEnqueued(Addr line_addr);

  public:
    // Whether a live cover row holds element `elem` of an index line for
    // (rid, pair), and as a write when `write` (a read does not cover a
    // write).
    bool hovCovers(Addr line_addr, RequestorID rid, unsigned pair, int elem,
                   bool write);

    bool
    disambiguated(Addr alias)
    {
        return aliasToAddrMap.find(alias) != aliasToAddrMap.end();
    }
    void
    setAliasForAddr(Addr alias, Addr address)
    {
        addrToAliasMap[address] = alias;
        aliasToAddrMap[alias] = address;
    }
    Addr getAddrFromAlias(Addr alias) { return aliasToAddrMap[alias]; }
    // FFUTSYM
  public:
    PARAMS(RubyController);
    AbstractController(const Params &p);
    void init();

    NodeID getVersion() const { return m_machineID.getNum(); }
    MachineType getType() const { return m_machineID.getType(); }

    void initNetworkPtr(Network* net_ptr) { m_net_ptr = net_ptr; }

    // return instance name
    void blockOnQueue(Addr, MessageBuffer*);
    bool isBlocked(Addr) const;
    void unblock(Addr);
    bool isBlocked(Addr);

    virtual MessageBuffer* getMandatoryQueue() const = 0;
    virtual MessageBuffer* getMemReqQueue() const = 0;
    virtual MessageBuffer* getMemRespQueue() const = 0;

    // That function must be called by controller when dequeuing mem resp queue
    // for memory controller to receive the retry request in time
    void memRespQueueDequeued();
    // Or that function can be called to perform both dequeue and notification
    // at once.
    void dequeueMemRespQueue();

    virtual AccessPermission getAccessPermission(const Addr &addr) = 0;

    virtual void print(std::ostream & out) const = 0;
    virtual void wakeup() = 0;
    virtual void resetStats() = 0;
    virtual void regStats();

    virtual void recordCacheTrace(int cntrl, CacheRecorder* tr) = 0;
    virtual Sequencer* getCPUSequencer() const = 0;
    virtual DMASequencer* getDMASequencer() const = 0;
    virtual GPUCoalescer* getGPUCoalescer() const = 0;

    // This latency is used by the sequencer when enqueueing requests.
    // Different latencies may be used depending on the request type.
    // This is the hit latency unless the top-level cache controller
    // introduces additional cycles in the response path.
    virtual Cycles mandatoryQueueLatency(const RubyRequestType& param_type)
    { return m_mandatory_queue_latency; }

    //! These functions are used by ruby system to read/write the data blocks
    //! that exist with in the controller.
    virtual bool functionalReadBuffers(PacketPtr&) = 0;
    virtual void functionalRead(const Addr &addr, PacketPtr)
    { panic("functionalRead(Addr,PacketPtr) not implemented"); }
    /**
     * Returns the priority used by functional reads when deciding from which
     * controller to read a Maybe_Stale data block.
     * Lower positive values have higher priority, negative values are ignored.
     *
     * @return the controller's priority
     */
    virtual int functionalReadPriority() { return -1; }

    //! Functional read that reads only blocks not present in the mask.
    //! Return number of bytes read.
    virtual bool functionalReadBuffers(PacketPtr&, WriteMask &mask) = 0;
    virtual void functionalRead(const Addr &addr, PacketPtr pkt,
                               WriteMask &mask)
    { panic("functionalRead(Addr,PacketPtr,WriteMask) not implemented"); }

    void functionalMemoryRead(PacketPtr);
    //! The return value indicates the number of messages written with the
    //! data from the packet.
    virtual int functionalWriteBuffers(PacketPtr&) = 0;
    virtual int functionalWrite(const Addr &addr, PacketPtr) = 0;
    int functionalMemoryWrite(PacketPtr);

    //! Function for enqueuing a prefetch request
    virtual void enqueuePrefetch(const Addr &, const RubyRequestType&)
    { fatal("Prefetches not implemented!");}

    //! Notifies controller of a request coalesced at the sequencer.
    //! By default, it does nothing. Behavior is protocol-specific.
    //! access_name is the request's label ("" without one), so the
    //! per-name hit and miss stats count coalesced requests too.
    virtual void notifyCoalesced(const Addr& addr,
                                 const RubyRequestType& type,
                                 const RequestPtr& req,
                                 const DataBlock& data_blk,
                                 const bool& was_miss,
                                 const std::string& access_name)
    { }

    //! Function for collating statistics from all the controllers of this
    //! particular type. This function should only be called from the
    //! version 0 of this controller type.
    virtual void collateStats()
    {fatal("collateStats() should be overridden!");}

    //! Initialize the message buffers.
    virtual void initNetQueues() = 0;

    /** A function used to return the port associated with this bus object. */
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID);

    bool recvTimingResp(PacketPtr pkt);
    Tick recvAtomic(PacketPtr pkt);

    const AddrRangeList &getAddrRanges() const { return addrRanges; }

  public:
    MachineID getMachineID() const { return m_machineID; }
    RequestorID getRequestorId() const { return m_id; }

    statistics::Histogram& getDelayHist() { return stats.delayHistogram; }
    statistics::Histogram& getDelayVCHist(uint32_t index)
    { return *(stats.delayVCHistogram[index]); }

    bool respondsTo(Addr addr)
    {
        for (auto &range: addrRanges)
            if (range.contains(addr)) return true;
        return false;
    }

    /**
     * Map an address to the correct MachineID
     *
     * This function querries the network for the NodeID of the
     * destination for a given request using its address and the type
     * of the destination. For example for a request with a given
     * address to a directory it will return the MachineID of the
     * authorative directory.
     *
     * @param the destination address
     * @param the type of the destination
     * @return the MachineID of the destination
     */
    MachineID mapAddressToMachine(Addr addr, MachineType mtype) const;

    /**
     * Maps an address to the correct dowstream MachineID (i.e. the component
     * in the next level of the cache hierarchy towards memory)
     *
     * This function uses the local list of possible destinations instead of
     * querying the network.
     *
     * @param the destination address
     * @param the type of the destination (optional)
     * @return the MachineID of the destination
     */
    MachineID mapAddressToDownstreamMachine(Addr addr,
                                    MachineType mtype = MachineType_NUM) const;

    bool mappedToMachine(Addr addr, MachineID mid);

    /** List of downstream destinations (towards memory) */
    const NetDest& allDownstreamDest() const { return downstreamDestinations; }

    /** List of upstream destinations (towards the CPU) */
    const NetDest& allUpstreamDest() const { return upstreamDestinations; }

    // Helper methods for commonly used functions called in common/address.hh
    Addr getOffset(Addr addr) const;
    Addr makeLineAddress(Addr addr) const;
    std::string printAddress(Addr addr) const;

  protected:
    //! Profiles original cache requests including PUTs
    void profileRequest(const std::string &request);
    //! Profiles the delay associated with messages.
    void profileMsgDelay(uint32_t virtualNetwork, Cycles delay);

    // Tracks outstanding transactions for latency profiling
    struct TransMapPair { unsigned transaction; unsigned state; Tick time; };
    std::unordered_map<Addr, TransMapPair> m_inTransAddressed;
    std::unordered_map<Addr, TransMapPair> m_outTransAddressed;

    std::unordered_map<Addr, TransMapPair> m_inTransUnaddressed;
    std::unordered_map<Addr, TransMapPair> m_outTransUnaddressed;

    /**
     * Profiles an event that initiates a protocol transactions for a specific
     * line (e.g. events triggered by incoming request messages).
     * A histogram with the latency of the transactions is generated for
     * all combinations of trigger event, initial state, and final state.
     * This function also supports "unaddressed" transactions,
     * those not associated with an address in memory but
     * instead associated with a unique ID.
     *
     * @param addr address of the line, or unique transaction ID
     * @param type event that started the transaction
     * @param initialState state of the line before the transaction
     * @param isAddressed is addr a line address or a unique ID
     */
    template<typename EventType, typename StateType>
    void incomingTransactionStart(Addr addr,
        EventType type, StateType initialState, bool retried,
        bool isAddressed=true)
    {
        auto& m_inTrans =
          isAddressed ? m_inTransAddressed : m_inTransUnaddressed;
        assert(m_inTrans.find(addr) == m_inTrans.end());
        m_inTrans[addr] = {type, initialState, curTick()};
        if (retried)
          ++(*stats.inTransRetryCnt[type]);
    }

    /**
     * Profiles an event that ends a transaction.
     * This function also supports "unaddressed" transactions,
     * those not associated with an address in memory but
     * instead associated with a unique ID.
     *
     * @param addr address or unique ID with an outstanding transaction
     * @param finalState state of the line after the transaction
     * @param isAddressed is addr a line address or a unique ID
     */
    template<typename StateType>
    void incomingTransactionEnd(Addr addr, StateType finalState,
        bool isAddressed=true)
    {
        auto& m_inTrans =
          isAddressed ? m_inTransAddressed : m_inTransUnaddressed;
        auto iter = m_inTrans.find(addr);
        assert(iter != m_inTrans.end());
        auto &trans = iter->second;

        auto stat_iter_ev = stats.inTransStateChanges.find(trans.transaction);
        gem5_assert(stat_iter_ev != stats.inTransStateChanges.end(),
          "%s: event type=%d not marked as in_trans in SLICC",
          name(), trans.transaction);

        auto stat_iter_state = stat_iter_ev->second.find(trans.state);
        gem5_assert(stat_iter_state != stat_iter_ev->second.end(),
          "%s: event type=%d has no transition from state=%d",
          name(), trans.transaction, trans.state);

        ++(*stat_iter_state->second[(unsigned)finalState]);

        stats.inTransLatHist[iter->second.transaction]->sample(
                                ticksToCycles(curTick() - trans.time));

       m_inTrans.erase(iter);
    }

    /**
     * Profiles an event that initiates a transaction in a peer controller
     * (e.g. an event that sends a request message)
     * This function also supports "unaddressed" transactions,
     * those not associated with an address in memory but
     * instead associated with a unique ID.
     *
     * @param addr address of the line or a unique transaction ID
     * @param type event that started the transaction
     * @param isAddressed is addr a line address or a unique ID
     */
    template<typename EventType>
    void outgoingTransactionStart(Addr addr, EventType type,
        bool isAddressed=true)
    {
        auto& m_outTrans =
          isAddressed ? m_outTransAddressed : m_outTransUnaddressed;
        assert(m_outTrans.find(addr) == m_outTrans.end());
        m_outTrans[addr] = {type, 0, curTick()};
    }

    /**
     * Profiles the end of an outgoing transaction.
     * (e.g. receiving the response for a requests)
     * This function also supports "unaddressed" transactions,
     * those not associated with an address in memory but
     * instead associated with a unique ID.
     *
     * @param addr address of the line with an outstanding transaction
     * @param isAddressed is addr a line address or a unique ID
     */
    void outgoingTransactionEnd(Addr addr, bool retried,
        bool isAddressed=true)
    {
        auto& m_outTrans =
          isAddressed ? m_outTransAddressed : m_outTransUnaddressed;
        auto iter = m_outTrans.find(addr);
        assert(iter != m_outTrans.end());
        auto &trans = iter->second;

        auto stat_iter = stats.outTransLatHist.find(trans.transaction);
        gem5_assert(stat_iter != stats.outTransLatHist.end(),
          "%s: event type=%d not marked as out_trans in SLICC",
          name(), trans.transaction);

        stat_iter->second->sample(
            ticksToCycles(curTick() - trans.time));
        if (retried)
          ++(*stats.outTransRetryCnt[trans.transaction]);
        m_outTrans.erase(iter);
    }

    void stallBuffer(MessageBuffer* buf, Addr addr);
    void wakeUpBuffer(MessageBuffer* buf, Addr addr);
    void wakeUpBuffers(Addr addr);
    void wakeUpAllBuffers(Addr addr);
    void wakeUpAllBuffers();
    bool serviceMemoryQueue();

    /**
     * Functions needed by CacheAccessor. These are implemented in SLICC,
     * thus the const& for all args to match the generated code.
     */
    virtual bool inCache(const Addr &addr, const bool &is_secure)
    { fatal("inCache: prefetching not supported"); return false; }

    virtual bool hasBeenPrefetched(const Addr &addr, const bool &is_secure)
    { fatal("hasBeenPrefetched: prefetching not supported"); return false; }

    virtual bool hasBeenPrefetched(const Addr &addr, const bool &is_secure,
                                   const RequestorID &requestor)
    { fatal("hasBeenPrefetched: prefetching not supported"); return false; }

    virtual bool inMissQueue(const Addr &addr, const bool &is_secure)
    { fatal("inMissQueue: prefetching not supported"); return false; }

    virtual bool coalesce()
    { fatal("coalesce: prefetching not supported"); return false; }

    friend class RubyPrefetcherProxy;

  protected:
    const NodeID m_version;
    MachineID m_machineID;
    const NodeID m_clusterID;

    // RequestorID used by some components of gem5.
    const RequestorID m_id;

    Network *m_net_ptr;
    bool m_is_blocking;
    std::map<Addr, MessageBuffer*> m_block_map;

    typedef std::vector<MessageBuffer*> MsgVecType;
    typedef std::set<MessageBuffer*> MsgBufType;
    typedef std::map<Addr, MsgVecType* > WaitingBufType;
    WaitingBufType m_waiting_buffers;

    unsigned int m_in_ports;
    unsigned int m_cur_in_port;
    const int m_number_of_TBEs;
    const int m_transitions_per_cycle;
    const unsigned int m_buffer_size;
    Cycles m_recycle_latency;
    const Cycles m_mandatory_queue_latency;
    bool m_waiting_mem_retry;
    bool m_mem_ctrl_waiting_retry;

    /**
     * Port that forwards requests and receives responses from the
     * memory controller.
     */
    class MemoryPort : public RequestPort
    {
      private:
        // Controller that operates this port.
        AbstractController *controller;

      public:
        MemoryPort(const std::string &_name, AbstractController *_controller,
                   PortID id = InvalidPortID);

      protected:
        // Function for receiving a timing response from the peer port.
        // Currently the pkt is handed to the coherence controller
        // associated with this port.
        bool recvTimingResp(PacketPtr pkt);

        void recvReqRetry();
    };

    /* Request port to the memory controller. */
    MemoryPort memoryPort;

    // State that is stored in packets sent to the memory controller.
    struct SenderState : public Packet::SenderState
    {
        // Id of the machine from which the request originated.
        MachineID id;

        SenderState(MachineID _id) : id(_id)
        {}
    };

    RubySystem *m_ruby_system = nullptr;

    // Formerly in RubySlicc_ComponentMapping.hh. Moved here to access
    // RubySystem pointer.
    NetDest broadcast(MachineType type);
    int machineCount(MachineType machType);

  private:
    /** The address range to which the controller responds on the CPU side. */
    const AddrRangeList addrRanges;

    std::unordered_map<MachineType, AddrRangeMap<MachineID, 3>>
      downstreamAddrMap;

    NetDest downstreamDestinations;
    NetDest upstreamDestinations;

    void sendRetryRespToMem();
    MemberEventWrapper<&AbstractController::sendRetryRespToMem> mRetryRespEvent;

  public:
    struct ControllerStats : public statistics::Group
    {
        ControllerStats(statistics::Group *parent);

        // Initialized by the SLICC compiler for all events with the
        // "in_trans" property.
        // Only histograms with samples will appear in the stats
        std::unordered_map<unsigned, statistics::Histogram*> inTransLatHist;
        std::unordered_map<unsigned, statistics::Scalar*> inTransRetryCnt;
        // Initialized by the SLICC compiler for all combinations of events
        // with the "in_trans" property, potential initial states, and
        // potential final states. Potential initial states are states that
        // appear in transitions triggered by that event. Currently all states
        // are considered as potential final states.
        std::unordered_map<unsigned, std::unordered_map<unsigned,
          std::vector<statistics::Scalar*>>> inTransStateChanges;

        // Initialized by the SLICC compiler for all events with the
        // "out_trans" property.
        // Only histograms with samples will appear in the stats.
        std::unordered_map<unsigned, statistics::Histogram*> outTransLatHist;
        std::unordered_map<unsigned, statistics::Scalar*>
          outTransRetryCnt;

        //! Counter for the number of cycles when the transitions carried out
        //! were equal to the maximum allowed
        statistics::Scalar fullyBusyCycles;

        //! Histogram for profiling delay for the messages this controller
        //! cares for
        statistics::Histogram delayHistogram;
        std::vector<statistics::Histogram *> delayVCHistogram;

        //! IDRs dropped at the Gather Unit because their data address had
        //! no downstream mapping (a wrong-path index past its pair)
        statistics::Scalar idrDroppedUnmapped;

        //! IDR coalescing at the Gather Unit (idr_coalesce): DAGs of one
        //! index line absorbed into another's IDR to the same data line,
        //! the sizes of those groups, IDRs dropped because one to their
        //! data line was in flight or sent recently, writes dropped over a
        //! read IDR (uop1's store upgrades the line), and read IDRs parked
        //! in retry lifted to WriteValue
        statistics::Scalar idrCoalescedSameLine;
        statistics::Histogram idrCoalesceGroupSize;
        statistics::Scalar idrDroppedInFlight;
        statistics::Scalar idrDroppedRecent;
        statistics::Scalar idrIntentLost;
        statistics::Scalar idrUpgradedInRetry;

        //! Post-evict stashes whose line left without a replacement (an
        //! invalidating snoop or a back-invalidate) and became notifications
        statistics::Scalar hovStashOrphaned;

        //! DAG descriptors the Gather Unit extracted, by the message that
        //! carried them: an index request, a notification, a post-evict
        //! write-back
        statistics::Vector hovDAGsByCarrier;

        //! In-line speculation (speculateLine): entries made, leaders that
        //! made them by mode, speculated IDRs sent and dropped (unmapped,
        //! coalesced), real DAGs dropped because a row covered them,
        //! rows dropped by the line limit, writes speculated as reads,
        //! and speculated entries absorbed into another IDR (one index
        //! line)
        statistics::Scalar hovSpecEntries;
        statistics::Scalar hovSpecLinesForward;
        statistics::Scalar hovSpecLinesLine;
        statistics::Scalar hovSpecIdrsSent;
        statistics::Vector hovSpecIdrsDropped;
        statistics::Scalar hovDagsCoveredAtGU;
        statistics::Scalar hovCoverReplaced;
        statistics::Scalar hovSpecWritesDemoted;
        statistics::Scalar hovSpecCoalesced;

        //! Run-ahead dropped when pools are full: at the Indirect Unit,
        //! run-ahead IDRs dropped with HovAck and refused with RetryAck
        //! (hovSpecIdrsDropped::full counts the Gather Unit's drops)
        statistics::Scalar hovSpecIdrsDroppedAtIU;
        statistics::Scalar hovSpecIdrsRetriedAtIU;
        //! Run-ahead stream rows: allocated, replaced, re-based; per side-
        //! car pair entry: real DAGs seen, run-ahead IDRs sent, real DAGs
        //! they covered, leaders the gate refused, epochs that backed the
        //! pair off; per epoch the accuracy (percent), per run-ahead entry
        //! made ahead of its leader the lead (elements)
        statistics::Scalar hovRaRowsAllocated;
        statistics::Scalar hovRaRowsReplaced;
        statistics::Scalar hovRaRebases;
        statistics::Vector hovRaDemands;
        statistics::Vector hovRaIssued;
        statistics::Vector hovRaHits;
        statistics::Vector hovRaGated;
        statistics::Vector hovRaThrottled;
        statistics::Histogram hovRaAccuracy;
        statistics::Histogram hovRaDistance;
    } stats;

};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_SLICC_INTERFACE_ABSTRACTCONTROLLER_HH__
