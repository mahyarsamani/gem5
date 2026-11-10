#ifndef __MEM_MAA_MAA_HH__
#define __MEM_MAA_MAA_HH__

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <queue>
#include <string>

#include "base/trace.hh"
#include "base/types.hh"
#include "mem/dx100/IF.hh"
#include "mem/cache/tags/base.hh"
#include "mem/packet.hh"
#include "mem/packet_queue.hh"
#include "mem/qport.hh"
#include "mem/request.hh"
#include "sim/clocked_object.hh"
#include "sim/system.hh"
#include "arch/generic/mmu.hh"

// DX100 port to CHI: the fork defines this in its src/mem/packet.hh.
#ifndef MAX_CMD_REGIONS
#define MAX_CMD_REGIONS 256
#endif

#define ADDR_CHANNEL_LEVEL   0
#define ADDR_RANK_LEVEL      1
#define ADDR_BANKGROUP_LEVEL 2
#define ADDR_BANK_LEVEL      3
#define ADDR_ROW_LEVEL       4
#define ADDR_COLUMN_LEVEL    5
#define ADDR_MAX_LEVEL       6

namespace gem5 {

namespace memory {
class DRAMInterface;
}

struct MAAParams;
class IF;
class RF;
class SPD;
class StreamAccessUnit;
class IndirectAccessUnit;
class Invalidator;
class ALUUnit;
class RangeFuserUnit;
class Instruction;
typedef Instruction *InstructionPtr;
struct Register;
typedef Register *RegisterPtr;

/**
 * A basic cache interface. Implements some common functions for speed.
 */
class MAA : public ClockedObject {
    typedef std::pair<Addr, Addr> AddrRegion;
    /**
     * A cache response port is used for the CPU-side port of the cache,
     * and it is basically a simple timing port that uses a transmit
     * list for responses to the CPU (or connected requestor). In
     * addition, it has the functionality to block the port for
     * incoming requests. If blocked, the port will issue a retry once
     * unblocked.
     */
    class MAAResponsePort : public QueuedResponsePort {

    protected:
        MAAResponsePort(const std::string &_name, MAA &_maa, const std::string &_label);

        MAA &maa;

        /** A normal packet queue used to store responses. */
        RespPacketQueue queue;
    };

    /**
     * DX100 port to CHI: the register port, in place of the fork's
     * per-core CpuSidePort. One response port on the IO crossbar that the
     * cores' data sequencers forward their uncacheable accesses to; it
     * serves every range but the cacheable scratchpad (Sprint 2) and the
     * region registers. Requests carry their requestor and context ids,
     * which the decoder keys instructions and registers by.
     */
    class PioPort : public MAAResponsePort {
    protected:
        bool recvTimingReq(PacketPtr pkt) override;

        Tick recvAtomic(PacketPtr pkt) override;

        void recvFunctional(PacketPtr pkt) override;

        AddrRangeList getAddrRanges() const override;

    public:
        PioPort(const std::string &_name, MAA &_maa,
                const std::string &_label);
    };

    /**
     * DX100 port to CHI: the data port, in place of the fork's
     * CacheSidePort (one per core, into the LLC bus) and MemSidePort (one
     * per DRAM channel, around coherence). One request port per DRAM
     * channel, each into the sequencer of a CHI DMA requester, so the
     * directory keeps every access coherent. Plain ReadReq and WriteReq
     * only; nothing snoops it. A send that the sequencer refuses blocks
     * the port until its retry.
     */
    class DataPort : public RequestPort {
    protected:
        bool recvTimingResp(PacketPtr pkt) override;

        void recvReqRetry() override;

        MAA &maa;
        int channel_id;
        bool blocked;

    public:
        bool sendPacket(PacketPtr pkt);
        bool isBlocked() const { return blocked; }

