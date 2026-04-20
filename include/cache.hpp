#ifndef __CACHE_HPP__
#define __CACHE_HPP__
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <numeric>
#include <mutex>
#include <semaphore>
#include <queue>
#include <thread>
#include <future>
#include "params.hpp"

using i8        = std::int8_t;
using u8        = std::uint8_t;
using i16       = std::int16_t;
using u16       = std::uint16_t;
using i32       = std::int32_t;
using u32       = std::uint32_t;
using i64       = std::int64_t;
using u64       = std::uint64_t;

#if defined(__STDCPP_FLOAT32_T__) && defined(__STDCPP_FLOAT64_T__)
#include <stdfloat>
using f32       = std::float32_t;
using f64       = std::float64_t;
#else
using f32       = float;
using f64       = double;
#endif

#define max_sem_count std::numeric_limits<i32>::max()

using size_t    = std::size_t;
using ptrdiff_t = std::ptrdiff_t;

namespace cachesim {
enum request : u8 {
    NullRq, BusRd, BusRdX, BusUpgr, BusInv, BusFlush
};

using response = u8;
#define InvAck      0x1
#define NullAck     0x2
#define ShareAck    0x4
#define Flush       0x8

#define RESPONSE_STRING(rsp) \
    (((rsp) == InvAck)   ? "(response=InvAck)"   : \
     ((rsp) == NullAck)  ? "(response=NullAck)"  : \
     ((rsp) == ShareAck) ? "(response=ShareAck)" : \
     ((rsp) == Flush)    ? "(response=Flush)"    : "")

using cache_state = u8;
#define Modified    0x0
#define Exclusive   0x1
#define Shared      0x2
#define Invalid     0x3

#define STATE_STRING(s) \
    (((s) == Modified)  ? "(state=M)"   : \
     ((s) == Exclusive) ? "(state=E)"   : \
     ((s) == Shared)    ? "(state=S)"   : \
     ((s) == Invalid)   ? "(state=I)"   : "")

enum evict_result : u8 {
    NoEvict         = 0x0,
    Evict           = 0x1,
    WriteBack       = 0x2,
    WriteThrough    = 0x3
};

enum cache_type : u8 {
    PrivateCache,
    BoundaryCache,
    SharedCache
};

using task_descriptor = u8;
#define NullTask    0x00
#define LoadTask    0x01
#define StoreTask   0x02
#define DataTask    0x04
#define InstrTask   0x08
#define DflTask     0x10
#define SnoopTask   0x20
#define StopTask    0x40

struct cache_profile {
    u32 wr_hits;
    u32 rd_hits;
    u32 wr_misses;
    u32 rd_misses;
    u32 evictions;
    u32 write_backs;

    constexpr cache_profile() noexcept
        : wr_hits(0u)
        , rd_hits(0u)
        , wr_misses(0u)
        , rd_misses(0u)
        , evictions(0u)
        , write_backs(0u)
    {}
};

struct cache_line {
    u64 tag     : 61;   // address tag
    u64 state   : 2;    // state : { M, E, S, I }
    u64 use     : 1;    // use bit (LRU approximation)

    constexpr cache_line() noexcept
        : tag(0u)
        , state(Invalid)
        , use(0u)
    {}
};

class cache {
private:
    std::vector<cache_line> lines;
    std::vector<size_t>     clock_hands;
    cache_profile           stats;
    u64                     set_mask;
    u32                     assoc;
    u8                      n_offbits;
    u8                      n_ixbits;
    cache_type              type;
    
    friend class cpu;
    friend class system;

    std::pair<u64, u64> decompose(void *addr) const noexcept;
public:
    cache() = default;
    cache(size_t sz, size_t blk_sz, u32 assoc, cache_type ty) noexcept;

    bool contains(void *addr, bool find_invalid) const noexcept;
    std::tuple<bool, cache_state, ptrdiff_t> find(void *addr) const noexcept;

    std::tuple<bool, cache_state, ptrdiff_t> load(void *addr) noexcept;
    std::tuple<bool, cache_state, ptrdiff_t> store(void *addr) noexcept;
    
    ptrdiff_t elect(void *addr) noexcept;
    
    std::pair<evict_result, u64> 
    evict(ptrdiff_t loc, void *addr, cache_state state) noexcept;

    std::tuple<evict_result, u64, ptrdiff_t> 
    insert(void *addr, cache_state state) noexcept;

    void update(ptrdiff_t loc, cache_state state, bool use) noexcept;
    void update(void *addr, cache_state state, bool use) noexcept;
};

struct task {
    std::promise<response>  promise;
    void                    *addr;
    task_descriptor         td;
    request                 brq;
    
