/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

 #include "mem/cache/prefetch/indirect_memory.hh"

 #include <algorithm>
 #include <cstdlib>
 #include <cstring>

 #include "base/cprintf.hh"
 #include "sim/byteswap.hh"
 #include "mem/cache/base.hh"
 #include "params/IndirectMemoryPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

IndirectMemory::IndirectMemory(const IndirectMemoryPrefetcherParams &p)
  : Queued(p),
    maxPrefetchDistance(p.max_prefetch_distance),
    shiftValues(p.shift_values), prefetchThreshold(p.prefetch_threshold),
    streamCounterThreshold(p.stream_counter_threshold),
    streamingDistance(p.streaming_distance),
    prefetchTable((name() + ".PrefetchTable").c_str(),
                  p.pt_table_entries,
                  p.pt_table_assoc,
                  p.pt_table_replacement_policy,
                  p.pt_table_indexing_policy,
                  PrefetchTableEntry(p.num_indirect_counter_bits,
                    genTagExtractor(p.pt_table_indexing_policy))),
    ipd((name() + ".IPD").c_str(), p.ipd_table_entries, p.ipd_table_assoc,
        p.ipd_table_replacement_policy,
        p.ipd_table_indexing_policy,
        IndirectPatternDetectorEntry(p.addr_array_len, shiftValues.size(),
            genTagExtractor(p.ipd_table_indexing_policy))),
    ipdEntryTrackingMisses(nullptr), byteOrder(p.sys->getGuestByteOrder()),
    impStats(this, shiftValues)
{
    impStats.pendingTranslations.functor(
        [this]() { return (statistics::Counter)
                   pfqMissingTranslation.size(); });
}

IndirectMemory::IndirectMemoryStats::IndirectMemoryStats(
    statistics::Group *parent, const std::vector<int> &shifts)
  : statistics::Group(parent, "imp"),
    ADD_STAT(indexReads, statistics::units::Count::get(),
             "PT hits whose value could be read as an index"),
    ADD_STAT(indexReadsEnabled, statistics::units::Count::get(),
             "index reads of an enabled pattern above the threshold"),
    ADD_STAT(ipdAllocs, statistics::units::Count::get(),
             "IPD entries allocated"),
    ADD_STAT(ipdReleases, statistics::units::Count::get(),
             "IPD entries released after a third index without a match"),
    ADD_STAT(ipdStolen, statistics::units::Count::get(),
             "IPD entries whose miss tracking another entry took over"),
    ADD_STAT(patternsEnabled, statistics::units::Count::get(),
             "patterns found, per shift value"),
    ADD_STAT(patternsDisabled, statistics::units::Count::get(),
             "confidence falling to the threshold from above"),
    ADD_STAT(patternsReplaced, statistics::units::Count::get(),
             "PT entries with an enabled pattern replaced"),
    ADD_STAT(confidenceHits, statistics::units::Count::get(),
             "accesses matching an enabled pattern's current target"),
    ADD_STAT(indirectCandidates, statistics::units::Count::get(),
             "indirect target addresses handed to the queue"),
    ADD_STAT(indirectAbsentIndexLine, statistics::units::Count::get(),
             "frontier walks stopped at an index line not in the cache"),
    ADD_STAT(indirectPageStop, statistics::units::Count::get(),
             "frontier walks stopped at the end of the trigger's page"),
    ADD_STAT(frontierResets, statistics::units::Count::get(),
             "frontier restarts because the index stream jumped"),
    ADD_STAT(indirectDistance, statistics::units::Count::get(),
             "distance in elements of every indirect candidate"),
    ADD_STAT(streamCandidates, statistics::units::Count::get(),
             "candidates of IMP's own stream part"),
    ADD_STAT(pendingTranslations, statistics::units::Count::get(),
             "prefetches waiting for a translation at the dump")
{
    patternsEnabled.init(shifts.size());
    for (int i = 0; i < shifts.size(); i++) {
        patternsEnabled.subname(i, csprintf("shift%s%d",
            shifts[i] < 0 ? "Neg" : "", std::abs(shifts[i])));
    }
    indirectDistance.init(0, 127, 4);
}

