#include <bit>
#include <span>
#include <algorithm>
#include <iterator>
#include <cassert>
#include "../include/params.hpp"
#include "../include/cache.hpp"

namespace cachesim {
/**
 * cache::decompose()
 *  Extract the index and tag of a memory address given the parameters of
 *  the cache.
 *
 * @addr: The memory address to decompose into [index, tag]
 */
std::pair<u64, u64> cache::decompose(void *addr) const noexcept
{
    auto index = (reinterpret_cast<u64>(addr) & set_mask) >> nr_offbits;
    auto tag = reinterpret_cast<u64>(addr) >> (nr_ixbits + nr_offbits);

    return {index, tag};
}

cache::cache(size_t size, size_t block_size, u32 assoc, bool local) noexcept
    : lines(size / block_size),
      clock_hands(size / (block_size * assoc)),
      assoc(assoc), local(local)
{
    auto num_sets = size / (block_size * assoc);
    nr_offbits = std::popcount(block_size - 1);
    nr_ixbits = std::popcount(num_sets - 1);
    set_mask = (num_sets - 1) << nr_offbits;
}

/**
 * cache::contains()
 *  Check if a data block corresponding to the given address exists in the
 *  cache. If find_invalid is true, then an invalid line with a matching tag
 *  counts as being present, otherewise, only data blocks with a matching tag
 *  that are also not invalid are considered present.
 *
 * @addr: Address of the load/store that required probing the cache
 * @find_invalid: Consider a data block present as long as the tag matches
 */
bool cache::contains(void *addr, bool find_invalid) const noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) {
        return line.tag == tag;
    });

    if (it != set.end() && (it->state != Invalid || find_invalid))
        return true;

    return false;
}

/**
 * cache::find()
 *  Attempt to find a data block in the cache corresponding to the given
 *  address. If the data block is found, a tuple of true, the state that the
 *  block was discovered in, and the index as type ptrdiff_t of the block is
 *  returned. Otherwise, false is returned for the boolean value in the tuple.
 *
 *  @addr: The address of the load/store requiring a search
 */
std::tuple<bool, cache_state, ptrdiff_t> cache::find(void *addr) const noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) { 
        return line.tag == tag;
    });

    if (it != set.end())
        return {true, it->state, std::distance(lines.begin(), it)};

    return {false, Invalid, -1};
}

/**
 * cache::load()
 *  Attempt to find a data block in the cache to perform a load. If the
 *  data block is found in a private cache, then regardless of if the block
 *  is modified, exclusive or shared this is a cache hit. 
 *
 *  If this cache is shared (L3), then finding the block present is only a
 *  cache hit if the state is shared or exclusive. A block found at L3 in
 *  the modified state indicates that another processor is caching a more
 *  recently modified copy.
 *
 *  If the data block is not found at all, increment the number of read
 *  misses.
 *
 * @addr: The memory address corresponding to the load
 */
std::tuple<bool, cache_state, ptrdiff_t> cache::load(void *addr) noexcept
{
    if (auto [found, state, loc] = find(addr); found && state != Invalid) {
        lines[loc].u |= 1;
        if (local || state != Modified)
            stats.rd_hits++;
        return {true, state, loc};
    }
    
    stats.rd_misses++;
    return {false, Invalid, -1};
}

/**
 * cache::store()
 *  Attempt to find a data block in the cache to perform a store. If the
 *  data block is found, set its reference/use bit and record a write hit.
 *  Otherwise, increment the number of write misses.
 *
 *  The state is returned on a write hit in order to allow the cpu to
 *  initiate a bus transaction if necessary. For e.g. even if the write
 *  hits in a private cache, but the state is shared, a bus transaction 
 *  (BusUpgr) is needed to notify other processors to invalidate their
 *  private copies.
 *
 *  If this cache is shared (L3), the finding the data block in the modified
 *  state is not a write hit; Another processor has a more recent private
 *  copy which needs to be written back to main memory or directly transferred
 *  for this processor.
 *
 *  For a private cache:
 *   state = Invalid -> cache miss
 *   state = Shared -> cache hit -> send BusUpgr -> Modified
 *   state = Exclusive -> cache hit -> no bus transaction -> Modified
 *   state = Modified -> cache hit <-> Modified 
 *
 *  For a shared cache:
 *   state = Invalid -> cache miss
 *   state = Shared -> cache hit -> send BusUpgr -> Modified
 *   state = Exclusive -> cache hit -> send BusUpgr -> Modified
 *   state = Modified -> cache miss -> send BusRdX -> Modified
 *
 * @addr: The memory address corresponding to the store
 */
std::tuple<bool, cache_state, ptrdiff_t> store(void *addr) noexcept
{
    if (auto [found, state, loc] = find(addr); found && state != Invalid) {
        lines[loc].u |= 1;
        if (local || state != Modified)
            stats.wr_hits++;
        return {true, state, loc};
    }
    
    stats.wr_misses++;
    return {false, Invalid, -1};
}

