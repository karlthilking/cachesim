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

/**
 * cache::cache()
 *  Constructs a cpu cache with initialization parameters.
 *
 * @size: Total capacity of the cache
 * @block_size: Size of a single data block in the cache
 * @assoc: Associativity of the cache
 * @local: True if a shared cache (L3), false if private (L1, L2)
 */
cache::cache(size_t size, size_t block_size, u32 assoc, bool local) noexcept
    : lines(size / block_size),
      clock_hands(size / (block_size * assoc)),
      assoc(assoc), 
      local(local)
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
 *  Private cache (L1-L2):
 *   state = Invalid -> cache miss -> go down hierarchy
 *   state = Shared -> cache hit -> no bus transaction -> Shared
 *   state = Exclusive -> cache hit -> no bus transaction -> Exclusive
 *   state = Modified -> cache hit -> no bus transaction -> Modified
 *  Shared cache (L3):
 *   state = Invalid -> cache miss -> fetch from RAM
 *   state = Shared -> cache hit -> no bus transaction -> Shared
 *   state = Exclusive -> cache hit -> send BusRd -> Shared
 *   state = Modified -> cache miss -> send BusRd -> Flush -> Shared
 *
 * @addr: The memory address corresponding to the load
 */
std::tuple<bool, cache_state, ptrdiff_t> cache::load(void *addr) noexcept
{
    if (auto [found, state, loc] = find(addr); found) {
        /**
         * If data block is invalid or marked modifed at L3, then the
         * result of the read request is a miss. State M in L3 indicates
         * that another processor has a dirty copy in a private cache.
         */
        if (state == Invalid || (state == Modified && !local)) {
            stats.rd_missess++;
            return {false, state, loc};
        }
        
        lines[loc].u |= 1;
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
 *  Private cache (L1-L2):
 *   state = Invalid -> cache miss -> go down hierarchy
 *   state = Shared -> cache hit -> send BusUpgr -> Modified
 *   state = Exclusive -> cache hit -> no bus transaction -> Modified
 *   state = Modified -> cache hit -> no bus transaction -> Modified 
 *
 *  Shared cache (L3):
 *   state = Invalid -> cache miss -> fetch from RAM
 *   state = Shared -> cache hit -> send BusUpgr -> Modified
 *   state = Exclusive -> cache hit -> send BusUpgr -> Modified
 *   state = Modified -> cache miss -> send BusRdX -> Modified
 *
 * @addr: The memory address corresponding to the store
 */
std::tuple<bool, cache_state, ptrdiff_t> cache::store(void *addr) noexcept
{
    if (auto [found, state, loc] = find(addr); found) {
        if (state == Invalid || (state == Modified && !local)) {
            stats.wr_misses++;
            return {false, state, loc};
        }

        lines[loc].u |= 1;
        stats.wr_hits++;
        return {true, state, loc};
    }

    stats.wr_misses++;
    return {false, Invalid, -1};
}

/**
 * cache::elect()
 *  Elect a victim block in the set of the given address such that the
 *  victim block can be evicted and replaced if necessary. The index of
 *  the candidate for eviction is returned.
 *
 * @addr: Address specifying the set to identify a victim block in
 */
ptrdiff_t cache::elect(void *addr) noexcept
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
 *  will set to the given state. Returns true along with the address of the
 *  evicted line if it needed to be written back in order to allow for the
 *  next level of cache to potentially update the state of the data block.
 *
 * @index: Index to evict/replace in the cache
 * @addr: Address of load/store that necessitated an eviction
 * @state: State to set for the new data block replacing the victim block
 * 
 * return: Boolean indicating if the block was written back and the address
 *         of the data block that was evicted
 */
std::pair<evict_result, u64> cache::evict(ptrdiff_t loc, void *addr, 
                                          cache_state state) noexcept
{
    auto [index, tag] = decompose(addr);
    cache_line &victim = lines[loc];
    auto ret = std::pair<evict_result, u64>{
        None, ((tag << nr_ixbits) | index) << nr_offbits
    };
    
    if (victim.state == Modified && local) {
        ret.first = WriteBack;
        stats.write_backs++;
    } else if (!local) {
        dirent &ent = system::instance().access_dir().entries[loc];
        if (victim.state == Modified) {
            /**
             * If L3 block is marked as modified, then another processor
             * has a dirty copy which is more up-to-date than the L3 copy
             * so the private processor needs to write the data block
             * through to RAM, and invalidate it. Then the data block
             * can be safely evicted from L3.
             */
            ret.first = WriteThrough;
        } else if (ent.dirty) {
            /**
             * If the data block being evicted from L3 is dirty w.r.t main
             * memory and no processor has a more up-to-date private copy
             * of this block, then L3 can write the block back the RAM.
             * Any processors with a private copy of this block still
             * need to invalidate it.
             */
            ret.first = WriteBack;
            stats.write_backs++;
        }
        /**
         * Otherwise, if the block was not dirty at L3 w.r.t to main memory,
         * and no process has a dirty copy of the evicted data block in a
         * private cache, then the data block only needs to be invalidated
         * from other processors
         */
    }
    
    /**
     * Overwrite contents of evicted cache line with the replacement
     * data block's metadata and set reference bit
     */
    victim.tag = tag;
    victim.state = state;
    victim.u = 1;

    stats.evictions++;
    return ret;
}

/**
 * cache::insert()
 *  Insert a data block brought in from lower memory or from a cache-to-cache
 *  transfer. If there is an available invalid cache line, the invalid line
 *  can be replaced by the insterted line. Otherwise, an eviction needs to
 *  occur to insert the new cache line.
 *
 *  If an eviction is necessary to insert the line, and the eviction causes a
 *  write back, the return value is {true, victim_address} in case the next
 *  level of cache is able to change the state of its copy of the data block
 *  due to the write back.
 *   For e.g. if a modified block is written back from L2 to L3, then L3
 *   can change the state of its copy to Shared.
 *
 *  @addr: Address of the original load/store that requested this data block
 *  @state: The state to set for the newly inserted cache line
 *
 *  return: Boolean indicating if a block was evicted and written back,
 *          address of the evicted block (if one needed to be evicted), and
 *          location where the inserted block was placed. If no block was
 *          evicted false, 0, and the inserted location are returned.
 */
std::tuple<evict_result, u64, ptrdiff_t> cache::insert(void *addr, 
                                                       cache_state state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};
    auto loc = elect(addr);

    if (lines[loc].state != Invalid) {
        auto [res, evictaddr] = evict(loc, addr, state);
        return {res, evictaddr, loc};
    }
    
    lines[loc].tag = tag;
    lines[loc].state = state;
    lines[loc].u = 1;
    
    return {false, 0u, loc};
}

/**
 * cache::update()
 *  Update the state of a data block in the cache, assuming that the block
 *  corresponding to the given address is present.
 *
 * @addr: Address corresponding to the data block to update in the cache
 * @state: New state to set for the data block
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
 * cpu::cpu()
 *  Constructs a cpu with a unique identifier and two levels of private cache
 *  where the first level cache is split between instruction and data, and
 *  the second level private cache is unified.
 */
cpu::cpu(u32 id) noexcept
    : L1d(l1d_size, l1d_blk_size, l1d_assoc, true),
      L1i(l1i_size, l1i_blk_size, l1i_assoc, true),
      L2(l2_size, l2_blk_size, l2_assoc, true), 
      id(id)
{}

/**
 * cpu::recvBusUpgr()
 *  Respond to a BusUpgr request snooped on the bus. If the line is present
 *  in one of this cpu's private caches, invalidate the block and respond
 *  with InvAck. Otherwise if the block is not present, respond with NullAck
 *  to signify that the block is not present in any of this cpu's caches.
 *
 * A processor snooping a BusUpgr request implies that it either does not
 * have the block at all, or it has it in the shared state, otherwise, if
 * it had the block in the exlusive or modified state, the requesting
 * processor would have sent a BusRdX (no other processor can have the block
 * if this processor has it in state M or E). Therefore, the state transition
 * is either S -> I or I -> I (i.e no change).
 *
 * @addr: Address corresponding to the data block the sending cpu wishes to
 *        acquire write permission for
 */
response cpu::recvBusUpgr(void *addr) noexcept
{
    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        assert(s == Shared);
        L1d.lines[loc].state = Invalid;
        L2.update(addr, Invalid);
        return InvAck;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        assert(s == Shared);
        L2.lines[loc].state = Invalid;
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
    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        L1d.lines[loc].state = Invalid;
        L2.update(addr, Invalid);
        /**
         * If a private copy of the data block that the requesting processor
         * wishes to acquire read/write access for is in the modified state,
         * the owning processor (state = M) needs to write back the data
         * to the shared cache and signal that the data has been flushed.
         */
        if (s == Modified) {
            /* Write back to L3 */
            L1d.stats.write_backs++;
            L2.stats.write_backs++;
            
            auto [expect, L3_state, L3_loc] = L3.find(addr);
            assert(expect && L3_state == Modified);
        
            /* Mark that L3 copy is dirty */
            dirent &ent = system::instance().access_dir().entries[L3_loc];
            assert(ent.valid);
            ent.dirty = 1;

            /**
             * Requesting processor will acquire the flushed data block in
             * state M, so the processor which flushed the block will no
             * longer have the block present. Therefore, this cpu should
             * unmark the present bit in the directory entry's bitmap.
             */
            assert(ent.bitamp & (1ull << id));
            ent.bitmap ^= (1ull << id);

            return Flush;
        }
        return InvAck;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        L2.lines[loc].state = Invalid;
        if (s == Modified) {
            L2.stats.write_backs++;

            auto [expect, L3_state, L3_loc] = L3.find(addr);
            assert(expect && L3_state == Modified);
            
            /**
             * After writing back modified data block to L3, the L3 copy
             * is dirty (does not reflect main memory until then block is
             * written back to RAM)
             */
            dirent &ent = system::instance().access_dir().entries[L3_loc];
            assert(ent.valid);
            ent.dirty = 1;
            
            assert(ent.bitmap & (1ull << id));
            ent.bitmap ^= (1ull << id);

            return Flush;
        }
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
    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        if (s == Modified || s == Exclusive) {
            L1d.lines[loc].state = Shared;
            L2.update(addr, Shared);
            if (s == Modified) {
                L1d.stats.write_backs++;
                L2.stats.write_backs++;
                
                auto [expect, L3_state, L3_loc] = L3.find(addr);
                assert(expect && L3_state == Modified);

                dirent &ent = system::instance().access_dir().entries[L3_loc];
                assert(ent.valid);
                ent.dirty = 1;
                
                assert(ent.bitmap & (1ull << id));
                ent.bitmap ^= (1ull << id);

                return Flush;
            }
        }
        return ShareAck;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        if (s == Modified || s == Exclusive) {
            L2.lines[loc].state = Shared;
            if (s == Modified) {
                L2.write_backs++;

                auto [expect, L3_state, L3_loc] = L3.find(addr);
                assert(expect && L3_state == Modified);

                dirent &ent = system::instance().access_dir().entries[L3_loc];
                assert(ent.valid);
                ent.dirty = 1;
                
                assert(ent.bitmap & (1ull << id));
                ent.bitmap ^= (1ull << id);

                return Flush;
            }
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
        assert(false);
    }
}

/**
 * cpu::ptp()
 *  Send point-to-point messages to processors with a private copy of a
 *  data block that needs to be invalidated, shared, flushed, etc.
 *
 * @addr: Address that the cpu is requesting r/w access for
 * @bitmap: Bitmap indicating which processors have the block present
 * @brq: The bus request to be broadcasted to processors with the data block
 */
response cpu::ptp(void *addr, u32 bitmap, request brq)
{
    response rsp = 0u;
    system &sys = system::instance();
    
    sys.acquire_bus();
    auto procs = std::span{sys.access_cpus().data(), ncpus};

    for (auto i = 0u; i < ncpus; i++, bitmap >>= 1) {
        if (i != id && (bitmap & 1))
            rsp |= procs[i].snoop(brq);
    }

    sys.release_bus();
    return rsp;
}

/**
 * cpu::bcast()
 *  Broadcast a bus-side request to all processors connected to the shared
 *  bus. Collect all received acknowledgements to ensure that the correct
 *  state transition has been initiated.
 *
 * @addr: Address that the cpu is requesting r/w permission for
 * @brq: The specific bus request to be broadcasted to all other processors
 */
response cpu::bcast(void *addr, request brq) noexcept
{
    response rsp = 0u;

    /* Acquire memory bus to initiate transaction */
    system::instance().acquire_bus();
    auto procs = std::span{system::instance().access_cpus().data(), ncpus};
    
    std::ranges::for_each(procs, [](const auto &proc) {
        if (&proc == this)
            continue;
        rsp |= proc.snoop(brq);
    });

    system::instance().release_bus();
    return rsp;
}

void cpu::load_data(void *addr) noexcept
{
    ptrdiff_t L1d_loc = -1, L2_loc = -1;

    do {
        if (auto [found, s, loc] = L1d.load(addr); found) {
            /* If not invalid, then cache hit from L1d */
            if (s != Invalid)
                break;
            /* 
             * If tag matches but the state is invalid, remember location 
             * of the block in order to update with copy from lower memory
             */
            L1d_loc = loc;
        }
        
        if (auto [found, s, loc] = L2.load(addr); found) {
            /* Hit in L2, now bring data block into L1d */
            if (s != Invalid && L1d_loc >= 0) {
                L1d.update(L1d_loc, s, true);
                break;
            } else if (s != Invalid) {
                L1d.insert(addr, s);
                break;
            }
            L2_loc = loc;
        }

        if (auto [found, s, loc] = L3.load(addr); found) {
            cache_state transition = Shared;
            directory &dir = system::instance().access_dir();
            dirent &ent = dir.entries[loc];
            
            /**
             * If state is exclusive or modified, a bus request must be sent
             * either through point-to-point communication or a broadcast
             * to all processors.
             *
             * E -> BusRd -> ShareAck -> S
             * M -> BusRd -> Flush -> S
             */
            if (s == Exclusive)
                assert(ptp(addr, ent.bitmap, BusRd) == ShareAck);
            else if (s == Modified)
                assert(ptp(addr, ent.bitmap, BusRd) == Flush);
            else if (s == Invalid) {
                /**
                 * If L3 entry is invalid (L3 miss), then bring block into L3
                 * in exclusive state as the processor who sent to PrRd 
                 * request will have the only copy of this data block.
                 * Also, mark the directory entry for this block as valid
                 * and not dirty (it reflects main memory).
                 */
                transition = Exclusive;
                ent.valid = 1;
                ent.dirty = 0;
                L3.update(loc, Exclusive, true);
            }

            if (L2_loc >= 0)
                L2.update(L2_loc, transition, true);
            else if (auto [res, victim, _] = L2.insert(addr, transition); res) {
                auto [expect, _, evictloc] = L3.find(victim);
                assert(expect);
                /**
                 * If populating L2 with the requested data block caused a
                 * write back, the data block written back to L3 should be
                 * marked dirty (w.r.t. main memory). However, this block
                 * was modified (implies exclusivity), so no other processor
                 * should have it present.
                 */
                dir.entries[evictloc].dirty |= 1;
                dir.entries[evictloc].bitmap ^= (1ull << id);
                assert(!dir.entries[evictloc].bitmap);

                /**
                 * Mark entry that was written back to L3 as shareable as
                 * no other processor should have the block and it is
                 * consistent from the view of each processor as L3 is
                 * shared.
                 */
                L3.lines[evictloc].state = Shared;
            }

            if (L1d_loc >= 0)
                L1d.update(L1d_loc, transition, true);
            else
                L1d.insert(addr, transition);
            
            ent.bitmap |= (1ull << id);
        } else /* Not present in L3 */ {
            directory &dir = system::instance().access_dir();
            
            /**
             * If inserting into L3 causes 
             */
            if (auto [wb, victim] = L3.insert(addr); victim > 0u) {

            }

            if (L2_loc >= 0)
                L2.update(L2_loc, Exclusive, true);
            else if (auto [wb, victim] = L2.insert(addr); wb) {
                auto [expect, _, evictloc] = L3.find(addr);
                assert(expect);
                
                assert(dir.entries[evictloc].valid);
                dir.entries[evictloc].dirty |= 1;
                dir.entries[evictloc].bitmap ^= (1ull << id);
                assert(!dir.entries[evictloc].bitmap);

                L3.lines[evictloc].state = Shared;
            }

            if (L1d_loc >= 0)
                L1d.update(L1d_loc, Exclusive, true);
            else
                L1d.insert(addr, Exclusive);

            auto [expect, _, loc] = L3.find(addr);
            assert(expect);

            dirent &ent = dir.entries[loc];
            ent.bitmap |= (1ull << id);
            ent.dirty = 0;
            ent.valid = 1;
        }
    } while (0);
}

void cpu::store_data(void *addr) noexcept
{
    ptrdiff_t L1d_loc = -1, L2_loc = -1;

    do {
        
    } while (0);
}

void cpu::load_instr(void *addr) noexcept
{
    do {
        if (auto [found, s, loc] = L1i.load(addr); found) {

        }

        if (auto [found, s, loc] = L2.load(addr); found) {

        }
    } while (0);
}

directory::directory(size_t size, size_t block_size, u32 assoc) noexcept
    : entries(size / block_size), assoc(assoc)
{
    auto num_sets = size / (block_size * assoc);

    nr_offbits = std::popcount(block_size - 1);
    nr_ixbits = std::popcount(num_sets - 1);
    set_mask = (num_sets - 1) << nr_offbits;
}

system::system() noexcept
{
    auto id = 0u;
    std::generate(begin(cpus), end(cpus), [&]{ return cpu(id++); });
}
} // cachesim