void
IndirectMemory::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    // This prefetcher requires a PC
    if (!pfi.hasPC()) {
        return;
    }

    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();

    checkAccessMatchOnActiveEntries(addr);

    // First check if this is a miss, if the prefetcher is tracking misses
    if (ipdEntryTrackingMisses != nullptr && miss) {
        // Check if the entry tracking misses has already set its second index
        if (!ipdEntryTrackingMisses->secondIndexSet) {
            trackMissIndex1(addr);
        } else {
            trackMissIndex2(addr);
        }
    } else {
        // if misses are not being tracked, attempt to detect stream accesses
        const PrefetchTableEntry::KeyType key{pc, is_secure};
        PrefetchTableEntry *pt_entry = prefetchTable.findEntry(key);
        if (pt_entry != nullptr) {
            prefetchTable.accessEntry(pt_entry);

            if (pt_entry->address != addr) {
                // Streaming access found
                pt_entry->streamCounter += 1;
                if (pt_entry->streamCounter >= streamCounterThreshold) {
                    int64_t delta = addr - pt_entry->address;
                    for (unsigned int i = 1; i <= streamingDistance; i += 1) {
                        addresses.push_back(AddrPriority(addr + delta * i, 0));
                        impStats.streamCandidates++;
                    }
                }
                pt_entry->address = addr;
                pt_entry->secure = is_secure;


                // if this is a read, read the data from the cache and assume
                // it is an index (this is only possible if the data is already
                // in the cache), also, only indexes up to 8 bytes are
                // considered

                if (!miss && !pfi.isWrite() && pfi.getSize() <= 8) {
                    int64_t index = 0;
                    bool read_index = true;
                    switch(pfi.getSize()) {
                        case sizeof(uint8_t):
                            index = pfi.get<uint8_t>(byteOrder);
                            break;
                        case sizeof(uint16_t):
                            index = pfi.get<uint16_t>(byteOrder);
                            break;
                        case sizeof(uint32_t):
                            index = pfi.get<uint32_t>(byteOrder);
                            break;
                        case sizeof(uint64_t):
                            index = pfi.get<uint64_t>(byteOrder);
                            break;
                        default:
                            // Ignore non-power-of-two sizes
                            read_index = false;
                    }
                    if (read_index) {
                        impStats.indexReads++;
                    }
                    if (read_index && !pt_entry->enabled) {
                        // Not enabled (no pattern detected in this stream),
                        // add or update an entry in the pattern detector and
                        // start tracking misses
                        allocateOrUpdateIPDEntry(pt_entry, index);
                    } else if (read_index) {
                        // Enabled entry, update the index
                        pt_entry->index = index;
                        if (!pt_entry->increasedIndirectCounter) {
                            if (pt_entry->indirectCounter ==
                                prefetchThreshold + 1) {
                                impStats.patternsDisabled++;
                            }
                            pt_entry->indirectCounter--;
                        } else {
                            // Set this to false, to see if the new index
                            // has any match
                            pt_entry->increasedIndirectCounter = false;
                        }

                        // If the counter is high enough, start prefetching
                        // The distance grows with confidence, as gem5 had
                        // it; the targets are those of the next indices,
                        // A[B[i+1..i+distance]], read from the index lines
                        // (the old loop pushed A[B[i]], the element the
                        // core loads next, distance - 1 times).
                        if (pt_entry->indirectCounter > prefetchThreshold) {
                            impStats.indexReadsEnabled++;
                            unsigned distance = std::max(1u,
                                (unsigned)(maxPrefetchDistance *
                                pt_entry->indirectCounter.calcSaturation()));
                            walkFrontier(pt_entry, pfi, distance, addresses,
                                         cache);
                        }
                    }
                }
            }
        } else {
            // findVictim() invalidates the victim, so count the enabled
            // patterns around it to see whether it held one.
            auto count_enabled = [this]() {
                int n = 0;
                for (const auto &entry : prefetchTable) {
                    n += entry.enabled ? 1 : 0;
                }
                return n;
            };
            const int enabled_before = count_enabled();
            pt_entry = prefetchTable.findVictim(key);
            assert(pt_entry != nullptr);
            if (count_enabled() < enabled_before) {
                impStats.patternsReplaced++;
            }
            // The victim's IPD entries must not enable a pattern on the
            // entry's new owner.
            releaseIPDEntriesOf(pt_entry);
            prefetchTable.insertEntry(key, pt_entry);
            pt_entry->address = addr;
            pt_entry->secure = is_secure;
        }
    }
}

