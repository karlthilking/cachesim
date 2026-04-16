#include <bit>
#include <span>
#include <algorithm>
#include "../include/params.hpp"
#include "../include/cache.hpp"

namespace cachesim {
cache_directory::cache_directory(size_t size, size_t block_size, u32 assoc)
    : entries(size / block_size)
{
    auto num_sets = size / (block_size * assoc);

    nr_offbits  = std::popcount(block_size - 1);
    nr_ixbits   = std::popcount(num_sets - 1);
    set_mask    = (num_sets - 1) << nr_offbits;
}

std::pair<u64, u64> cache_directory::decompose(u64 addr) const noexcept
{
    auto index  = (addr & set_mask) >> nr_offbits;
    auto tag    = addr >> (nr_offbits + nr_ixbits);

    return {index, tag};
}

void cache_directory::putS(u64 addr, u32 cpuid)
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{entries.data() + (index * assoc), assoc};
    
    /**
     * Find the entry for the given address in the directory and clear the
     * present bit for the calling cpu. If this is the last cpu using this
     * data block (i.e. entry.bitmap == 0), the directory entry can be
     * marked invalid.
     */
    for (auto &entry : set) {
        if (entry.tag == tag) {
            entry.bitmap ^= 1ull << cpuid;
            if (!entry.bitmap)
                entry.state = Invalid;
        }
    }
}

void cache_directory::putM(u64 addr, u32 cpuid)
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{entries.data() + (index * assoc), assoc};

    for (auto &entry : set) {
        if (entry.tag == tag)
            entry.tag = 0;
    }
}

void cache_directory::getS(u64 addr, u32 cpuid)
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{entries.data() + (index * assoc), assoc};
    
    bool found = false;
    for (auto &entry : set) {
        if (entry.tag != tag)
            continue;
        else if (entry.state == Invalid)
            entry.state == Exclusive;
        else if (entry.state == Modified)
            entry.state == Shared;
        entry.bitmap |= (1ull << cpuid);
    }
}

void cache_directory::getM(u64 addr, u32 cpuid)
{
}

std::pair<u64, u64> cache::decompose(void *addr) const noexcept
{
    auto index  = (reinterpret_cast<u64>(addr) & set_mask) >> nr_offbits;
    auto tag    = reinterpret_cast<u64>(addr) >> (nr_ixbits + nr_offbits);

    return {index, tag};
}

cache::cache(size_t size, size_t block_size, int assoc) noexcept
    : lines(size / block_size),
      clock_hands(size / (block_size * assoc))
{
    auto num_sets = size / (block_size * assoc);

    nr_offbits  = std::popcount(block_size - 1);
    nr_ixbits   = std::popcount(num_sets - 1);
    set_mask    = (num_sets - 1) << nr_offbits;
}

void cache::evict(void *addr, cache_state new_state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};
    
    /**
     * Fisrt search for an invalid entry to see if it is possible to evict
     * an invalid entry instead of one that may be referenced soon after
     */
    auto it = std::find_if(set.begin(), set.end(),
                           [](const auto &line) { 
                                return line.state == Invalid; 
                           });
    if (it != set.end()) {
        it->tag = tag;
        it->state = new_state;
        it->u = 1;
        return;
    }
    
    prof.evictions++;
    /**
     * Evict a data block from the set where the desired cache line needs
     * to be placed
     */
    size_t hand = clock_hands[index];
    for (auto i = hand;; i = (i + 1) % set.size()) {
        /* Unset use bit */
        if (set[i].u) {
            set[i].u = 0;
            continue;
        }
        
        /**
         * If the victim data block is in the modified state, it must be written
         * back to lower memory before it is replaced
         */
        if (set[i].state == Modified) {
            prof.write_backs++;
            /**
             * Assume next level cache is inclusive and already has this line
             * marked as Modified so it does not have to transition state
             */
        }

        /**
         * Mark that the cache line that is being evicted in no longer cache
         * by this cpu if this cache level is one before the LLC
         */
        if (level == 2) {
            cache_directory &dir = system::get_directory();
            u64 victim_addr = ((set[i].tag << nr_ixbits) | index) >> nr_offbits;
            dir.remove(victim_addr, 
        }

        /**
         * Now that a victim for eviction was found, update the line with the
         * new tag, state, and set the use bit. Also, remember the clock hand
         * position for the next eviction cycle.
         */
        set[i].tag = tag;
        set[i].state = new_state;
        set[i].u = 1;
        clock_hands[index] = i;
        break;
    }
}

void cache::update(void *addr, cache_state new_state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span(lines.data() + (index * assoc), assoc};

    auto it = std::find_if(set.begin(), set.end(),
                           [](const auto &line) { return line.tag == tag; });
    /**
     * If the line is not present (but invalid) in the cache, another line
     * must be evicted in order to store the updated block which is being
     * brought into the cache.
     */
    if (it == set.end()) {
        evict(addr, new_state);
        return;
    }
        
    /**
     * Otherwise, if the block with the matching tag is present, but invalid,
     * update its state in the cache
     */
    for (auto &line : set) {
        if (line.tag == tag) {
            line.state = new_state;
            line.u = 1;
            return;
        }
    }
}

bool cache::load(void *addr) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    for (auto &line : set) {
        if (line.tag != tag || line.state == Invalid)
            continue;
        prof.hits++;
        line.u = 1;
        /**
         * Regardless of whether the state is Modified, Exclusive or Shared,
         * a processor read that generates a hit does not require any bus
         * transactions or state transitions.
         */
        return true;
    }

    return false;
}

bool cache::store(void *addr) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    for (auto &line : set) {
        /* If tag does not match, proceed to search */
        if (line.tag != tag)
            continue;
        else if (line.state == Invalid) {
            /**
             * If tag matches but the state of the cache line is marked invalid,
             * then this is a stale copy and has to be supplied either from
             * main memory or from a cache-to-cache transfer (whichever has
             * the most up-to-date copy of the data block).
             */
            break;
        }
        prof.hits++;
        line.u = 1;
        if (line.state == Exclusive)
            line.state = Modified;
        else if (line.state == Shared) {
            /**
             * Broadcast to all other processors caching a local copy of this
             * data block to invalidate their copies
             */

        }
        /**
         * If the state was modified, no bus transaction or state transition
         * is necessary (modified state implies exclusivity). The store can
         * be treated as a normal write hit.
         */
        return true;
    }
    
    prof.misses++;
    return false;
}

cpu::cpu(int id) noexcept
    : id(id), 
      L1i_cache(l1i_size, l1i_blk_size, l1i_assoc),
      L1d_cache(l1d_size, l1d_blk_size, l1d_assoc),
      L2_cache(l2_size, l2_blk_size, l2_assoc)
{}

void cpu::access(void *addr, bool data, bool write) noexcept
{
    if (data && write) {

    } else if (data) {

    } else {
        if (L1i.load(addr))
            return;
        else if (L2.load(addr)) {
            /**
             * Data block was found in the L2 cache, bring it into the L1
             * instruction cache and marked it as shared
             */
            L1i.update(addr, Shared);
        }
    }
}

} // cachesim