        DataPort(const std::string &_name, MAA &_maa, int _channel_id);
    };

protected:
    PioPort *pioPort;
    std::vector<DataPort *> dataPorts;

public:
    SPD *spd;
    RF *rf;
    IF *ifile;
    StreamAccessUnit *streamAccessUnits;
    IndirectAccessUnit *indirectAccessUnits;
    Invalidator *invalidator;
    ALUUnit *aluUnits;
    RangeFuserUnit *rangeUnits;

    // DRAM organisation for address mapping (Ramulator2's in the fork;
    // gem5's DRAMInterface here, see addDRAMInterface).
    std::vector<int> m_org;
    std::vector<int> m_addr_bits; // How many address bits for each level in the hierarchy?
    int m_num_levels;             // How many levels in the hierarchy?
    int m_tx_offset;
    int m_col_bits_idx;
    int m_row_bits_idx;
    // DX100 port to CHI: what decodePacket of the DRAMInterface needs.
    int m_intlv_low_bit;     // lowest channel-interleaving bit
    int m_intlv_bits;        // log2(channels)
    int m_burst_size;        // bytes per DRAM burst
    int m_bursts_per_row;    // bursts per row buffer
    int m_banks_per_rank;    // banks per rank, all bank groups
    Addr m_rows_per_bank;
    AddrRange dramRange;     // channel 0's range: getOffset decodes any

public:
    std::vector<int> map_addr(Addr addr);
    int channel_addr(Addr addr);
    int core_addr(Addr addr);
    Addr calc_Grow_addr(std::vector<int> addr_vec);
    void addDRAMInterface(memory::DRAMInterface *iface);
    bool sendPacketMem(PacketPtr pkt);
    bool sendPacketCache(PacketPtr pkt);
    void dataPortRetry(int channel_id);

protected:
    /**
     * Performs the access specified by the request.
     * @param pkt The request to perform.
     */
    void recvTimingReq(PacketPtr pkt, int core_id);

    /**
     * Handles a response from the bus.
     * @param pkt The response packet
     */
    void recvTimingResp(PacketPtr pkt);

    /**
     * Serves a write to the region registers (DX100 port to CHI: they
     * replace the fork's add_mem_region pseudo-op).
     */
    void recvRegionWrite(PacketPtr pkt);

    /**
     * Schedules the response to a register-port request (DX100 port to
     * CHI): one cycle after `cycles` of the accelerator plus pio_latency.
     */
    void sendPioResponse(PacketPtr pkt, Cycles cycles = Cycles(1));

    /**
     * Determine if an address is in the ranges covered by this
     * cache. This is useful to filter snoops.
     *
     * @param addr Address to check against
     *
     * @return The id of the range that contains the address, or -1 if none
     */
    int inRange(Addr addr) const;

    /**
     * The address range to which the cache responds on the CPU side.
     * Normally this is all possible memory addresses. */
    const AddrRangeList addrRanges;
    // DX100 port to CHI: the ranges the register port serves, and the
    // region registers (start, end of region i at 16 i, 16 i + 8; any
    // write at MAX_CMD_REGIONS * 16 clears every region).
    AddrRangeList pioAddrRanges;
    const AddrRange regionRange;
    std::vector<Addr> regionStarts;
    const Tick pioLatency;
    memory::DRAMInterface *dramInterface;

public:
    unsigned int num_tiles;
    unsigned int num_tile_elements;
    unsigned int num_regs;
    unsigned int num_instructions_per_core;
    unsigned int num_instructions_per_maa;
    unsigned int num_instructions_total;
    unsigned int num_row_table_rows_per_slice;
    unsigned int num_row_table_entries_per_subslice_row;
    unsigned int num_row_table_config_cache_entries;
    bool reconfigure_row_table;
    bool reorder_row_table;
    bool force_cache_access;
    unsigned int num_initial_row_table_slices;
    unsigned int num_request_table_addresses;
    unsigned int num_request_table_entries_per_address;
    unsigned int num_memory_channels;
    unsigned int num_cores;
    unsigned int num_channels;
    unsigned int num_maas;
    unsigned int num_cores_per_maas;
    unsigned int m_core_addr_bits;

    Cycles rowtable_latency;
    RequestorID requestorId;

