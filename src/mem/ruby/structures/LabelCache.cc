
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
    uint64_t cl_start = alignDown(address);
    uint64_t cl_end   = cl_start + cacheLineSize;
    uint64_t new_page = address / pageSizeBytes;

    DPRINTFR(Usefulness, "%s: %s: First touch at addr %#lx "
            "(cl: [%#lx, %#lx), page: %#lx) with label %s.\n",
            name, __func__, address, cl_start, cl_end,
            new_page, label);

    // Check if the new cache line is already within an existing
    // interval (for any label). If so, nothing to do.
    auto existing_at_addr = findIntervalContaining(cl_start);
    if (existing_at_addr != intervalsMap.end()) {
        if (existing_at_addr->second.label == label) {
            DPRINTFR(Usefulness, "%s: %s: Address %#lx already within "
                    "interval [%#lx, %#lx) for label %s.\n",
                    name, __func__, address,
                    existing_at_addr->second.start,
                    existing_at_addr->second.end, label);
            return;
        }
        // Different label owns this address — don't overwrite.
        DPRINTFR(Usefulness, "%s: %s: Address %#lx already owned by "
                "label %s (interval [%#lx, %#lx)), cannot assign "
                "to %s.\n",
                name, __func__, address,
                existing_at_addr->second.label,
                existing_at_addr->second.start,
                existing_at_addr->second.end, label);
        return;
    }

    // Look for an existing same-label interval on an adjacent page.
    // If found, compute the convex hull to fill the gap.
    auto adj_it = findAdjacentLabelInterval(label, new_page);
    if (adj_it != intervalsMap.end()) {
        Interval &adj = adj_it->second;
        uint64_t merged_start = std::min(adj.start, cl_start);
        uint64_t merged_end   = std::max(adj.end,   cl_end);

        // Safety: check if the hull-filled region overlaps any
        // interval owned by a different label. If so, reject.
        for (auto it = intervalsMap.lower_bound(merged_start);
             it != intervalsMap.end() && it->second.start < merged_end;
             ++it)
        {
            if (it->second.label != label) {
                DPRINTFR(Usefulness, "%s: %s: Rejecting hull fill "
                        "for label %s over [%#lx, %#lx) because "
                        "interval [%#lx, %#lx) is owned by %s.\n",
                        name, __func__, label, merged_start,
                        merged_end, it->second.start,
                        it->second.end, it->second.label);
                // Fall through to insert just the cache line without
                // hull fill.
                insertAndMerge({cl_start, cl_end, label});
                DPRINTFR(Usefulness, "%s: %s: Inserted standalone "
                        "cache line [%#lx, %#lx) for label %s.\n",
                        name, __func__, cl_start, cl_end, label);
                return;
            }
        }

        // Remove the old interval and insert the hull-filled one.
        removeInterval(adj);
        insertAndMerge({merged_start, merged_end, label});

        DPRINTFR(Usefulness, "%s: %s: Hull-filled label %s to "
                "[%#lx, %#lx).\n",
                name, __func__, label, merged_start, merged_end);
    } else {
        // No adjacent-page interval found. Create a new cache-line
        // interval.
        insertAndMerge({cl_start, cl_end, label});
        DPRINTFR(Usefulness, "%s: %s: Created new interval "
                "[%#lx, %#lx) for label %s.\n",
                name, __func__, cl_start, cl_end, label);
    }
}

std::optional<std::string>
LabelCache::lookup(Addr address) const
{
    uint64_t cl_addr = alignDown(address);
    DPRINTFR(Usefulness, "%s: %s: Looking up addr %#lx "
            "(aligned: %#lx).\n",
            name, __func__, address, cl_addr);
    auto it = findIntervalContainingConst(cl_addr);
    if (it != intervalsMap.end()) {
        DPRINTFR(Usefulness, "%s: %s: Found label %s for addr %#lx "
                "(interval [%#lx, %#lx)).\n",
                name, __func__, it->second.label, address,
                it->second.start, it->second.end);
        return it->second.label;
    } else {
        DPRINTFR(Usefulness, "%s: %s: Miss for addr %#lx.\n",
                name, __func__, address);
        return std::nullopt;
    }
}

