#include <bit>
#include <span>
#include <algorithm>
#include <iterator>
#include <cassert>
#include "../include/params.hpp"
#include "../include/cache.hpp"

namespace cachesim {
std::pair<u64, u64> cache::decompose(void *addr) const noexcept
{
    auto index = (reinterpret_cast<u64>(addr) & set_mask) >> nr_offbits;
    auto tag = reinterpret_cast<u64>(addr) >> (nr_ixbits + nr_offbits);

    return {index, tag};
}

cache::cache(size_t size, size_t block_size, u32 assoc, u8 level) noexcept
    : lines(size / block_size),
      clock_hands(size / (block_size * assoc)),
      level(level)
{
    auto num_sets = size / (block_size * assoc);
    nr_offbits = std::popcount(block_size - 1);
    nr_ixbits = std::popcount(num_sets - 1);
    set_mask = (num_sets - 1) << nr_offbits;
}

std::pair<bool, cache_state> cache::find(void *addr) const noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) {
        return line.tag == tag;
    });
    if (it != end(set))
        return {true, it->state};

    return {false, Invalid};
}

size_t cache::evict(void *addr) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_of(set, [](const auto &line) {
        return line.state == Invalid;
    });
    if (it != end(set))
        return std::distance(begin(lines), it);

    for (auto i = clock_hands[index];; i = (i + 1) % set.size()) {
        if (set[i].u) {
            set[i].u = 0;
            continue;
        }

        return (index * assoc) + i;
    }
}

void cache::update(void *addr, cache_state new_state) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) {
        return line.tag == tag;
    });
    
    if (it == end(set)) {
        auto &line = lines[evict(addr)];
        stats.evictions++;
        line.state = new_state;
        line.tag = tag;
        line.u = 1;
    } else {
        if (it->state == Modified) {
            assert(new_state != Modified);
            stats.write_backs++;
        }
        it->state = new_state;
        it->u = 1;
    }
}

void cache::set_use(void *addr) noexcept
{
    auto [index, tag] = decompose(addr);
    auto set = std::span{lines.data() + (index * assoc), assoc};

    auto it = std::ranges::find_if(set, [](const auto &line) {
        return line.tag == tag;
    });

    assert(it != end(set));
    it->u = 1;
}

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
