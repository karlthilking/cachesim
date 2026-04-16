#ifndef __CACHE_HPP__
#define __CACHE_HPP__
#include <vector>
#include <array>
#include "params.hpp"

using i8        = std::int8_t;
using u8        = std::uint8_t;
using i16       = std::int16_t;
using u16       = std::uint16_t;
using i32       = std::int32_t;
using u32       = std::uint32_t;
using i64       = std::int64_t;
using u64       = std::uint64_t;
using size_t    = std::size_t;

namespace cachesim {
enum request {
    PrRd, PrWr, BusRd, BusRdX, BusUpgr, Flush, FlushOpt
};

enum cache_state : u8 {
    Modified    = 0x0,
    Exclusive   = 0x1,
    Shared      = 0x2,
    Invalid     = 0x3
};

struct cache_profile {
    u32 hits;
    u32 misses;
    u32 evictions;
    u32 write_backs;

    constexpr cache_profile() = default;
};

struct cache_line {
    u64 tag     : 61;   // address tag
    u64 state   : 2;    // state : { M, E, S, I }
    u64 use     : 1;    // use bit (LRU approximation)

    constexpr cache_line() noexcept
        : tag(0u), state(Invalid), use(0u)
    {}
};

template<size_t N = 64 - ncpus - 2, size_t M = ncpus>
struct cache_dirent {
    u64 tag     : N;
    u64 bitmap  : ncpus;
    u64 state   : 2;

    constexpr cache_dirent() noexcept
        : tag(0u), bitmap(0u), state(Invalid)
    {}
};

class cache_directory {
private:
    std::vector<cache_dirent<>> entries;
    u64                         set_mask;
    u32                         assoc;
    u8                          nr_ixbits;
    u8                          nr_offbits;
    
    std::pair<u64, u64> decompose(u64 addr) const noexcept;
public:
    cache_directory(size_t size, size_t block_size, u32 assoc) noexcept;

    void putS(u64 addr, u32 cpuid);
    void putM(u64 addr, u32 cpuid);
    void getS(u64 addr, u32 cpuid);
    void getM(u64 addr, u32 cpuid);
};

class cache {
private:
    std::vector<cache_line> lines;
    std::vector<size_t>     clock_hands; // 1 clock hand per set for eviction
    cache_profile           prof;
    u64                     set_mask;
    u32                     assoc;
    u8                      nr_offbits; // num offset bits = log2(block size)
    u8                      nr_ixbits;  // num index bits = log2(num sets)

    std::pair<u64, u64> decompose(void *addr) const noexcept;
public:
    cache() = default;
    cache(size_t size, size_t block_size, unsigned int assoc) noexcept;
    
    /**
     * Evict a line to allow the data block corresponding to the given address to
     * replace the evicted entry in the cache.
     */
    void evict(void *addr, cache_state new_state) noexcept;

    /**
     * Update an invalid cache entry with a new state corresponding to where the
     * data block was brought in from and type of request for the data block
     */
    void update(void *addr, cache_state new_state) noexcept;
    
    /**
     * Search cache for an entry corresponding to the given address to service
     * the processor read request.
     */
    bool load(void *addr) noexcept;

    /**
     * Search cache for an entry corresponding to the given address in order
     * to service the request to perform a write.
     */
    bool store(void *addr) noexcept;
};

class cpu {
private:
    cache   L1i_cache;
    cache   L1d_cache;
    cache   L2_cache;
    u32     id;
public:
    cpu(int id) noexcept;

    void access(void *addr, bool data, bool write) noexcept;
};

class system {
private:
    std::array<cpu, ncpus>  cpus;
    cache_directory         dir;
    cache                   L3_cache;

    system() noexcept;
public:
    static system &instance() noexcept
    {
        system sys;
        return sys;
    }

    static cache_directory &get_directory() noexcept
    {
        return system::instance().dir;
    }
};
} // cachesim
#endif // __CACHE_HPP__
