#include <bit>
#include <bitset>
#include <span>
#include <algorithm>
#include <iterator>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sched.h>
#include <unistd.h>
#include "../include/params.hpp"
#include "../include/cache.hpp"

namespace cachesim {
/**
 * cache::decompose()
 *  Extract the index and tag of a memory address given the parameters of
 *  the cache.
 *
 * @addr: The memory address to decompose into {index, tag}
 */
std::pair<u64, u64> cache::decompose(void *addr) const noexcept
{
    auto index = (reinterpret_cast<u64>(addr) & set_mask) >> n_offbits;
    auto tag = reinterpret_cast<u64>(addr) >> (n_ixbits + n_offbits);

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
cache::cache(size_t sz, size_t blk_sz, u32 assoc, cache_type ty) noexcept
    : lines(sz / blk_sz)
    , clock_hands(sz / (blk_sz * assoc))
    , assoc(assoc)
    , type(ty)
{
    auto num_sets = sz / (blk_sz * assoc);
    n_offbits = std::popcount(blk_sz - 1);
    n_ixbits = std::popcount(num_sets - 1);
    set_mask = (num_sets - 1) << n_offbits;
}

cache::cache(cache &&other) noexcept
    : lines(std::move(other.lines))
    , clock_hands(std::move(other.clock_hands))
    , stats(other.stats)
    , set_mask(other.set_mask)
    , assoc(other.assoc)
    , n_offbits(other.n_offbits)
    , n_ixbits(other.n_ixbits)
    , type(other.type)
{}

cache &cache::operator=(cache &&other) noexcept
{
    if (this != &other) {
        lines = std::move(other.lines);
        clock_hands = std::move(other.clock_hands);
        stats = other.stats;
        set_mask = other.set_mask;
        assoc = other.assoc;
        n_offbits = other.n_offbits;
        n_ixbits = other.n_ixbits;
        type = other.type;
    }
    return *this;
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

    auto it = std::ranges::find_if(set, [tag](const auto &line) {
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

    auto it = std::ranges::find_if(set, [tag](const auto &line) { 
        return line.tag == tag;
    });

    if (it != set.end())
        return {true, it->state, ptrdiff_t{&*it - lines.data()}};

    return {false, Invalid, ptrdiff_t{-1}};
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
    if (auto [found, s, loc] = find(addr); found) {
        /**
         * If data block is invalid or marked modifed at L3, then the
         * result of the read request is a miss. State M in L3 indicates
         * that another processor has a dirty copy in a private cache.
         */
        if (s == Invalid || (s == Modified && type == SharedCache)) {
            stats.rd_misses++;
            return {false, s, loc};
        }
        
        lines[loc].use |= 1;
        stats.rd_hits++;
        return {true, s, loc};
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
    if (auto [found, s, loc] = find(addr); found) {
        if (s == Invalid || (s == Modified && type == SharedCache)) {
            stats.wr_misses++;
            return {false, s, loc};
        }

        lines[loc].use |= 1;
        stats.wr_hits++;
        return {true, s, loc};
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
        return ptrdiff_t{&*it - lines.data()};

    for (auto i = clock_hands[index];; i = (i + 1) % set.size()) {
        if (set[i].use) {
            set[i].use = 0;
            continue;
        }
        clock_hands[index] = i;
        return ptrdiff_t{static_cast<i32>((index * assoc) + i)};
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
        Evict, ((victim.tag << n_ixbits) | index) << n_offbits
    };
    assert((static_cast<u32>(loc) / assoc) == index);
    
    if (victim.state == Modified && type != SharedCache) {
        ret.first = WriteBack;
        stats.write_backs++;
    } else if (type == SharedCache) {
        dirent &ent = system::instance().access_dir().entries[loc];
        if (victim.state == Modified) {
            assert(ent.bitmap);
            /**
             * If L3 block is marked as modified, then another processor
             * has a dirty copy which is more up-to-date than the L3 copy
             * so the private processor needs to write the data block
             * through to RAM, and invalidate it. Then the data block
             * can be safely evicted from L3.
             */
            ret.first = WriteThrough;
            /**
             * Return early and do not yet evict the block from L3. The
             * processor with a private copy of this block needs to write
             * the block through to L3 and then from there it may be
             * safely evicted from L3 and written back to main memory.
             */
            return ret;
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
    victim.use = 1;

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
std::tuple<evict_result, u64, ptrdiff_t> 
cache::insert(void *addr, cache_state state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto loc = elect(addr);

    if (lines[loc].state != Invalid) {
        auto [res, evictaddr] = evict(loc, addr, state);
        return {res, evictaddr, loc};
    }
    
    lines[loc].tag = tag;
    lines[loc].state = state;
    lines[loc].use = 1;
    
    return {NoEvict, 0u, loc};
}

/**
 * cache::update()
 *  Update the state of a data block in the cache at a given location.
 *
 * @loc: Location of the block in the cache
 * @state: New state to set for the data block
 * @use: Whether or not to set the use bit for the updated block
 */
void cache::update(ptrdiff_t loc, cache_state state, bool use) noexcept
{
    lines[loc].state = state;
    if (use)
        lines[loc].use |= 1;
}

/**
 * cache::update()
 *  Update the state of a data block in the cache, assuming that the block
 *  corresponding to the given address is present.
 *
 * @addr: Address corresponding to the data block to update in the cache
 * @state: New state to set for the data block
 * @use: Whether or not to set the use bit for the updated block
 */
void cache::update(void *addr, cache_state state, bool use) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [tag](const auto &line) {
        return line.tag == tag;
    });
    assert(it != set.end());

    it->state = state;
    if (use)
        it->use |= 1;
}

cpu::cpu() noexcept
    : L1d(l1d_size, l1d_blk_size, l1d_assoc, PrivateCache)
    , L1i(l1i_size, l1i_blk_size, l1i_assoc, PrivateCache)
    , L2(l2_size, l2_blk_size, l2_assoc, BoundaryCache)
    , id(0u)
{}

cpu::cpu(u32 id) noexcept
    : L1d(l1d_size, l1d_blk_size, l1d_assoc, PrivateCache)
    , L1i(l1i_size, l1i_blk_size, l1i_assoc, PrivateCache)
    , L2(l2_size, l2_blk_size, l2_assoc, BoundaryCache)
    , id(id)
{}

cpu::cpu(cpu &&other) noexcept
    : L1d(std::move(other.L1d))
    , L1i(std::move(other.L1i))
    , L2(std::move(other.L2))
    , id(other.id)
{}

cpu &cpu::operator=(cpu &&other) noexcept
{
    if (this != &other) {
        L1d = std::move(other.L1d);
        L1i = std::move(other.L1i);
        L2 = std::move(other.L2);
        id = other.id;
    }
    return *this;
}

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
        L2.update(addr, Invalid, false);
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
    // cache &L3 = system::instance().access_L3();
    // auto [_, L3_state, L3_loc] = L3.find(addr);
    // dirent &ent = system::instance().access_dir().entries[L3_loc];

    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        L1d.lines[loc].state = Invalid;
        L2.update(addr, Invalid, false);
        /**
         * If a private copy of the data block that the requesting processor
         * wishes to acquire read/write access for is in the modified state,
         * the owning processor (state = M) needs to write back the data
         * to the shared cache and signal that the data has been flushed.
         */
        if (s == Modified) {
            // assert(L3_state == Modified);

            /* Write back to L3 */
            L1d.stats.write_backs++;
            L2.stats.write_backs++;
        
            /* Mark that L3 copy is dirty */
            // assert(ent.valid);
            // ent.dirty = 1;
            // 
            // assert(ent.bitmap & (1ull << id));
            // ent.bitmap ^= (1ull << id);

            return Flush;
        }
        /**
         * Because this processor invalidated their private copies of the
         * data block requesting over the bus, the present bit in the
         * bitmap of this block should be turned off.
         */
        // assert(ent.bitmap & (1ull << id));
        // ent.bitmap ^= (1ull << id);

        return InvAck;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        L2.lines[loc].state = Invalid;
        if (s == Modified) {
            // assert(L3_state == Modified);
            L2.stats.write_backs++;
            /**
             * After writing back modified data block to L3, the L3 copy
             * is dirty (does not reflect main memory until then block is
             * written back to RAM)
             */
            // assert(ent.valid);
            // ent.dirty = 1;

            // assert(ent.bitmap & (1ull << id));
            // ent.bitmap ^= (1ull << id);
            
            return Flush;
        }
        // assert(ent.bitmap & (1ull << id));
        // ent.bitmap ^= (1ull << id);

        return InvAck;
    }
    
    // assert(!(ent.bitmap & (1ull << id)));
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
    // cache &L3 = system::instance().access_L3();

    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        if (s == Modified || s == Exclusive) {
            L1d.lines[loc].state = Shared;
            L2.update(addr, Shared, false);
            if (s == Modified) {
                L1d.stats.write_backs++;
                L2.stats.write_backs++;
                
                return Flush;
            }
        }
        return ShareAck;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        if (s == Modified || s == Exclusive) {
            L2.lines[loc].state = Shared;
            if (s == Modified) {
                L2.stats.write_backs++;

                // auto [expect, L3_state, L3_loc] = L3.find(addr);
                // assert(expect && L3_state == Modified);

                // dirent &ent = system::instance().access_dir().entries[L3_loc];
                // assert(ent.valid);
                // ent.dirty = 1;

                return Flush;
            }
        }
        return ShareAck;
    }

    return NullAck;
}

/**
 * cpu::recvBusInv()
 *  Snoop a request to unconditionally invalidate private copies of the
 *  data block corresponding to the address associated with the bus request.
 *
 * @addr: Address of the data block to evict
 * return: Invalidation acknowledgement if the block is found and was
 *         invalidated. Otherwise, if the block is not found, a Null
 *         acknowledgement is returned. If communication is point-to-point
 *         using a directory, the response should always be InvAck.
 */
response cpu::recvBusInv(void *addr) noexcept
{
    // cache &L3 = system::instance().access_L3();
    // auto [_, L3_state, L3_loc] = L3.find(addr);
    // dirent &ent = system::instance().access_dir().entries[L3_loc];

    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        L1d.lines[loc].state = Invalid;
        L2.update(addr, Invalid, false);
        // ent.bitmap ^= (1ull << id);
        return InvAck;
    } else if (auto [found, s, loc] = L1i.find(addr); found && s != Invalid) {
        L1i.lines[loc].state = Invalid;
        L2.update(addr, Invalid, false);
        // ent.bitmap ^= (1ull << id);
        return InvAck;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        L2.lines[loc].state = Invalid;
        // ent.bitmap ^= (1ull << id);
        return InvAck;
    }

    return NullAck;
}

/**
 * cpu::recvBusFlush()
 *  Respond to request to flush a data block that is dirty in a private
 *  cache by writing the data block through to L3 and indicating the
 *  the owning processor no longer has the block present. Because the
 *  block is written through to L3, it is now dirty with respect to
 *  main memory (RAM).
 *
 *  @addr: Address associated with the block to be flushed
 *  return: Respond with Flush if the block is present and subsequently
 *          flushed. Otherwise, if the block is not present, respond
 *          with NullAck to indicate that no such copy is present in
 *          this processor's private cache.
 */
response cpu::recvBusFlush(void *addr) noexcept
{
    // cache &L3 = system::instance().access_L3();

    if (auto [found, s, loc] = L1d.find(addr); found && s != Invalid) {
        /**
         * If receiving a bus request to flush a data block, it is assumed
         * that the block is only present for one processor, the processor
         * who owns the dirty copy.
         */
        assert(s == Modified);
        
        /* Mark the block as invalid */
        L1d.lines[loc].state = Invalid;
        L2.update(addr, Invalid, false);
        
        /* Write through to L3 */
        L1d.stats.write_backs++;
        L2.stats.write_backs++;

        // auto [expect, L3_state, L3_loc] = L3.find(addr);
        // assert(expect && L3_state == Modified);

        // dirent &ent = system::instance().access_dir().entries[L3_loc];
        // assert(ent.valid);

        /**
         * Mark that the block written back to L3 is dirty and that
         * the processor that received the Flush request no longer
         * has the block present. Also mark the block as Shared at L3,
         * it is no longer dirty in a private cache.
         */
        // ent.dirty = 1;
        // ent.bitmap ^= (1ull << id);
        // assert(!ent.bitmap);
        // L3.update(L3_loc, Shared, false);

        return Flush;
    } else if (auto [found, s, loc] = L2.find(addr); found && s != Invalid) {
        assert(s == Modified);

        L2.lines[loc].state = Invalid;
        L2.stats.write_backs++;

        // auto [expect, L3_state, L3_loc] = L3.find(addr);
        // assert(expect && L3_state == Modified);

        // dirent &ent = system::instance().access_dir().entries[L3_loc];
        // assert(ent.valid);

        // ent.dirty = 1;
        // ent.bitmap ^= (1ull << id);
        // assert(!ent.bitmap);
        // L3.update(L3_loc, Shared, false);

        return Flush;
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
response cpu::snoop(void *addr, request brq) noexcept
{
    switch (brq) {
    case BusRd:
        return recvBusRd(addr);
    case BusRdX:
        return recvBusRdX(addr);
    case BusUpgr:
        return recvBusUpgr(addr);
    case BusInv:
        return recvBusInv(addr);
    case BusFlush:
        return recvBusFlush(addr);
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
response cpu::ptp(void *addr, u32 bitmap, request brq) noexcept
{
    response rsp = 0u;
    system &sys = system::instance();
    
    sys.initiate_transaction();
    for (auto i = 0u; i < ncpus; i++, bitmap >>= 1) {
        if (i != id && (bitmap & 1)) {
            cpu &receiver = sys.access_cpu(i);
            rsp |= receiver.snoop(addr, brq);
        }
    }

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
    system &sys = system::instance();
    
    sys.initiate_transaction();
    for (auto i = 0u; i < ncpus; i++) {
        if (i != id) {
            cpu &receiver = sys.access_cpu(i);
            rsp |= receiver.snoop(addr, brq);
        }
    }

    return rsp;
}

/**
 * cpu::insert()
 *  Insert a new data block into the specified cache, handling the 
 *  different eviction, write back, or flush events to can occur
 *  due to bringing in the new block.
 *
 *  Insertion assumes that the function is used for bringing a new data
 *  block into the cache, so the reference bit is set after insertion.
 *
 * @c: The cache to insert a new data block into
 * @loc: Location of an invalid block in the cache that be resused for
 *       insertion. Set to -1 if no such location exists.
 * @addr: The address of the new data block being brought in
 * @state: The state to set for the new data block
 */
void cpu::insert(cache &c, ptrdiff_t loc, 
                 void *addr, cache_state state) noexcept
{
    if (loc >= 0) {
        c.update(loc, state, true);
        
        if (c.type == SharedCache) {
            /* If L3 (shared), initialize directory entry for new block */
            dirent &ent = system::instance().access_dir().entries[loc];
            cache &L3 = system::instance().access_L3();
            cache_line &line = L3.lines[loc];

            auto [index, tag] = L3.decompose(addr);
            assert(line.tag == tag && line.state == state);

            ent.valid = 1u;
            ent.dirty = 0u;
            ent.bitmap = 0u;
        } else if (c.type == BoundaryCache) {
            cache &L3 = system::instance().access_L3();
            /**
             * If boundary (L2, i.e. last level of private cache) the
             * directory entry's bitmap should mark that this cpu
             * now has a copy of the block in a private cache.
             */
            auto [found, L3_state, L3_loc] = L3.find(addr);
            assert(found && L3_state == state);

            cache_line &line = L3.lines[L3_loc];
            auto [index, tag] = L3.decompose(addr);
            assert(line.tag == tag && line.state == state);

            dirent &ent = system::instance().access_dir().entries[L3_loc];
            assert(ent.valid && !(ent.bitmap & (1ull << id)));
            ent.bitmap |= (1ull << id);
        }
    } else if (c.type == PrivateCache) {
        /**
         * For a private cache that is not at the coherence boundary,
         * i.e. L1, a block being evicted can be safely ignored, as
         * the state of the block is unchanged and the block is still
         * present in a processor-private cache (L2).
         */
        c.insert(addr, state);
    } else if (c.type == BoundaryCache) {
        cache &L3 = system::instance().access_L3();
        if (auto [res, victim, _] = c.insert(addr, state); res != NoEvict) {
            void *evictaddr = reinterpret_cast<void *>(victim);
            auto [expect, _null, evictloc] = L3.find(evictaddr);
            assert(expect && res != WriteThrough);

            /**
             * Because block was evicted from L2, it must also be 
             * invalidated in L1, if present, in order to maintain 
             * coherence and inclusivity.
             */
            if (auto [found, s, L1d_loc] = L1d.find(evictaddr); 
                found && s != Invalid) {
                L1d.update(L1d_loc, Invalid, false);
            } else if (auto [found, s, L1i_loc] = L1i.find(evictaddr);
                       found && s != Invalid) {
                L1i.update(L1i_loc, Invalid, false);
            }
            
            directory &dir = system::instance().access_dir();
            dirent &evictent = dir.entries[evictloc];
            /**
             * Mark that this processor no longer has a copy of the
             * evicted cache line is one of its private caches
             */
            assert(evictent.bitmap & (1ull << id));
            evictent.bitmap ^= (1ull << id);
            cache_line &evictline = L3.lines[evictloc];
            /**
             * If written back from L2 to L3, mark that the copy of the
             * data block now in L3 is dirty; it does not reflect what
             * is in main memory
             */
            if (res == WriteBack) {
                assert(evictline.state == Modified);
                evictline.state = Shared;
                evictent.dirty |= 1;
            } else if (evictline.state == Exclusive)
                evictline.state = Shared;
        }
        
        auto [expect, s, L3_loc] = L3.find(addr);
        assert(expect && s == state);
        
        dirent &ent = system::instance().access_dir().entries[L3_loc];
        assert(ent.valid);
        
        /**
         * Because the requested data block is coming from L3 into
         * L2, mark the directory entry for this cache line indicating
         * that the current processor now has a private copy.
         */
        ent.bitmap |= (1ull << id);
    } else if (c.type == SharedCache) {
        cache &L3 = system::instance().access_L3();
        directory &dir = system::instance().access_dir();
        auto [res, victim, evictloc] = c.insert(addr, state);
        
        /**
         * If the blocked evicted from L3 was in state M, then another
         * processor has the most recent copy of this data block in one
         * of its private caches. The owner must write the block through
         * to L3 from where it can then be written back to main memory
         * and safely evicted. The owner must also invalidate their copy.
         *
         * Alternatively, if the block in L3 is up-to-date (no processor
         * has a more recent copy in a private cache), then the evicted
         * data block only needs to be invalidated by any processors with
         * a private copy in order to maintain inclusivity.
         */
        if (res == WriteBack || res == Evict) {
            void *evictaddr = reinterpret_cast<void *>(victim);
            dirent &evictent = dir.entries[evictloc];
            
            /* Invalidate evicted block in private caches */
            if (evictent.bitmap & (1ull << id)) {
                if (auto [found, s, L1d_loc] = L1d.find(evictaddr);
                    found && s != Invalid) {
                    L1d.lines[L1d_loc].state = Invalid;
                    L2.update(evictaddr, Invalid, false);
                } else if (auto [found, s, L1i_loc] = L1i.find(evictaddr);
                           found && s != Invalid) {
                    L1i.lines[L1i_loc].state = Invalid;
                    L2.update(evictaddr, Invalid, false);
                } else if (auto [found, s, L2_loc] = L2.find(evictaddr);
                           found && s != Invalid) {
                    L2.lines[L2_loc].state = Invalid;
                }
                evictent.bitmap ^= (1ull << id);
            }
            
            /* Request other cpus with this block to invalidate */
            if (evictent.bitmap) {
                assert(ptp(evictaddr, evictent.bitmap, BusInv) == InvAck);
                evictent.bitmap = 0u;
            }
        } else if (res == WriteThrough) {
            /**
             * Send request on bus for the processor with the private
             * and most recent copy of the data block that should be
             * evicted. Once the write through to L3 completes, the
             * block is evicted from L3 and its contents are written
             * back to main memory.
             */
            void *evictaddr = reinterpret_cast<void *>(victim);
            dirent &evictent = dir.entries[evictloc];
            assert(std::popcount(evictent.bitmap) == 1);

            if (evictent.bitmap == (1ull << id)) {
                /**
                 * Current processor is the owner of the data block that
                 * will be evicted L3, so they should write back their
                 * copies to L3 and mark them invalid.
                 */
                if (auto [found, s, L1d_loc] = L1d.find(evictaddr); 
                    found && s != Invalid) {
                    L1d.lines[L1d_loc].state = Invalid;
                    L2.update(evictaddr, Invalid, false);
                    L1d.stats.write_backs++;
                    L2.stats.write_backs++;
                } else if (auto [found, s, L2_loc] = L2.find(evictaddr);
                           found && s != Invalid) {
                    L2.lines[L2_loc].state = Invalid;
                    L2.stats.write_backs++;
                } else {
                    std::cerr << __LINE__ << ", " << __func__ << ": "
                              << "bitmap marked cpu " << id << " as present"
                              << ", but no private data block was found\n";
                    std::abort();
                }
                evictent.dirty |= 1;
                evictent.bitmap ^= (1ull << id);
            } else {
                /**
                 * Another processor has the modified cache line; send a
                 * point-to-point bus request
                 */
                assert(evictent.bitmap);
                assert(ptp(evictaddr, evictent.bitmap, BusFlush) == Flush);
                evictent.dirty |= 1;
                evictent.bitmap = 0u;
            }
            
            /**
             * After having processor with private modified copy write
             * back to L3, commit the eviction by writing back the
             * data block flushed to L3 to main memory
             */
            L3.lines[evictloc].state = Invalid;
            auto [commit, _] = L3.evict(evictloc, addr, state);
            assert(commit == WriteBack);
        }
        
        /**
         * For a new cache line brought into L3 from RAM, its bitmap
         * should be zeroed (no processor has a private copy yet),
         * its valid bit should be set and it is not dirty.
         */
        dirent &ent = dir.entries[evictloc];
        ent.valid = 1u;
        ent.dirty = 0u;
        ent.bitmap = 0u;
    }
}

/**
 * cpu::load_data()
 *  Perform load data address, searching throughout memory hierarchy.
 *  Evictions, bus transactions, and state transitions are handled
 *  as necessary either directly or by called functions.
 *
 * @addr: Address of data to load
 */
void cpu::load_data(void *addr) noexcept
{
    ptrdiff_t L1d_loc = -1, L2_loc = -1;

    do {
        if (auto [hit, s, loc] = L1d.load(addr); loc != -1) {
            /* If cache hit, return */
            if (hit)
                break;
            /* 
             * If tag matches but the state is invalid, remember location 
             * of the block in order to update with copy from lower memory
             */
            L1d_loc = loc;
        }
        
        if (auto [hit, s, loc] = L2.load(addr); loc != -1) {
            /* Hit in L2, now bring data block into L1d */
            if (hit) {
                insert(L1d, L1d_loc, addr, s);
                break;
            }
            L2_loc = loc;
        }
        
        cache &L3 = system::instance().access_L3();
        if (auto [hit, s, loc] = L3.load(addr); loc != -1) {
            cache_state transition = (s == Invalid) ? Exclusive : Shared;
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
            if (s == Exclusive) {
                assert(ent.valid && std::popcount(ent.bitmap) == 1);
                assert(ptp(addr, ent.bitmap, BusRd) == ShareAck);

                L3.lines[loc].state = Shared;
                L3.lines[loc].use = 1;
            } else if (s == Modified) {
                assert(ent.valid && std::popcount(ent.bitmap) == 1);
                assert(ptp(addr, ent.bitmap, BusRd) == Flush);
                /**
                 * L3 line is dirty because flush only went to L3, so
                 * main memory is still not up-to-date
                 */
                ent.dirty |= 1;
                L3.lines[loc].state = Shared;
                L3.lines[loc].use = 1;
            } else if (s == Invalid) {
                assert(!hit);
                /**
                 * If L3 entry is invalid (L3 miss), then bring block into L3
                 * in exclusive state as the processor who sent to PrRd 
                 * request will have the only copy of this data block.
                 */
                insert(L3, loc, addr, transition);
            }
            insert(L2, L2_loc, addr, transition);
            insert(L1d, L1d_loc, addr, transition);
        } else /* Not present in L3 */ {
            insert(L3, -1, addr, Exclusive);
            insert(L2, L2_loc, addr, Exclusive);
            insert(L1d, L1d_loc, addr, Exclusive);
        }
    } while (0);

    cache &L3 = system::instance().access_L3();
    directory &dir = system::instance().access_dir();

    auto [f, s, l] = L3.find(addr);
    assert(f && s != Invalid && l != -1);
    dirent &ent = dir.entries[l];
    assert(ent.valid && (ent.bitmap & (1ull << id)));
}

void cpu::store_data(void *addr) noexcept
{
    ptrdiff_t L1d_loc = -1, L2_loc = -1;

    do {
        if (auto [hit, s, loc] = L1d.store(addr); loc != -1) {
            if (hit && s == Modified) {
                /**
                 * No state transition or bus transaction is necessary
                 * if the block is already marked as modified
                 */
                break;
            } else if (s == Exclusive || s == Shared) {
                /**
                 * If state is exclusive, a bus transaction is not
                 * necessary, but the state must transition to modified.
                 *
                 * If state is shared, a BusUpgr is necessary to inform
                 * other processors to invalidate their copies. Then,
                 * the state will transition to modified.
                 */
                cache &L3 = system::instance().access_L3();
                directory &dir = system::instance().access_dir();

                auto [found, L3_state, L3_loc] = L3.find(addr);
                assert(found && s == L3_state);

                dirent &ent = dir.entries[L3_loc];
                assert(ent.valid && (ent.bitmap & (1ull << id)));
                if (s == Shared && ent.bitmap != (1ull << id)) {
                    assert(ptp(addr, ent.bitmap, BusUpgr) == InvAck);
                    ent.bitmap &= (1ull << id);
                }
                
                /* Mark data block as modified in all cache levels */
                L3.lines[L3_loc].state = Modified;
                L2.update(addr, Modified, false);
                L1d.lines[loc].state = Modified;
                
                /**
                 * Assert only this processor has the block present now
                 * because it is modified
                 */
                assert(ent.bitmap == (1ull << id));
                break;
            }
            L1d_loc = loc;
        }

        if (auto [hit, s, loc] = L2.store(addr); loc != -1) {
            if (hit && s == Modified)
                break;
            else if (s == Exclusive || s == Shared) {
                cache &L3 = system::instance().access_L3();
                directory &dir = system::instance().access_dir();

                auto [found, L3_state, L3_loc] = L3.find(addr);
                assert(found && s == L3_state);

                dirent &ent = dir.entries[L3_loc];
                if (s == Shared && ent.bitmap != (1ull << id)) {
                    assert(ptp(addr, ent.bitmap, BusUpgr) == InvAck);
                    ent.bitmap &= (1ull << id);
                }
                
                /* Mark state as modified in all cache levels */
                L3.lines[L3_loc].state = Modified;
                L2.update(loc, Modified, false);
                insert(L1d, L1d_loc, addr, Modified);

                /**
                 * Assert this processor has the only private copy of the
                 * modified block
                 */
                assert(ent.bitmap == (1ull << id));
                break;
            }
            L2_loc = loc;
        }
        
        cache &L3 = system::instance().access_L3();
        if (auto [hit, s, loc] = L3.store(addr); loc != -1) {
            directory &dir = system::instance().access_dir();
            dirent &ent = dir.entries[loc];
            
            if (s == Modified) {
                assert(ent.bitmap && ent.valid);
                assert(ent.bitmap != (1ull << id));
                assert(ptp(addr, ent.bitmap, BusRdX) == Flush);
                ent.dirty |= 1;
                ent.bitmap = 0u;
                L3.update(loc, Modified, true);
            } else if (s == Exclusive || s == Shared) {
                if (s == Exclusive || ent.bitmap) {
                    assert(ent.bitmap && ent.valid);
                    assert(ptp(addr, ent.bitmap, BusRdX) == InvAck);
                    ent.bitmap = 0u;
                }
                L3.update(loc, Modified, true);
            } else /* if s == Invalid */
                insert(L3, loc, addr, Modified);
            
            insert(L2, L2_loc, addr, Modified);
            insert(L1d, L1d_loc, addr, Modified);
        } else /* if not found in L3 */ {
            insert(L3, -1, addr, Modified);
            insert(L2, L2_loc, addr, Modified);
            insert(L1d, L1d_loc, addr, Modified);
        }
    } while (0);

    auto [f, s, l] = system::instance().access_L3().find(addr);
    assert(f && s == Modified && l != -1);
    dirent &ent = system::instance().access_dir().entries[l];
    assert(ent.valid && (ent.bitmap && (1ull << id)));
}

void cpu::load_instr(void *addr) noexcept
{
    ptrdiff_t L1i_loc = -1, L2_loc = -1;

    do {
        if (auto [hit, s, loc] = L1i.load(addr); loc != -1) {
            if (hit)
                break;
            L1i_loc = loc;
        }

        if (auto [hit, s, loc] = L2.load(addr); loc != -1) {
            if (hit) {
                insert(L1i, L1i_loc, addr, s);
                break;
            }
            L2_loc = loc;
        }
        
        cache &L3 = system::instance().access_L3();
        if (auto [hit, s, loc] = L3.load(addr); loc != -1) {
            if (s == Invalid) {
                assert(!hit);
                /**
                 * If not a hit because the cache line is marked invalid
                 * (i.e. cache line with matching tag was found but
                 * state = I), insert the block into L3 at the position
                 * where a matching tag was found.
                 */
                insert(L3, loc, addr, Shared);
            }
            /* Bring into L2 and L1 */
            insert(L2, L2_loc, addr, Shared);
            insert(L1i, L1i_loc, addr, Shared);
        } else {
            /* Bring in from RAM -> L3 -> L2 -> L1 */
            insert(L3, -1, addr, Shared);
            insert(L2, L2_loc, addr, Shared);
            insert(L1i, L1i_loc, addr, Shared);
        }
    } while (0);
}

/**
 * directory::directory()
 *  Construct directory with number of entries equal to the number
 *  of blocks that are stored in the L3 cache
 */
directory::directory(size_t size, size_t block_size) noexcept
    : entries(size / block_size)
{}

system::system() noexcept
    : L3(l3_size, l3_blk_size, l3_assoc, SharedCache)
    , dir(l3_size, l3_blk_size)
    , sem(0u)
    , bus_transactions(0u)
{
    for (auto i = 0u; i < ncpus; i++)
        cpus[i] = cpu(i);

    worker = std::thread([this] {
        bool stop = false;
        for (;;) {
            task t;
            sem.acquire();
            {
                std::scoped_lock lock(mtx);
                t = std::move(tasks.front());
                tasks.pop();
            }
            if (stop && tasks.empty())
                return;
            else if (t.task_type == TASK_STOP) {
                stop = true;
                if (tasks.empty())
                    return;
                continue;
            } else {
                cpu &proc = cpus[t.cpuid];
                switch (t.task_type) {
                case STORE_DATA:
                    proc.store_data(t.addr);
                    continue;
                case LOAD_DATA:
                    proc.load_data(t.addr);
                    continue;
                case LOAD_INSTR:
                    proc.load_instr(t.addr);
                    continue;
                default:
                    assert(0);
                }
            }
        }
    });
}

system::~system() noexcept
{
    enqueue(TASK_STOP);
    worker.join();

    u64 L1i_totals[3]{};
    u64 L1d_totals[6]{};
    u64 L2_totals[6]{};

    for (const auto &proc : cpus) {
        L1i_totals[0] += proc.L1i.stats.rd_hits;
        L1i_totals[1] += proc.L1i.stats.rd_misses;
        L1i_totals[2] += proc.L1i.stats.evictions;

        L1d_totals[0] += proc.L1d.stats.wr_hits;
        L1d_totals[1] += proc.L1d.stats.wr_misses;
        L1d_totals[2] += proc.L1d.stats.rd_hits;
        L1d_totals[3] += proc.L1d.stats.rd_misses;
        L1d_totals[4] += proc.L1d.stats.evictions;
        L1d_totals[5] += proc.L1d.stats.write_backs;

        L2_totals[0] += proc.L2.stats.wr_hits;
        L2_totals[1] += proc.L2.stats.wr_misses;
        L2_totals[2] += proc.L2.stats.rd_hits;
        L2_totals[3] += proc.L2.stats.rd_misses;
        L2_totals[4] += proc.L2.stats.evictions;
        L2_totals[5] += proc.L2.stats.write_backs;
    }

    // u64 instructions    = L1i_totals[0] + L1i_totals[1];
    u64 loads           = L1i_totals[0] + L1i_totals[1] + 
                          L1d_totals[2] + L1d_totals[3];
    u64 stores          = L1d_totals[0] + L1d_totals[1];
    u64 evictions       = L1i_totals[2] + L1d_totals[4] +
                          L2_totals[4] + L3.stats.evictions;
    u64 write_backs     = L1d_totals[5] + L2_totals[5] + 
                          L3.stats.write_backs;
    u64 accesses        = loads + stores;
    
    f64 pct_loads       = (static_cast<f64>(loads) / 
                           static_cast<f64>(accesses)) * 100.0f;
    f64 pct_stores      = (static_cast<f64>(stores) / 
                           static_cast<f64>(accesses)) * 100.0f;
    
    f64 pct_l1d_hits    = (static_cast<f64>(L1d_totals[0] + 
                                            L1d_totals[2]) /
                           static_cast<f64>(L1d_totals[0] + 
                                            L1d_totals[1] +
                                            L1d_totals[2] + 
                                            L1d_totals[3])) * 100.0f;

    f64 pct_l2_hits     = (static_cast<f64>(L2_totals[0] +
                                            L2_totals[2]) /
                           static_cast<f64>(L2_totals[0] +
                                            L2_totals[1] +
                                            L2_totals[2] +
                                            L2_totals[3])) * 100.0f;

    u64 L3_accesses     = L3.stats.rd_hits + L3.stats.rd_misses +
                          L3.stats.wr_hits + L3.stats.wr_misses;
    u64 L3_misses       = L3.stats.rd_misses + L3.stats.wr_misses;


    std::cout << "+---------------+" << '\n'
              << "| Cache Summary |" << '\n'
              << "+---------------+" << '\n'
              << "Memory Accesses:      " << accesses << '\n'
              << "  Loads:              " << loads 
              << " (" << pct_loads << "%)" << '\n'
              << "  Stores:             " << stores 
              << " (" << pct_stores << "%)" << '\n'
              << "  Evictions:          " << evictions << '\n'
              << "  Writebacks:         " << write_backs << '\n'
              << '\n'
              << "+-------------+" << '\n'
              << "| L1d Summary |" << '\n'
              << "+-------------+" << '\n'
              << "  Hit Rate:           " << pct_l1d_hits << "%\n"
              << "  Write Hits:         " << L1d_totals[0] << '\n'
              << "  Write Misses:       " << L1d_totals[1] << '\n'
              << "  Read Hits:          " << L1d_totals[2] << '\n'
              << "  Read Misses:        " << L1d_totals[3] << '\n'
              << "  Evictions:          " << L1d_totals[4] << '\n'
              << "  Writebacks:         " << L1d_totals[5] << '\n'
              << '\n'
              << "+------------+" << '\n'
              << "| L2 Summary |" << '\n'
              << "+------------+" << '\n'
              << "  Accesses:           " << L2_totals[0] + L2_totals[1] + 
                                             L2_totals[2] + L2_totals[3] << '\n'
              << "  Hit Rate:           " << pct_l2_hits << "%\n"
              << "  Read Hits/Misses:   " << L2_totals[2] << " / " 
                                          << L2_totals[3] << '\n'
              << "  Write Hits/Misses:  " << L2_totals[0] << " / "
                                          << L2_totals[1] << '\n'
              << "  Writebacks:         " << L2_totals[5] << '\n'
              << '\n'
              << "+------------+" << '\n'
              << "| L3 Summary |" << '\n'
              << "+------------+" << '\n'
              << "  Accesses:           " << L3_accesses << '\n'
              << "  Misses to RAM:      " << L3_misses << '\n'
              << '\n'
              << "+-------------------+" << '\n'
              << "| Coherence Summary |" << '\n'
              << "+-------------------+" << '\n'
              << "  Bus Transactions:   " << bus_transactions << '\n';
}

/**
 * system::access_cpu()
 *  Access a specific cpu for communication
 */
auto system::access_cpu(u32 cpuid) noexcept -> cpu &
{
    return cpus[cpuid];
}

/**
 * system::access_cpus()
 *  Access system's cpus in order to initiate a bus transaction or
 *  send a bus request.
 */
auto system::access_cpus() noexcept -> std::array<cpu, ncpus> &
{
    return cpus;
}

/**
 * system::access_dir()
 *  Access the system's cache directory in order to read or modify
 *  cache directory entries.
 */
auto system::access_dir() noexcept -> directory &
{
    return dir;
}

/**
 * system::access_L3()
 *  Access the system's L3 (shared) cache, assuming that the cpu
 *  accessing the L3 cache has already acquired the bus.
 */
auto system::access_L3() noexcept -> cache &
{
    return L3;
}

void system::acquire() noexcept
{
    mtx.lock();
}

void system::release() noexcept
{
    mtx.unlock();
}

/**
 * system::initiate_transaction()
 *  Caller notifies that a bus transaction is being initiated
 */
void system::initiate_transaction() noexcept 
{
    bus_transactions++; 
}
} // cachesim

extern "C" void __cachesim_store_data(void *addr)
{
    int cpuid = sched_getcpu();
    cachesim::system::instance().enqueue(addr, cpuid, true, true);
}

extern "C" void __cachesim_load_data(void *addr)
{
    int cpuid = sched_getcpu();
    cachesim::system::instance().enqueue(addr, cpuid, false, true);
}

extern "C" void __cachesim_load_instr(u64 pc)
{
    int cpuid = sched_getcpu();
    cachesim::system::instance().enqueue(reinterpret_cast<void *>(pc),
                                         cpuid, false, false);
}

__attribute__((constructor(101))) static void setup()
{ 
    const char *msg = "Setting up cache simulator...\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    cachesim::system::instance(); 
}