void
LabelCache::invalidateLabel(const std::string &label)
{
    DPRINTFR(Usefulness, "%s: %s: Invalidating label %s.\n",
            name, __func__, label);

    // Scan intervalsMap for all entries matching this label and
    // remove them. O(n) scan but n is typically small.
    std::vector<uint64_t> keys_to_remove;
    for (auto &[key, interval] : intervalsMap) {
        if (interval.label == label) {
            // Move to invalidated cache if applicable.
            if (invalidated != nullptr) {
                Addr start_addr = interval.start;
                if (invalidated->lookup(start_addr) != std::nullopt) {
                    invalidated->removeInterval(interval);
                }
                invalidated->insertAndMerge(interval);
                DPRINTFR(Usefulness, "%s: %s: Adding interval "
                        "[%#lx, %#lx) with label %s to invalidated "
                        "cache.\n", name, __func__,
                        interval.start, interval.end, label);
            }
            keys_to_remove.push_back(key);
        }
    }
    for (uint64_t key : keys_to_remove) {
        intervalsMap.erase(key);
    }

    DPRINTFR(Usefulness, "%s: %s: Label %s invalidated.\n",
            name, __func__, label);
}

bool
LabelCache::canOverride(const std::string &label, Addr address) const
{
    return (invalidated->lookup(address) != std::nullopt &&
           invalidated->lookup(address).value() == label);
}


std::map<uint64_t, LabelCache::Interval>::const_iterator
LabelCache::findIntervalContainingConst(uint64_t addr) const
{
    auto upper = intervalsMap.upper_bound(addr);
    if (upper == intervalsMap.begin()) {
        return intervalsMap.end();
    } else {
        auto candidate = std::prev(upper);
        if (candidate->second.start <= addr
            && addr < candidate->second.end)
        {
            return candidate;
        } else {
            return intervalsMap.end();
        }
    }
}

std::map<uint64_t, LabelCache::Interval>::iterator
LabelCache::findIntervalContaining(uint64_t addr)
{
    auto upper = intervalsMap.upper_bound(addr);
    if (upper == intervalsMap.begin()) {
        return intervalsMap.end();
    } else {
        auto candidate = std::prev(upper);
        if (candidate->second.start <= addr
            && addr < candidate->second.end)
        {
            return candidate;
        } else {
            return intervalsMap.end();
        }
    }
}

std::map<uint64_t, LabelCache::Interval>::iterator
LabelCache::findAdjacentLabelInterval(const std::string &label,
                                      uint64_t page_number)
{
    // We need to find any interval for the same label that touches
    // page (page_number - 1), page_number, or (page_number + 1).
    //
    // An interval "touches" a page if any part of it falls within
    // [page * pageSizeBytes, (page + 1) * pageSizeBytes).
    //
    // Scan from the start address of (page_number - 1) forward,
    // stopping when interval starts exceed (page_number + 2) * pageSize.

    uint64_t scan_start = (page_number > 0)
        ? (page_number - 1) * pageSizeBytes
        : 0;
    uint64_t scan_end = (page_number + 2) * pageSizeBytes;

    // Start from the interval that could contain scan_start.
    // We need upper_bound(scan_start) - 1 as a starting point,
    // because an interval starting before scan_start could still
    // extend into the scan range.
    auto it = intervalsMap.upper_bound(scan_start);
    if (it != intervalsMap.begin()) {
        --it;
    }

    for (; it != intervalsMap.end() && it->second.start < scan_end;
         ++it)
    {
        // Check if this interval has the right label and actually
        // overlaps with the 3-page scan range.
        if (it->second.label == label &&
            it->second.end > scan_start &&
            it->second.start < scan_end)
        {
            return it;
        }
    }

    return intervalsMap.end();
}

