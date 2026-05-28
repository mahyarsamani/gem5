
#ifndef __MEM_RUBY_STRUCTURES_LABELCACHE_HH__
#define __MEM_RUBY_STRUCTURES_LABELCACHE_HH__

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>

#include "base/types.hh"

namespace gem5
{

namespace ruby
{

class LabelCache {
  public:

    explicit LabelCache(std::string name, uint64_t page_size_bytes,
                        uint64_t cache_line_size):
        name(name), pageSizeBytes(page_size_bytes),
        cacheLineSize(cache_line_size)
    {}

    void onFirstTouch(const std::string &label, uint64_t address);

    std::optional<std::string> lookup(uint64_t address) const;

    void invalidateLabel(const std::string &label);

    bool canOverride(const std::string &label, Addr address) const;

    void setInvalidatedCache(LabelCache *invalidated_cache)
    {
        invalidated = invalidated_cache;
    }

  private:
    LabelCache *invalidated;

    struct Interval {
        uint64_t start;
        uint64_t end;
        std::string label;
    };

    std::string name;

    /** Page size in bytes. Used to determine page adjacency for the
     *  convex hull extension rule: spans may only extend across
     *  physically consecutive pages (P and P±1). */
    uint64_t pageSizeBytes;

    /** Cache line size in bytes. This is the minimum unit of tracking:
     *  a single onFirstTouch creates a [alignDown(addr), alignDown(addr)
     *  + cacheLineSize) interval. */
    uint64_t cacheLineSize;

    /** Map from interval start address to the interval. Used for
     *  O(log n) lookup of which label covers a given address.
     *  This is the sole source of truth for label→address mappings. */
    std::map<uint64_t, Interval> intervalsMap;

    /** Align an address down to cache line boundary. */
    uint64_t alignDown(uint64_t addr) const
    {
        return addr & ~(cacheLineSize - 1);
    }

    std::map<uint64_t, Interval>::const_iterator
    findIntervalContainingConst(uint64_t addr) const;

    std::map<uint64_t, Interval>::iterator
    findIntervalContaining(uint64_t addr);

    /** Find an existing same-label interval on a page adjacent to
     *  (or equal to) page_number. Returns intervalsMap.end() if
     *  no such interval exists. */
    std::map<uint64_t, Interval>::iterator
    findAdjacentLabelInterval(const std::string &label,
                              uint64_t page_number);

    void insertAndMerge(const Interval &interval_to_insert);

    void removeInterval(const Interval &interval_to_remove);
};

} // namespace ruby

} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_LABELCACHE_HH__