    std::vector<AddrRegion> addrRegions;
    int maxRegionID;
    void addAddrRegion(Addr start, Addr end, int8_t id);
    void clearAddrRegion();
    int getAddrRegion(Addr addr);

public:
    /** System we are currently operating in. */
    System *system;

    /** Registered mmu for address translations */
    BaseMMU *mmu;

public:
    MAA(const MAAParams &p);
    ~MAA();

    void init() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    const AddrRangeList &getPioAddrRanges() const { return pioAddrRanges; }
    void setTileReady(int tileID, int wordSize);
    void finishInstructionCompute(InstructionPtr instruction);
    void finishInstructionInvalidate(InstructionPtr instruction, int tileID);
    bool sentMemSidePacket(PacketPtr pkt);
    Tick getClockEdge(Cycles cycles = Cycles(0)) const;
    Cycles getTicksToCycles(Tick t) const;
    Tick getCyclesToTicks(Cycles c) const;
    void resetStats() override;
    bool getAddrRegionPermit(Instruction *instruction);
    void scheduleIssueInstructionEvent(int latency = 0);

protected:
    std::vector<RequestorID> my_instruction_RIDs;
    std::map<RequestorID, int> my_RID_to_core_id;
    std::vector<PacketPtr> my_instruction_pkts;
    std::vector<bool> my_instruction_recvs;
    std::vector<PacketPtr> my_ready_pkts;
    std::vector<RegisterPtr> my_registers;
    std::vector<PacketPtr> my_register_pkts;
    std::vector<int> my_ready_tile_ids;
    std::vector<InstructionPtr> my_instructions;
    uint8_t getTileStatus(InstructionPtr instruction, int tile_id, bool is_dst);
    void issueInstruction();
    void dispatchInstruction();
    void dispatchRegister();
    EventFunctionWrapper issueInstructionEvent, dispatchInstructionEvent, dispatchRegisterEvent;
    void scheduleDispatchInstructionEvent(int latency = 0);
    void scheduleDispatchRegisterEvent(int latency = 0);
    bool *streamAccessIdle;
    bool *indirectAccessIdle;
    bool *aluUnitsIdle;
    bool *rangeUnitsIdle;
    bool invalidatorIdle;
    std::unique_ptr<Packet> pendingDelete;

public:
    Tick my_last_idle_tick;
    Tick my_last_reset_tick;
    bool allFuncUnitsIdle();
    Tick getCurTick();

public:
    struct MAAStats : public statistics::Group {
        MAAStats(statistics::Group *parent, int num_maas, MAA *_maa);

        MAA *maa;
        void preDumpStats() override;

        /** Number of instructions. */
        statistics::Scalar numInst_INDRD;
        statistics::Scalar numInst_INDWR;
        statistics::Scalar numInst_INDRMW;
        statistics::Scalar numInst_STRRD;
        statistics::Scalar numInst_STRWR;
        statistics::Scalar numInst_RANGE;
        statistics::Scalar numInst_ALUS;
        statistics::Scalar numInst_ALUV;
        statistics::Scalar numInst_ALUR;
        statistics::Scalar numInst_INV;
        statistics::Scalar numInst;

        /** Cycles of instructions. */
        statistics::Scalar cycles_INDRD;
        statistics::Scalar cycles_INDWR;
        statistics::Scalar cycles_INDRMW;
        statistics::Scalar cycles_STRRD;
        statistics::Scalar cycles_STRWR;
        statistics::Scalar cycles_RANGE;
        statistics::Scalar cycles_ALUS;
        statistics::Scalar cycles_ALUV;
        statistics::Scalar cycles_ALUR;
        statistics::Scalar cycles_INV;
        statistics::Scalar cycles_IDLE;
        statistics::Formula cycles_BUSY;
        statistics::Scalar cycles_TOTAL;
        statistics::Scalar cycles;

