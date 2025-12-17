
#include "mem/ruby/structures/LabelCache.hh"

#include "base/trace.hh"
#include "debug/Usefulness.hh"

namespace gem5
{

namespace ruby
{

void
LabelCache::onFirstTouch(const std::string &label, uint64_t address)
{
    uint64_t page_number = address / pageSizeBytes;
    DPRINTFR(Usefulness, "%s: %s: First touch of page %#lx (addr: %#lx) with label %s.\n", name, __func__, page_number, address, label);

    Interval new_interval{page_number, page_number + 1, label};
    insertAndMerge(new_interval);
    labelToIntervalsMap[label].push_back(new_interval);
}

std::optional<std::string>
LabelCache::lookup(Addr address) const
{
    uint64_t page_number = address / pageSizeBytes;
    DPRINTFR(Usefulness, "%s: %s: Looking up address %#lx (page: %#lx).\n", name, __func__, address, page_number);
    auto it = findIntervalContainingConst(page_number);
    if (it != intervalsMap.end()) {
        DPRINTFR(Usefulness, "%s: %s: Found label %s for page %#lx.\n", name, __func__, it->second.label, page_number);
        return it->second.label;
    } else {
        DPRINTFR(Usefulness, "%s: %s: Miss for page %#lx.\n", name, __func__, page_number);
        return std::nullopt;
    }
}

void
LabelCache::invalidateLabel(const std::string &label)
{
    DPRINTFR(Usefulness, "%s: %s: Invalidating label %s.\n", name, __func__, label);
    auto map_iterator = labelToIntervalsMap.find(label);
    if (map_iterator == labelToIntervalsMap.end()) {
        DPRINTFR(Usefulness, "%s: %s: No intervals recorded for label %s.\n", name, __func__, label);
        return;
    }
    for (const Interval &entry : map_iterator->second) {
        // NOTE: Everytime we need to invalidate a label, we also need to
        // remove the invalidation from before and then override it.
        // invalidated will be nullptr if the cache is actually the
        // invalidated label cache. This pointer is set outside constructor.
        if (invalidated != nullptr) {
            Addr start_addr = entry.start * pageSizeBytes;
            if (invalidated->lookup(start_addr) != std::nullopt) {
                invalidated->removeInterval(entry);
            }
            invalidated->insertAndMerge(entry);
            DPRINTFR(Usefulness, "%s: %s: Adding interval [%lu, %lu) "
                "with label %s to invalidated cache.\n",
                name, __func__, entry.start, entry.end, entry.label);
        }
        removeInterval(entry);
    }
    labelToIntervalsMap.erase(map_iterator);
    DPRINTFR(Usefulness, "%s: %s: Label %s invalidated.\n", name, __func__, label);
}

bool
LabelCache::canOverride(const std::string &label, Addr address) const
{
    return (invalidated->lookup(address) != std::nullopt &&
           invalidated->lookup(address).value() == label);
}


std::map<uint64_t, LabelCache::Interval>::const_iterator
LabelCache::findIntervalContainingConst(uint64_t page_number) const
{
    auto upper = intervalsMap.upper_bound(page_number);
    if (upper == intervalsMap.begin()) {
        return intervalsMap.end();
    } else {
        auto candidate = std::prev(upper);
        if (candidate->second.start <= page_number && page_number < candidate->second.end)
        {
            return candidate;
        } else {
            return intervalsMap.end();
        }
    }
}

void
LabelCache::insertAndMerge(const Interval &interval_to_insert)
{
    uint64_t merged_start = interval_to_insert.start;
    uint64_t merged_end   = interval_to_insert.end;
    const std::string &label = interval_to_insert.label;
    DPRINTFR(Usefulness,
            "%s: %s: Inserting interval [%lu, %lu) with label %s.\n",
            name, __func__, merged_start, merged_end, label);

    // Merge with predecessor if adjacent and same-label
    auto upper = intervalsMap.upper_bound(merged_start);
    if (upper != intervalsMap.begin()) {
        auto predecessor = std::prev(upper);
        if (predecessor->second.label == label
            && predecessor->second.end == merged_start)
        {
            DPRINTFR(Usefulness, "%s: %s: Merging with predecessor [%lu, %lu) label=%s.\n",
                    name, __func__, predecessor->second.start, predecessor->second.end, label);
            merged_start = predecessor->second.start;
            merged_end = std::max(merged_end, predecessor->second.end);
            intervalsMap.erase(predecessor);
        } else {
            DPRINTFR(Usefulness,
                    "%s: %s: No predecessor merge for [%lu, %lu) label=%s.\n",
                    name, __func__, merged_start, merged_end, label);
        }
    } else {
        DPRINTFR(Usefulness, "%s: %s: No predecessor to consider.\n",name, __func__);
    }

    // Merge with successors
    while (true) {
        auto successor = intervalsMap.upper_bound(merged_start);
        if (successor == intervalsMap.end() || successor->second.label != label || successor->second.start != merged_end)
        {
            DPRINTFR(Usefulness, "%s: %s: No successor merge for [%lu, %lu) label=%s.\n",
                    name, __func__, merged_start, merged_end, label);
            break;
        }
        DPRINTFR(Usefulness, "%s: %s: Merging with successor [%lu, %lu) label=%s.\n",
                name, __func__, successor->second.start, successor->second.end, label);
        merged_end = std::max(merged_end, successor->second.end);
        intervalsMap.erase(successor);
    }

    Interval merged_interval{merged_start, merged_end, label};
    intervalsMap[merged_start] = merged_interval;
    DPRINTFR(Usefulness, "%s: %s: Merged interval now [%lu, %lu) label=%s.\n",
            name, __func__, merged_interval.start, merged_interval.end, label);
}

void
LabelCache::removeInterval(const Interval &interval_to_remove)
{
    uint64_t removal_start = interval_to_remove.start;
    uint64_t removal_end   = interval_to_remove.end;
    const std::string &label = interval_to_remove.label;
    DPRINTFR(Usefulness, "%s: %s: Removing [%lu, %lu) label=%s.\n",
            name, __func__, removal_start, removal_end, label);

    auto it = intervalsMap.find(removal_start);
    if (it == intervalsMap.end()) {
        DPRINTFR(Usefulness, "%s: %s: Interval not found.\n", name, __func__);
        return;
    }
    Interval original = it->second;
    intervalsMap.erase(it);

    if (removal_start > original.start) {
        Interval left_remainder{original.start, removal_start, original.label};
        DPRINTFR(Usefulness, "%s: %s: Added left remainder [%lu, %lu) label=%s.\n",
                name, __func__, left_remainder.start, left_remainder.end, original.label);
    }
    if (removal_end < original.end) {
        Interval right_remainder{removal_end, original.end, original.label};
        intervalsMap[right_remainder.start] = right_remainder;
        DPRINTFR(Usefulness, "%s: %s: Added right remainder [%lu, %lu) label=%s.\n",
                name, __func__, right_remainder.start, right_remainder.end, original.label);
    }
}

} // namespace ruby

} // namespace gem5
