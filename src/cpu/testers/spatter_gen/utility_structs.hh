/*
* Copyright (c) 2024 The Regents of The University of California
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

#ifndef __CPU_TESTERS_SPATTER_GEN_UTILITY_STRUCTS_HH__
#define __CPU_TESTERS_SPATTER_GEN_UTILITY_STRUCTS_HH__

#include <deque>
#include <memory>
#include <queue>

#include "base/random.hh"
#include "base/types.hh"
#include "enums/SpatterAccessMode.hh"
#include "enums/SpatterKernelType.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

template<typename T>
class TimedQueue
{
  private:
    Tick latency;

    std::queue<T> items;
    std::queue<Tick> insertionTimes;

  public:
    TimedQueue(Tick latency): latency(latency) {}

    void push(T item, Tick insertion_time)
    {
        items.push(item);
        insertionTimes.push(insertion_time);
    }

    void pop()
    {
        items.pop();
        insertionTimes.pop();
    }

    T front() const { return items.front(); }

    bool empty() const { return items.empty(); }

    size_t size() const { return items.size(); }

    bool hasReady(Tick current_time) const
    {
        if (empty()) {
            return false;
        }
        return (current_time - insertionTimes.front()) >= latency;
    }
};


/**
 * Minimal DependentAccessGen subclass for SpatterGen.
 *
 * SpatterGen pre-computes Phase 2 addresses in the access pair queue,
 * so genNextRequest simply returns a request for the stored data address
 * (ignoring the index_value extracted from the cache line).
 */
class SpatterDAG : public DependentAccessGen
{
  public:
    SpatterDAG(AccessIntent intent, size_t index_size,
               Addr index_addr, Addr data_addr, size_t data_size,
               RequestorID rid):
        DependentAccessGen(index_size)
    {
        _requestorId = rid;
        addDescriptor(intent, index_addr, data_addr, data_size);
    }

    std::unique_ptr<ExtensionBase> clone() const override {
        return std::make_unique<SpatterDAG>(*this);
    }

    RequestPtr genNextRequest(uint64_t index_value) override {
        assert(!isMerged());
        const auto& desc = _descriptors.front();
        return std::make_shared<Request>(
            desc.baseAddr, desc.dataSize, 0, _requestorId);
    }

    Addr genAddress(uint64_t index_value) const override {
        assert(!isMerged());
        return _descriptors.front().baseAddr;
    }

    std::vector<std::unique_ptr<DependentAccessGen>>
        unwrap() const override
    {
        std::vector<std::unique_ptr<DependentAccessGen>> result;
        for (const auto& desc : _descriptors) {
            auto s = std::make_unique<SpatterDAG>(
                desc.intent, _indexSize,
                desc.indexAddr, desc.baseAddr, desc.dataSize,
                _requestorId);
            result.push_back(std::move(s));
        }
        return result;
    }
};