        /** Average cycles per instruction. */
        statistics::Formula avgCPI_INDRD;
        statistics::Formula avgCPI_INDWR;
        statistics::Formula avgCPI_INDRMW;
        statistics::Formula avgCPI_STRRD;
        statistics::Formula avgCPI_STRWR;
        statistics::Formula avgCPI_RANGE;
        statistics::Formula avgCPI_ALUS;
        statistics::Formula avgCPI_ALUV;
        statistics::Formula avgCPI_ALUR;
        statistics::Formula avgCPI_INV;
        statistics::Formula avgCPI;

        /** Port statistics */
        statistics::Scalar port_cache_WR_packets;
        statistics::Scalar port_cache_RD_packets;
        statistics::Scalar port_mem_WR_packets;
        statistics::Scalar port_mem_RD_packets;
        statistics::Formula port_cache_packets;
        statistics::Formula port_mem_packets;
        statistics::Formula port_cache_WR_BW;
        statistics::Formula port_cache_RD_BW;
        statistics::Formula port_cache_BW;
        statistics::Formula port_mem_WR_BW;
        statistics::Formula port_mem_RD_BW;
        statistics::Formula port_mem_BW;

        /** Indirect Unit -- Row-Table Statistics. */
        std::vector<statistics::Scalar *> IND_NumInsts;
        std::vector<statistics::Scalar *> IND_NumWordsInserted;
        std::vector<statistics::Scalar *> IND_NumCacheLineInserted;
        std::vector<statistics::Scalar *> IND_NumRowsInserted;
        std::vector<statistics::Scalar *> IND_NumUniqueWordsInserted;
        std::vector<statistics::Scalar *> IND_NumUniqueCacheLineInserted;
        std::vector<statistics::Scalar *> IND_NumUniqueRowsInserted;
        std::vector<statistics::Scalar *> IND_NumRTFull;
        std::vector<statistics::Formula *> IND_AvgWordsPerCacheLine;
        std::vector<statistics::Formula *> IND_AvgCacheLinesPerRow;
        std::vector<statistics::Formula *> IND_AvgRowsPerInst;
        std::vector<statistics::Formula *> IND_AvgUniqueWordsPerCacheLine;
        std::vector<statistics::Formula *> IND_AvgUniqueCacheLinesPerRow;
        std::vector<statistics::Formula *> IND_AvgUniqueRowsPerInst;
        std::vector<statistics::Formula *> IND_AvgRTFullsPerInst;

        /** Indirect Unit -- Cycles of stages. */
        std::vector<statistics::Scalar *> IND_CyclesFill;
        std::vector<statistics::Scalar *> IND_CyclesBuild;
        std::vector<statistics::Scalar *> IND_CyclesRequest;
        std::vector<statistics::Scalar *> IND_CyclesRTAccess;
        std::vector<statistics::Scalar *> IND_CyclesSPDReadAccess;
        std::vector<statistics::Scalar *> IND_CyclesSPDWriteAccess;
        std::vector<statistics::Formula *> IND_AvgCyclesFillPerInst;
        std::vector<statistics::Formula *> IND_AvgCyclesBuildPerInst;
        std::vector<statistics::Formula *> IND_AvgCyclesRequestPerInst;
        std::vector<statistics::Formula *> IND_AvgCyclesRTAccessPerInst;
        std::vector<statistics::Formula *> IND_AvgCyclesSPDReadAccessPerInst;
        std::vector<statistics::Formula *> IND_AvgCyclesSPDWriteAccessPerInst;

        /** Indirect Unit -- Load accesses. */
        std::vector<statistics::Scalar *> IND_LoadsCacheHitResponding;
        std::vector<statistics::Scalar *> IND_LoadsCacheHitAccessing;
        std::vector<statistics::Scalar *> IND_LoadsMemAccessing;
        std::vector<statistics::Scalar *> IND_LoadsCacheHitRespondingLatency;
        std::vector<statistics::Scalar *> IND_LoadsCacheHitAccessingLatency;
        std::vector<statistics::Scalar *> IND_LoadsMemAccessingLatency;
        std::vector<statistics::Formula *> IND_AvgLoadsCacheHitRespondingPerInst;
        std::vector<statistics::Formula *> IND_AvgLoadsCacheHitAccessingPerInst;
        std::vector<statistics::Formula *> IND_AvgLoadsMemAccessingPerInst;
        std::vector<statistics::Formula *> IND_AvgLoadsCacheHitRespondingLatency;
        std::vector<statistics::Formula *> IND_AvgLoadsCacheHitAccessingLatency;
        std::vector<statistics::Formula *> IND_AvgLoadsMemAccessingLatency;