/**
 * cache::elect()
 *  Elect a victim block in the set of the given address such that the
 *  victim block can be evicted and replaced if necessary.
 *
 * @addr: Address specifying the set to identify a victim block in
 */
size_t cache::elect(void *addr) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) {
        return line.state == Invalid;
    });

    if (it != set.end())
        return std::distance(lines.begin(), it);

    for (auto i = clock_hands[index];; i = (i + 1) % set.size()) {
        if (set[i].u) {
            set[i].u = 0;
            continue;
        }
        return (index * assoc) + i;
    }
}

/**
 * cache::evict()
 *  Commit the eviction of the data block at the given index, replacing it
 *  with the data block corresponding to the provided address whose state
 *  will set to the given state.
 *
 * @index: Index to evict/replace in the cache
 * @addr: Address of load/store that necessitated an eviction
 * @state: State to set for the new data block replacing the victim block
 */
void cache::evict(size_t index, void *addr, cache_state state) noexcept
{
    auto [_, tag] = decompose(addr);
    cache_line &victim = lines[index];

    if (victim.state == Modified)
        stats.write_backs++;

    victim.tag = tag;
    victim.state = state;
    victim.u = 1;

    stats.evictions++;
}

/**
 * cache::insert()
 *  Insert a data block brought in from lower memory or from a cache-to-cache
 *  transfer. If there is an available invalid cache line, the invalid line
 *  can be replaced by the insterted line. Otherwise, an eviction needs to
 *  occur to insert the new cache line.
 *
 *  @addr: Address of the original load/store that requested this data block
 *  @state: The state to set for the newly inserted cache line
 */
void cache::insert(void *addr, cache_state state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};
    auto free = elect(addr);

    if (lines[free].state != Invalid)
        evict(free, addr, state);
    else {
        lines[free].tag = tag;
        lines[free].state = state;
        lines[free].u = 1;
    }
}

/**
 * cache::update()
 *  Update the state of a data block in the cache, assuming that the block
 *  corresponding to the given address is present.
 *
 * @addr: Address corresponding to the data block to update in the cache
 * @state: New stat to set for the data block
 */
void cache::update(void *addr, cache_state state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) {
        return line.tag == tag;
    });
    assert(it != set.end());

    it->state = state;
}

/**
 * cpu::recvBusUpgr()
 *  Respond to a BusUpgr request snooped on the bus. If the line is present
 *  in one of this cpu's private caches, invalidate the block and respond
 *  with InvAck. Otherwise if the block is not present, respond with NullAck
 *  to signify that the block is not present in any of this cpu's caches.
 *
 * @addr: Address corresponding to the data block the sending cpu wishes to
 *        acquire write permission for
 */
response cpu::recvBusUpgr(void *addr) noexcept
{
    if (auto [found, s] = L1d.find(addr); found && s != Invalid) {
        L1d.update(addr, Invalid);
        L2.update(addr, Invalid);
        return InvAck;
    } else if (auto [found, s] = L2.find(addr); found && s != Invalid) {
        L2.update(addr, Invalid);
        return InvAck;
    }

    return NullAck;
}

/**
 * cpu::recvBusRdX()
 *  Respond to a BusRdX request snooped on the bus. If the line is present
 *  and modified by this cpu, then it is responsible for flushing the data
 *  block back to main memory and transitioning the block's state to invalid.
 *  If the line is present, but not modified, this cpu can simply invalidate
 *  its copy to allow the requesting cpu to freely write to the block. If the
 *  data block is not present in any of this cpu's private caches, respond
 *  with NullAck.
 *
 * @addr: Address corresponding to the data block that the requesting cpu
 *        is attempting to acquire exclusive access of
 */
response cpu::recvBusRdX(void *addr) noexcept
{
    if (auto [found, s] = L1d.find(addr); found && s != Invalid) {
        L1d.update(addr, Invalid);
        L2.update(addr, Invalid);
        if (s == Modified)
            return Flush;
        return InvAck;
    } else if (auto [found, s] = L2.find(addr); found && s != Invalid) {
        L2.update(addr, Invalid);
        if (s == Modified)
            return Flush;
        return InvAck;
    }

    return NullAck;
}

/**
 * cpu::recvBusRd()
 *  Respond to a BusRd request snooped on the bus. If the corresponding data
 *  block is present and modified, this cpu is responsible for flushing the
 *  data to memory and transitioning the block state to shared. If this cpu
 *  has the data block in the exclusive state, the block should transition
 *  to the shared state to disallow this cpu to freely write to the block.
 *  If the data block is not present in any of this cpu's private caches,
 *  it simply responds with NullAck.
 *
 * @addr: Address of the requesting cpu's load that generate a cache miss
 */