// Represents a single access to a SpatterKernel.
// It supports multiple levels of indirection.
// However, the SpatterKernel class only works with one level of
// indirection (i.e. accessing value[index[i]]).
class SpatterAccess: public Extension<Request, SpatterAccess>,
                     public std::enable_shared_from_this<SpatterAccess>
{
  private:
    friend class SpatterKernel;

    using AccessPair = std::tuple<Addr, size_t>;

    using SpatterAccessMode = enums::SpatterAccessMode;
    using SpatterKernelType = enums::SpatterKernelType;

    static constexpr Addr InstructionSize = 4;

    RequestorID _requestorId;
    SpatterKernelType _kernelType;

    Tick accTripTime;

    Addr emulatedPC;
    std::queue<AccessPair> accessPairs;

    std::shared_ptr<IndirectAccessAlias> _alias;

    Random::RandomPtr rng = Random::genRandom();

    AccessPair nextAccessPair(bool advance)
    {
        assert(tripsLeft() > 0);
        AccessPair access_pair = accessPairs.front();
        if (advance) {
            accessPairs.pop();
        }
        return access_pair;
    }

    RequestPtr createRequest(Addr addr, size_t size, Addr pc)
    {
        RequestPtr req = std::make_shared<Request>(addr, size, 0, _requestorId);
        req->setExtension<SpatterAccess>(shared_from_this());
        // Dummy PC to have PC-based prefetchers latch on;
        // get entropy into higher bits
        // This piece of code is directly copied from
        // gem5::TrafficGen::
        req->setPC(pc << 2);
        return req;
    }

    PacketPtr createPacket(RequestPtr req, MemCmd cmd)
    {
        PacketPtr pkt = new Packet(req, cmd);
        uint8_t* pkt_data = new uint8_t[req->getSize()];
        // Randomly intialize pkt_data, for testing cache coherence.
        for (int i = 0; i < req->getSize(); i++) {
            pkt_data[i] = rng->random<uint8_t>();
        }
        pkt->dataDynamic(pkt_data);
        return pkt;
    }

  public:
    SpatterAccess(
        RequestorID requestor_id,
        SpatterKernelType kernel_type,
        const std::queue<AccessPair> &access_pairs
    ):
        Extension<Request, SpatterAccess>(), _requestorId(requestor_id),
        _kernelType(kernel_type),
        accTripTime(0), emulatedPC(0), accessPairs(access_pairs)
    {}

    void setAlias(std::shared_ptr<IndirectAccessAlias> alias) { _alias = alias; }

    SpatterKernelType type() const { return _kernelType; }

    int tripsLeft() const { return accessPairs.size(); }

    void recordTripTime(Tick trip_time) { accTripTime += trip_time; }
    // NOTE: This is used for the `clone` method;
    void setAccTripTime(Tick acc_trip_time) { accTripTime = acc_trip_time; }

    Tick tripTimeSoFar() const { return accTripTime; }

    std::unique_ptr<ExtensionBase> clone() const override
    {
        auto copy = std::make_unique<SpatterAccess>(
            _requestorId, _kernelType, accessPairs
        );
        copy->setAccTripTime(accTripTime);
        copy->emulatedPC = emulatedPC;
        copy->_alias = _alias;
        return copy;
    }


    RequestPtr nextRequest(bool attach_dag, bool attach_iaa) {
        auto [addr, size] = nextAccessPair(true);

        emulatedPC += InstructionSize;
        RequestPtr req = createRequest(addr, size, emulatedPC - InstructionSize);
        if (attach_dag) {
            auto [data_addr, data_size] = nextAccessPair(false);
            auto intent = (_kernelType == SpatterKernelType::gather)
                ? DependentAccessGen::AccessIntent::Read
                : DependentAccessGen::AccessIntent::Write;
            auto dag = std::make_shared<SpatterDAG>(intent, size, addr, data_addr, data_size, _requestorId);
            req->setExtension<DependentAccessGen>(dag);
        }
        if (attach_iaa) {
            req->setExtension<IndirectAccessAlias>(_alias);
        }
        return req;
    }

    PacketPtr nextPacket(SpatterAccessMode access_mode)
    {
        MemCmd cmd;
        if (tripsLeft() > 2) {
            cmd = access_mode == SpatterAccessMode::normal ? MemCmd::ReadReq : MemCmd::ReadIndReq;
        } else if (tripsLeft() == 2) {
            cmd = access_mode == SpatterAccessMode::normal ? MemCmd::ReadReq :
            (_kernelType == SpatterKernelType::gather ? MemCmd::ReadIndReq : MemCmd::WriteIndReq);
        } else {
            cmd = _kernelType == \
                SpatterKernelType::gather ? MemCmd::ReadReq : MemCmd::WriteReq;
        }

        bool indirect = access_mode == SpatterAccessMode::indirect;
        RequestPtr req = nextRequest(indirect && (tripsLeft() > 1), indirect);

        return createPacket(req, cmd);
    }

    void startNextTrip()
    {
        assert(tripsLeft() > 0);
        accessPairs.pop();
    }
};