        /** Indirect Unit -- Store accesses. */
        std::vector<statistics::Scalar *> IND_StoresMemAccessing;
        std::vector<statistics::Formula *> IND_AvgStoresMemAccessingPerInst;

        /** Indirect Unit -- Evict accesses. */
        std::vector<statistics::Scalar *> IND_Evicts;
        std::vector<statistics::Formula *> IND_AvgEvictssPerInst;

        /** Stream Unit -- Row-Table Statistics. */
        std::vector<statistics::Scalar *> STR_NumInsts;
        std::vector<statistics::Scalar *> STR_NumWordsInserted;
        std::vector<statistics::Scalar *> STR_NumCacheLineInserted;
        std::vector<statistics::Scalar *> STR_NumRTFull;
        std::vector<statistics::Formula *> STR_AvgWordsPerCacheLine;
        std::vector<statistics::Formula *> STR_AvgCacheLinesPerInst;
        std::vector<statistics::Formula *> STR_AvgRTFullsPerInst;

        /** Stream Unit -- Cycles of stages. */
        std::vector<statistics::Scalar *> STR_CyclesRequest;
        std::vector<statistics::Scalar *> STR_CyclesRTAccess;
        std::vector<statistics::Scalar *> STR_CyclesSPDReadAccess;
        std::vector<statistics::Scalar *> STR_CyclesSPDWriteAccess;
        std::vector<statistics::Formula *> STR_AvgCyclesRequestPerInst;
        std::vector<statistics::Formula *> STR_AvgCyclesRTAccessPerInst;
        std::vector<statistics::Formula *> STR_AvgCyclesSPDReadAccessPerInst;
        std::vector<statistics::Formula *> STR_AvgCyclesSPDWriteAccessPerInst;

        /** Stream Unit -- Load accesses. */
        std::vector<statistics::Scalar *> STR_LoadsCacheAccessing;
        std::vector<statistics::Formula *> STR_AvgLoadsCacheAccessingPerInst;

        /** Stream Unit -- Evict accesses. */
        std::vector<statistics::Scalar *> STR_Evicts;
        std::vector<statistics::Formula *> STR_AvgEvictssPerInst;

        /** Range Fuser Unit -- Cycles of stages. */
        std::vector<statistics::Scalar *> RNG_NumInsts;
        std::vector<statistics::Scalar *> RNG_CyclesCompute;
        std::vector<statistics::Scalar *> RNG_CyclesSPDReadAccess;
        std::vector<statistics::Scalar *> RNG_CyclesSPDWriteAccess;
        std::vector<statistics::Formula *> RNG_AvgCyclesComputePerInst;
        std::vector<statistics::Formula *> RNG_AvgCyclesSPDReadAccessPerInst;
        std::vector<statistics::Formula *> RNG_AvgCyclesSPDWriteAccessPerInst;

        /** ALU Unit -- Cycles of stages. */
        std::vector<statistics::Scalar *> ALU_NumInsts;
        std::vector<statistics::Scalar *> ALU_NumInstsCompare;
        std::vector<statistics::Scalar *> ALU_NumInstsCompute;
        std::vector<statistics::Scalar *> ALU_CyclesCompute;
        std::vector<statistics::Scalar *> ALU_CyclesSPDReadAccess;
        std::vector<statistics::Scalar *> ALU_CyclesSPDWriteAccess;
        std::vector<statistics::Formula *> ALU_AvgCyclesComputePerInst;
        std::vector<statistics::Formula *> ALU_AvgCyclesSPDReadAccessPerInst;
        std::vector<statistics::Formula *> ALU_AvgCyclesSPDWriteAccessPerInst;