void
IndirectMemory::walkFrontier(PrefetchTableEntry *pt_entry,
    const PrefetchInfo &pfi, unsigned distance,
    std::vector<AddrPriority> &addresses, const CacheAccessor &cache)
{
    const unsigned size = pfi.getSize();
    const Addr trigger = pfi.getAddr();
    const Addr limit = trigger + (Addr)distance * size;

    // Continue after the frontier; restart at the trigger when the
    // stream jumped (a new row, a new pass) or the core overtook the
    // frontier (the walk had stopped at an absent line or a page end).
    if (!pt_entry->frontierValid || pt_entry->frontier < trigger ||
        pt_entry->frontier > limit) {
        if (pt_entry->frontierValid) {
            impStats.frontierResets++;
        }
        pt_entry->frontier = trigger;
        pt_entry->frontierValid = true;
    }

    // The index lines are read on the trigger's page only, whose physical
    // address the trigger gives; the next page waits for a trigger on it.
    const Addr page_end = pageAddress(trigger) + pageBytes;
    const Addr trigger_paddr = pfi.getPaddr();
    std::vector<uint8_t> line(blkSize);
    Addr line_addr = MaxAddr;

    for (Addr pos = pt_entry->frontier + size; pos <= limit; pos += size) {
        if (pos + size > page_end) {
            impStats.indirectPageStop++;
            break;
        }
        const Addr pos_line = blockAddress(pos);
        const Addr offset = pos - pos_line;
        if (offset + size > blkSize) {
            // A misaligned index straddles two lines; stop.
            break;
        }
        if (pos_line != line_addr) {
            const Addr pos_line_paddr = trigger_paddr + pos_line - trigger;
            if (!cache.readLineFunctional(pos_line_paddr, line.data(),
                                          blkSize)) {
                impStats.indirectAbsentIndexLine++;
                break;
            }
            line_addr = pos_line;
        }

        // Indices are read unsigned, as for the trigger's own value.
        uint64_t raw = 0;
        switch (size) {
          case sizeof(uint8_t): {
            uint8_t v;
            std::memcpy(&v, &line[offset], size);
            raw = gtoh(v, byteOrder);
            break;
          }
          case sizeof(uint16_t): {
            uint16_t v;
            std::memcpy(&v, &line[offset], size);
            raw = gtoh(v, byteOrder);
            break;
          }
          case sizeof(uint32_t): {
            uint32_t v;
            std::memcpy(&v, &line[offset], size);
            raw = gtoh(v, byteOrder);
            break;
          }
          default: {
            assert(size == sizeof(uint64_t));
            uint64_t v;
            std::memcpy(&v, &line[offset], size);
            raw = gtoh(v, byteOrder);
            break;
          }
        }

        addresses.push_back(AddrPriority(
            targetAddr(pt_entry->baseAddr, (int64_t)raw, pt_entry->shift),
            0));
        impStats.indirectCandidates++;
        impStats.indirectDistance.sample((pos - trigger) / size);
        pt_entry->frontier = pos;
    }
}

Addr
IndirectMemory::targetAddr(Addr base, int64_t index, int shift)
{
    // Shifting by a negative amount is undefined in C++; the paper's -3
    // means one bit per index (a bit vector), a division by 8.
    if (shift >= 0) {
        return base + (Addr)(index << shift);
    }
    return base + (Addr)(index >> -shift);
}

void
IndirectMemory::releaseIPDEntriesOf(const PrefetchTableEntry *pt_entry)
{
    for (auto &ipd_entry : ipd) {
        if (ipd_entry.isValid() && ipd_entry.ptEntry == pt_entry) {
            if (ipdEntryTrackingMisses == &ipd_entry) {
                ipdEntryTrackingMisses = nullptr;
            }
            ipd.invalidate(&ipd_entry);
        }
    }
}

