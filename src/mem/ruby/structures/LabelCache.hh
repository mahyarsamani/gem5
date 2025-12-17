
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

    explicit LabelCache(std::string name, uint64_t page_size_bytes):
        name(name), pageSizeBytes(page_size_bytes)
    {}

    void onFirstTouch(const std::string &label, uint64_t address);

    std::optional<std::string> lookup(uint64_t address) const;

    void invalidateLabel(const std::string &label);

    bool canOverride(const std::string &label, Addr address) const;

    void setInvalidatedCache(LabelCache *invalidated_cache) { invalidated = invalidated_cache; }

  private:
    LabelCache *invalidated;

    struct Interval {
        uint64_t start;
        uint64_t end;
        std::string label;
    };

    std::string name;
    uint64_t pageSizeBytes;
    std::map<uint64_t, Interval> intervalsMap;
    std::unordered_map<std::string, std::vector<Interval>> labelToIntervalsMap;

    std::map<uint64_t, Interval>::const_iterator findIntervalContainingConst(uint64_t page_number) const;

    void insertAndMerge(const Interval &interval_to_insert);

    void removeInterval(const Interval &interval_to_remove);
};

} // namespace ruby

} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_LABELCACHE_HH__