        /** ALU Unit -- Comparison Info. */
        std::vector<statistics::Scalar *> ALU_NumComparedWords;
        std::vector<statistics::Scalar *> ALU_NumTakenWords;
        std::vector<statistics::Formula *> ALU_AvgNumTakenWordsPerComparedWords;

        /** ALU Unit -- Comparison Info. */
        statistics::Scalar *INV_NumInvalidatedCachelines;
        statistics::Formula *INV_AvgInvalidatedCachelinesPerInst;

    } stats;

protected:
    struct pair_hash {
        template <class T1, class T2>
        std::size_t operator()(const std::pair<T1, T2> &p) const {
            return std::hash<T1>{}(p.first) ^ (std::hash<T2>{}(p.second) << 1);
        }
    };
    class OutstandingPacket {
    public:
        PacketPtr packet;
        Addr paddr;
        Tick tick;
        MemCmd cmd;
        bool cached;
        bool sent;
        std::vector<uint8_t> maaIDs;
        std::vector<FuncUnitType> funcUnits;
        OutstandingPacket(PacketPtr _packet, Addr _paddr, Tick _tick, MemCmd _cmd)
            : packet(_packet), paddr(_paddr), tick(_tick), cmd(_cmd), cached(false), sent(false) {}
        OutstandingPacket() {}
        OutstandingPacket(const OutstandingPacket &other) {
            packet = other.packet;
            paddr = other.paddr;
            tick = other.tick;
            cmd = other.cmd;
            funcUnits = other.funcUnits;
            maaIDs = other.maaIDs;
            sent = other.sent;
            cached = other.cached;
        }
        bool operator<(const OutstandingPacket &rhs) const {
            return tick < rhs.tick;
        }
        OutstandingPacket &operator=(const OutstandingPacket &other) = default;
    };
    struct CompareByTick {
        bool operator()(const OutstandingPacket &lhs, const OutstandingPacket &rhs) const {
            return lhs.tick < rhs.tick;
        }
    };
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_indirect_cache_read_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_indirect_cache_write_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_indirect_mem_write_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_indirect_mem_read_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_stream_cache_read_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_stream_cache_write_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_stream_mem_write_pkts;
    std::multiset<OutstandingPacket, CompareByTick> *my_outstanding_stream_mem_read_pkts;
    std::unordered_map<Addr, OutstandingPacket> my_outstanding_pkt_map;
    uint32_t *my_num_outstanding_indirect_pkts;
    uint32_t *my_num_outstanding_stream_pkts;
    bool allIndirectEmpty();
    bool scheduleNextSendCache();
    bool scheduleNextSendMem();
    void scheduleSendCacheEvent(int latency = 0);
    void scheduleSendMemEvent(int latency = 0);
    bool sendOutstandingCachePacket();
    bool sendOutstandingMemPacket();
    EventFunctionWrapper sendCacheEvent;
    EventFunctionWrapper sendMemEvent;
    bool *mem_channels_blocked;
    bool *cache_bus_blocked;
    void unblockMemChannel(int channel_id);
    void unblockCache(int core_id);

public:
    void sendPacket(FuncUnitType funcUnit, uint8_t maaID, PacketPtr pkt, Tick tick, bool force_cache = false);
    bool allIndirectPacketsSent(uint8_t maaID);
    bool allStreamPacketsSent(uint8_t maaID);
};
/**
 * Returns the address of the closest aligned fixed-size block to the given
 * address.
 * @param addr Input address.
 * @param block_size Block size in bytes.
 * @return Address of the closest aligned block.
 */
inline Addr addrBlockAligner(Addr addr, Addr block_size) {
    return addr & ~(block_size - 1);
}
inline int getCeiling(int a, int b) {
    return (a + b - 1) / b;
}
} // namespace gem5

#endif //__MEM_MAA_MAA_HH__