void
IndirectMemory::allocateOrUpdateIPDEntry(
    PrefetchTableEntry *pt_entry, int64_t index)
{
    // The address of the pt_entry is used to index the IPD
    Addr ipd_entry_addr = (Addr) pt_entry;
    const IndirectPatternDetectorEntry::KeyType key{ipd_entry_addr, false};
    IndirectPatternDetectorEntry *ipd_entry = ipd.findEntry(key);
    if (ipd_entry != nullptr) {
        ipd.accessEntry(ipd_entry);
        if (!ipd_entry->secondIndexSet) {
            // Second time we see an index, fill idx2
            ipd_entry->idx2 = index;
            ipd_entry->secondIndexSet = true;
            if (ipdEntryTrackingMisses != nullptr &&
                ipdEntryTrackingMisses != ipd_entry) {
                impStats.ipdStolen++;
            }
            ipdEntryTrackingMisses = ipd_entry;
        } else {
            // Third access! no pattern has been found so far,
            // release the IPD entry
            ipd.invalidate(ipd_entry);
            ipdEntryTrackingMisses = nullptr;
            impStats.ipdReleases++;
        }
    } else {
        // Any entry still tracking misses stops here, the victim included.
        if (ipdEntryTrackingMisses != nullptr) {
            impStats.ipdStolen++;
        }
        ipd_entry = ipd.findVictim(key);
        assert(ipd_entry != nullptr);
        ipd.insertEntry(key, ipd_entry);
        ipd_entry->idx1 = index;
        ipd_entry->ptEntry = pt_entry;
        ipdEntryTrackingMisses = ipd_entry;
        impStats.ipdAllocs++;
    }
}

void
IndirectMemory::trackMissIndex1(Addr miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // If the second index is not set, we are just filling the baseAddr
    // vector
    assert(entry->numMisses < entry->baseAddr.size());
    std::vector<Addr> &ba_array = entry->baseAddr[entry->numMisses];
    int idx = 0;
    for (int shift : shiftValues) {
        ba_array[idx] = miss_addr - targetAddr(0, entry->idx1, shift);
        idx += 1;
    }
    entry->numMisses += 1;
    if (entry->numMisses == entry->baseAddr.size()) {
        // stop tracking misses once we have tracked enough
        ipdEntryTrackingMisses = nullptr;
    }
}
void
IndirectMemory::trackMissIndex2(Addr miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // Second index is filled, compare the addresses generated during
    // the previous misses (using idx1) against newly generated values
    // using idx2, if a match is found, fill the additional fields
    // of the PT entry
    for (int midx = 0; midx < entry->numMisses; midx += 1)
    {
        std::vector<Addr> &ba_array = entry->baseAddr[midx];
        int idx = 0;
        for (int shift : shiftValues) {
            if (ba_array[idx] ==
                    miss_addr - targetAddr(0, entry->idx2, shift)) {
                // Match found!
                // Fill the corresponding pt_entry
                PrefetchTableEntry *pt_entry = entry->ptEntry;
                assert(pt_entry != nullptr);
                pt_entry->baseAddr = ba_array[idx];
                pt_entry->shift = shift;
                pt_entry->enabled = true;
                pt_entry->indirectCounter.reset();
                pt_entry->frontierValid = false;
                impStats.patternsEnabled[idx]++;
                // Release the current IPD Entry
                ipd.invalidate(entry);
                // Do not track more misses
                ipdEntryTrackingMisses = nullptr;
                return;
            }
            idx += 1;
        }
    }
}

void
IndirectMemory::checkAccessMatchOnActiveEntries(Addr addr)
{
    for (auto &pt_entry : prefetchTable) {
        if (pt_entry.enabled) {
            if (addr == targetAddr(pt_entry.baseAddr, pt_entry.index,
                                   pt_entry.shift)) {
                pt_entry.indirectCounter++;
                pt_entry.increasedIndirectCounter = true;
                impStats.confidenceHits++;
            }
        }
    }
}

} // namespace prefetch
} // namespace gem5