void
LabelCache::insertAndMerge(const Interval &interval_to_insert)
{
    uint64_t merged_start = interval_to_insert.start;
    uint64_t merged_end   = interval_to_insert.end;
    const std::string &label = interval_to_insert.label;
    DPRINTFR(Usefulness,
            "%s: %s: Inserting interval [%#lx, %#lx) with label %s.\n",
            name, __func__, merged_start, merged_end, label);

    // Merge with predecessor if overlapping/adjacent and same-label
    auto upper = intervalsMap.upper_bound(merged_start);
    if (upper != intervalsMap.begin()) {
        auto predecessor = std::prev(upper);
        if (predecessor->second.label == label
            && predecessor->second.end >= merged_start)
        {
            DPRINTFR(Usefulness, "%s: %s: Merging with predecessor "
                    "[%#lx, %#lx) label=%s.\n", name, __func__,
                    predecessor->second.start,
                    predecessor->second.end, label);
            merged_start = std::min(merged_start,
                                    predecessor->second.start);
            merged_end = std::max(merged_end,
                                  predecessor->second.end);
            intervalsMap.erase(predecessor);
        } else {
            DPRINTFR(Usefulness, "%s: %s: No predecessor merge for "
                    "[%#lx, %#lx) label=%s.\n", name, __func__,
                    merged_start, merged_end, label);
        }
    } else {
        DPRINTFR(Usefulness, "%s: %s: No predecessor to consider.\n",
                name, __func__);
    }

    // Merge with successors
    while (true) {
        auto successor = intervalsMap.upper_bound(merged_start);
        if (successor == intervalsMap.end()
            || successor->second.label != label
            || successor->second.start > merged_end)
        {
            DPRINTFR(Usefulness, "%s: %s: No successor merge for "
                    "[%#lx, %#lx) label=%s.\n", name, __func__,
                    merged_start, merged_end, label);
            break;
        }
        DPRINTFR(Usefulness, "%s: %s: Merging with successor "
                "[%#lx, %#lx) label=%s.\n", name, __func__,
                successor->second.start,
                successor->second.end, label);
        merged_end = std::max(merged_end, successor->second.end);
        intervalsMap.erase(successor);
    }

    Interval merged_interval{merged_start, merged_end, label};
    intervalsMap[merged_start] = merged_interval;
    DPRINTFR(Usefulness, "%s: %s: Merged interval now [%#lx, %#lx) "
            "label=%s.\n", name, __func__,
            merged_interval.start, merged_interval.end, label);
}

void
LabelCache::removeInterval(const Interval &interval_to_remove)
{
    uint64_t removal_start = interval_to_remove.start;
    uint64_t removal_end   = interval_to_remove.end;
    const std::string &label = interval_to_remove.label;
    DPRINTFR(Usefulness, "%s: %s: Removing [%#lx, %#lx) label=%s.\n",
            name, __func__, removal_start, removal_end, label);

    // Use findIntervalContaining instead of find() to handle
    // merged intervals whose key may differ from removal_start.
    auto it = findIntervalContaining(removal_start);
    if (it == intervalsMap.end()) {
        DPRINTFR(Usefulness, "%s: %s: Interval not found.\n",
                name, __func__);
        return;
    }
    Interval original = it->second;
    intervalsMap.erase(it);

    // Re-insert any remaining fragments.
    if (removal_start > original.start) {
        Interval left_remainder{original.start, removal_start,
                                original.label};
        intervalsMap[left_remainder.start] = left_remainder;
        DPRINTFR(Usefulness, "%s: %s: Added left remainder "
                "[%#lx, %#lx) label=%s.\n", name, __func__,
                left_remainder.start, left_remainder.end,
                original.label);
    }
    if (removal_end < original.end) {
        Interval right_remainder{removal_end, original.end,
                                 original.label};
        intervalsMap[right_remainder.start] = right_remainder;
        DPRINTFR(Usefulness, "%s: %s: Added right remainder "
                "[%#lx, %#lx) label=%s.\n", name, __func__,
                right_remainder.start, right_remainder.end,
                original.label);
    }
}

} // namespace ruby

} // namespace gem5