class SpatterKernel
{
  private:
    using SpatterKernelType = enums::SpatterKernelType;
    using AccessPair = SpatterAccess::AccessPair;

    template <typename T>
    class RollingDeque : public std::deque<T> { // Fixed: added <T>
      private:
        size_t remainingRolls;

      public:
        // Use the member initializer list for better performance
        RollingDeque(const std::vector<T>& values):
            std::deque<T>(values.begin(), values.end()),
            remainingRolls(values.size())
        {}

        bool roll() {
            if (this->empty()) return false;

            T to_roll = this->front();
            this->pop_front();
            this->push_back(to_roll);

            if (--remainingRolls == 0) {
                remainingRolls = this->size();
            }

            return remainingRolls == this->size();
        }
    };

    class IndexGen
    {
      private:
        uint32_t indicesPerStride;
        uint32_t stride;

        uint32_t next;
      public:
        IndexGen(): indicesPerStride(0), stride(0), next(0)
        {}

        IndexGen(uint32_t base_index,
                uint32_t indices_per_stride,
                uint32_t stride_size):
            indicesPerStride(indices_per_stride),
            stride(stride_size), next(base_index)
        {}

        uint32_t nextIndex() {
            uint32_t ret = next;
            // update next index
            next++;
            if (next % indicesPerStride == 0) {
                next += (stride - indicesPerStride);
            }
            return ret;
        }
    };

    RequestorID requestorId;
    IndexGen indexGen;

    uint32_t _id;
    uint32_t delta;
    uint32_t count;

    SpatterKernelType _type;

    size_t indexSize;
    Addr baseIndexAddr;

    size_t valueSize;
    Addr baseValueAddr;

    Addr baseAliasAddr;

    // number of times we have left to roll indices to finish one iteration.
    RollingDeque<uint32_t> indices;

    // current iteration over indices
    uint32_t iteration;
  public:

    SpatterKernel(
        RequestorID requestor_id,
        uint32_t id, uint32_t delta, uint32_t count,
        SpatterKernelType type,
        uint32_t base_index, uint32_t indices_per_stride, uint32_t stride,
        size_t index_size, Addr base_index_addr,
        size_t value_size, Addr base_value_addr,
        size_t alias_size, Addr base_alias_addr,
        const std::vector<uint32_t> &pattern
    ):
        requestorId(requestor_id),
        indexGen(base_index, indices_per_stride, stride),
        _id(id), delta(delta), count(count), _type(type),
        indexSize(index_size), baseIndexAddr(base_index_addr),
        valueSize(value_size), baseValueAddr(base_value_addr),
        baseAliasAddr(base_alias_addr), indices(pattern), iteration(0)
    {}

    uint32_t id() const { return _id; }

    SpatterKernelType type() const { return _type; }

    bool done() const { return iteration == count; }

    std::shared_ptr<SpatterAccess> nextSpatterAccess()
    {
        std::queue<AccessPair> access_pairs;
        // get the next index for the index array
        uint32_t index = indexGen.nextIndex();
        Addr index_addr = baseIndexAddr + (index * indexSize);

        uint32_t front = indices.front();
        uint32_t value_index = (delta * iteration) + front;
        Addr value_addr = baseValueAddr + (value_index * valueSize);

        access_pairs.emplace(index_addr, indexSize);
        access_pairs.emplace(value_addr, valueSize);

        // roll indices
        if (indices.roll()) {
            iteration++;
        }

        auto spatter_access = std::make_shared<SpatterAccess>(requestorId, _type, access_pairs);
        auto alias = std::make_shared<IndirectAccessAlias>(baseAliasAddr + (index * valueSize));
        spatter_access->setAlias(alias);
        return spatter_access;
    }
};

} // namespace gem5

#endif // __CPU_TESTERS_SPATTER_GEN_UTILITY_STRUCTS_HH__