response cpu::recvBusRd(void *addr) noexcept
{
    if (auto [found, s] = L1d.find(addr); found && s != Invalid) {
        if (s == Modified) {
            L1d.update(addr, Shared);
            L2.update(addr, Shared);
            return Flush;
        } else if (s == Exclusive) {
            L1d.update(addr, Shared);
            L2.update(addr, Shared);
            return ShareAck;
        }
        return ShareAck;
    } else if (auto [found, s] = L2.find(addr); found && s != Invalid) {
        if (s == Modified) {
            L1d.update(addr, Shared);
            L2.update(addr, Shared);
            return Flush;
        } else if (s == Exclusive) {
            L1d.update(addr, Shared);
            L2.update(addr, Shared);
            return ShareAck;
        }
        return ShareAck;
    }

    return NullAck;
}

/**
 * cpu::snoop()
 *  Snoop a bus-side request. The request can be either a BusRd, BusRdX, or
 *  BusUpgr request coming from another processor.
 *
 * @addr: Address corresponding to the sender cpu's bus request
 * @brq: The specific bus request sent by the requesting cpu
 */
response cpu::snoop(void *addr, request brq)
{
    switch (brq) {
    case BusRd:
        return recvBusRd(addr);
    case BusRdX:
        return recvBusRdX(addr);
    case BusUpgr:
        return recvBusUpgr(addr);
    default:
        assert(0);
    }
}

/**
 * cpu::bcast()
 *  Broadcast a bus-side request to all processors connected to the shared
 *  bus. Collect all received acknowledgements to ensure that the correct
 *  state transition has been initiated.
 *
 * @addr: Address that the cpu is requesting a certain permission for over
 *        the bus
 * @brq: The specific bus request to be broadcasted to all other processors
 */
response cpu::bcast(void *addr, request brq) noexcept
{
    auto cpus = std::span{system::instance().getcpus().data(), ncpus};
    reponse r = 0u;

    std::ranges::for_each(cpus, [](const auto &proc) {
        if (&proc == this)
            continue;
        r |= proc.snoop(brq);
    });

    return r;
}

void cpu::load_data(void *addr) noexcept
{
    if (auto [found, s] = L1d.find(addr); found && s != Invalid) {
        L1d.stats.hits++;
        L1d.set_use(addr);
    } else if (auto [found, s] = L2.find(addr); found && s != Invalid) {
        L1d.stats.misses++;
        L2.stats.hits++;
        L2.set_use(addr);
        /* Load L2 cached block into L1 */
        L1d.update(addr, Shared);
    } else {
        cache &L3 = system::instance().access_L3();
        L1d.stats.misses++;
        L2.stats.misses++;
        
        if (auto [found, s] = L3.find(addr); found && s != Invalid) {
            L3.stats.hits++;
            L3.set_use(addr);
            
            if (s == Exclusive) {
                auto rsp = bcast(addr, BusRd);
                assert(rsp & ShareAck);
            } else if (s == Modified) {
                auto rsp = bcast(addr, BusRd);
                assert(rsp & Flush);
            }

            L2.update(addr, Shared);
            L1d.update(addr, Shared);
        } else {
            L3.stats.misses++;
            L3.update(addr, Exclusive);
            L2.update(addr, Exclusive);
            L1d.update(addr, Exclusive);
        }
    }
}

void cpu::store_data(void *addr) noexcept
{
    cache &L3 = system::instance().access_L3();

    if (auto [found, s] = L1d.find(addr); found && s != Invalid) {
        L1d.stats.hits++;
        
        if (s == Modified)
            L1d.set_use(addr);
        else {
            L1d.update(addr, Modified);
            L2.update(addr, Modified);
            L3.update(addr, Modified);
            if (s == Shared) {
                auto rsp = bcast(addr, BusUpgr);
                assert(rsp == (InvAck | NullAck));
            }
        }
    } else if (auto [found, s] = L2.find(addr); found && s != Invalid) {
        L1d.stats.misses++;
        L2.stats.hits++;

        if (s == Modified) {
            L1d.update(addr, Modified);
            L2.set_use(addr);
        } else {
            L1d.update(addr, Modified);
            L2.update(addr, Modified);
            L3.update(addr, Modified);
            if (s == Shared) {
                auto rsp = bcast(addr, BusUpgr);
                assert(rsp == (InvAck | NullAck));
            }
        }
    } else if (auto [found, s] = L3.find(addr); found && s != Invalid) {
        L1d.stats.misses++;
        L2.stats.misses++;
        L3.stats.hits++;

        if (s == Modified) {
            auto rsp = bcast(addr, BusRdX);
            assert(rsp & Flush);
        } else {

        }
    }
}

} // cachesim