    task() noexcept = default;

    task(void *addr_, task_descriptor td_, request brq) noexcept
        : addr(addr), td(td), brq(brq)
    {}

    task(void *addr, task_descriptor td, 
         request brq, std::promise<response> prom) noexcept
         : promise(std::move(prom)), addr(addr), td(td), brq(brq)
    {}
    
    task(const task &_t) = delete;
    task &operator=(const task &_t) = delete;

    task(task &&t) noexcept
        : promise(std::move(t.promise))
        , addr(std::exchange(t.addr, nullptr))
        , td(std::exchange(t.td, NullTask))
        , brq(t.brq)
    {}

    task &operator=(task &&t) noexcept
    {
        if (this != &t) {
            promise = std::move(t.promise);
            addr = std::exchange(t.addr, nullptr);
            td = std::exchange(t.td, NullTask);
            brq = t.brq;
        }
        return *this;
    }
};

struct taskcmp {
    auto operator()(const task &a, const task &b) -> bool
    {
        return a.td != SnoopTask && b.td != SnoopTask;
    }
};

class cpu {
public:
    using task_queue = std::priority_queue<task, std::vector<task>, taskcmp>;
    using counting_sem = std::counting_semaphore<max_sem_count>;
private:
    task_queue      tasks;
    std::mutex      mtx;
    std::thread     worker;
    counting_sem    sem;
    cache           L1d;
    cache           L1i;
    cache           L2;
    u32             id;
    bool            stop;

    friend class system;
public:
    cpu() noexcept;
    void start(u32 cpuid) noexcept;
    
    cpu(const cpu &other) = delete;
    cpu &operator=(const cpu &other) = delete;
    
    cpu(cpu &&other) = delete;
    cpu &operator=(cpu &&other) = delete;
    
    response recvBusUpgr(void *addr) noexcept;
    response recvBusRdX(void *addr) noexcept;
    response recvBusRd(void *addr) noexcept;
    response recvBusInv(void *addr) noexcept;
    response recvBusFlush(void *addr) noexcept;
    response snoop(void *addr, request brq) noexcept;
    
    response ptp(void *addr, u32 bitmap, request brq) noexcept;
    response bcast(void *addr, request brq) noexcept;
    
    void insert(cache &c, ptrdiff_t loc, void *addr, 
                cache_state state, bool data) noexcept;
    
    void load_data(void *addr) noexcept;
    void store_data(void *addr) noexcept;
    void load_instr(void *addr) noexcept;
    
    /**
     * enqueue_task():
     *  Notify cpu of available work (loads, stores)
     */
    template<typename... Args>
    void enqueue_task(Args &&...args) noexcept
    {
        std::scoped_lock lock(mtx);
        tasks.emplace(std::forward<Args>(args)...);
        sem.release();
    }
    
    /**
     * enqueue_busrq():
     *  Notify cpu to snoop a bus request sent over the memory bus by
     *  adding a snoop request message onto its work queue
     */
    template<typename... Args>
    std::future<response> enqueue_busrq(Args &&...args) noexcept
    requires std::is_constructible_v<task, Args..., std::promise<response>>
    {
        std::promise<response> promise;
        auto future = promise.get_future();
        {
            std::scoped_lock lock(mtx);
            tasks.emplace(std::forward<Args>(args)..., std::move(promise));
        }
        sem.release();
        return future;
    }
};

struct dirent {
    u32 bitmap  : ncpus;
    u32 dirty   : 1;
    u32 valid   : 1;

    constexpr dirent() noexcept
        : bitmap(0u)
        , dirty(0u)
        , valid(0u)
    {}
};

struct directory {
    std::vector<dirent> entries;
    directory(size_t size, size_t block_size) noexcept;
};

class system {
private:
    std::array<cpu, ncpus>  cpus;
    cache                   L3;
    directory               dir;
    std::mutex              bus;
    u32                     bus_transactions;

    system() noexcept;
    ~system() noexcept;
public:
    static system &instance() noexcept
    {
        static system sys;
        return sys;
    }
    
    auto access_cpu(u32 cpuid) noexcept -> cpu &;
    auto access_cpus() noexcept -> std::array<cpu, ncpus> &;
    
    auto access_dir() noexcept -> directory &;
    auto access_L3() noexcept -> cache &;

    void acquire_bus(cpu *proc) noexcept;
    void release_bus() noexcept;
    void initiate_transaction() noexcept;
};
} // cachesim

void process(void *addr, bool write, bool data);

#endif // __CACHE_HPP__
