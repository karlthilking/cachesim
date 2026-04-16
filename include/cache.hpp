#ifndef __CACHE_HPP__
#define __CACHE_HPP__
#include <vector>
#include <array>
#include <cstddef>
#include <tuple>
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
using ptrdiff_t = std::ptrdiff_t;

namespace cachesim {
enum request : u8 {
    BusRd, BusRdX, BusUpgr
};

enum response : u8 {
    InvAck      = 0x1, 
    NullAck     = 0x2,
    ShareAck    = 0x4,
    Flush       = 0x8
};

enum cache_state : u8 {
    Modified    = 0x0,
    Exclusive   = 0x1,
    Shared      = 0x2,
    Invalid     = 0x3
};

struct cache_profile {
    u32 wr_hits;
    u32 rd_hits;
    u32 wr_misses;
    u32 rd_misses;
    u32 evictions;
    u32 write_backs;
    u32 bus_transactions;

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

template<typename T>
class cache {
private:
    std::vector<cache_line> lines;
    std::vector<size_t>     clock_hands;
    cache_profile           stats;
    u64                     set_mask;
    u32                     assoc;
    u8                      n_offbits;
    u8                      n_ixbits;
    bool                    local;

    friend class cpu;
    std::pair<u64, u64> decompose(void *addr) const noexcept;
public:
    cache() = default;
    cache(size_t size, size_t block_size, u32 assoc, bool local) noexcept;

    bool contains(void *addr, bool find_invalid) const noexcept;
    std::tuple<bool, cache_state, ptrdiff_t> find(void *addr) const noexcept;

    std::tuple<bool, cache_state, ptrdiff_t> load(void *addr) noexcept;
    std::tuple<bool, cache_state, ptrdiff_t> store(void *addr) noexcept;
    
    size_t elect(void *addr) noexcept;
    void evict(size_t index, void *addr, cache_state state) noexcept;
    
    void insert(void *addr, cache_state state) noexcept;
    void update(void *addr, cache_state state) noexcept;
};

class cpu {
private:
    cache   L1d;
    cache   L1i;
    cache   L2;
    u32     id;
public:
    cpu(int id) noexcept;
    
    response recvBusUpgr(void *addr) noexcept;
    response recvBusRdX(void *addr) noexcept;
    response recvBusRd(void *addr) noexcept;
    response snoop(void *addr, request brq) noexcept;
    
    void sendBusUpgr(void *addr) noexcept;
    void sendBusRdX(void *addr) noexcept;
    void sendBusRd(void *addr) noexcept;
    void bcast(void *addr, request brq) noexcept;
    
    void load_data(void *addr) noexcept;
    void store_data(void *addr) noexcept;
    void load_instr(void *addr) noexcept;
};

class system {
private:
    std::array<cpu, ncpus>  cpus;
    cache                   L3;

    system() noexcept;
public:
    static system &instance() noexcept
    {
        system sys;
        return sys;
    }

    std::array<cpu, ncpus> &get_cpus() noexcept { return cpus; }
    cache &access_L3() noexcept { return L3; }
};
} // cachesim
#endif // __CACHE_HPP__
