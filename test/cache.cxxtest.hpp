#ifndef __CACHE_TEST_HPP__
#define __CACHE_TEST_HPP__

#include <vector>
#include <random>
#include <cxxtest/TestSuite.h>
#include "../include/cache.hpp"

using namespace cachesim;
using namespace std;

class cache_test_suite : public CxxTest::TestSuite {
private:
    template<typename T>
    T logbase2(T t)
    {
        T x = 0;
        while (t > 1) {
            t >>= 1;
            x += 1;
        }
        return x;
    }
public:
    void test_decompose()
    {
        cache L1d(l1d_size, l1d_blk_size, l1d_assoc, PrivateCache);
        
        auto num_sets = l1d_size / (l1d_blk_size * l1d_assoc);
        auto n_offbits = logbase2(l1d_blk_size);
        auto n_ixbits = logbase2(num_sets);
        auto set_mask = ((1ull << n_ixbits) - 1) << n_offbits;

        int arr[1024];
        for (auto i = 0u; i < 1024; i++) {
            u64 addr = reinterpret_cast<u64>(&arr[i]);
            
            u64 expected_index = (addr & set_mask) >> n_offbits;
            u64 expected_tag = addr >> (n_ixbits + n_offbits);
            auto [index, tag] = L1d.decompose(&arr[i]);
            
            TS_ASSERT(index < num_sets);
            TS_ASSERT(index == expected_index);
            TS_ASSERT(tag == expected_tag);
        }
        

        const size_t jump = l1d_blk_size;
        
        alignas(jump) double d0;
        alignas(jump) double d1;
        alignas(jump) double d2;

        auto [ix0, tag0] = L1d.decompose(&d0);
        auto [ix1, tag1] = L1d.decompose(&d1);
        auto [ix2, tag2] = L1d.decompose(&d2);

        TS_ASSERT(ix0 != ix1);
        TS_ASSERT(ix0 != ix2);
        TS_ASSERT(ix1 != ix2);
    }

    void test_insert()
    {
        cache L1d(l1d_size, l1d_blk_size, l1d_assoc, PrivateCache);
        std::vector<int> v(100u, 0xFF);
        TS_ASSERT(v.size() == 100u);
        int *ptr = v.data();

        for (auto i = 0u; i < 100u; i++) {
            L1d.insert(ptr + i, Modified);
            {
                auto [found, state, loc] = L1d.find(ptr + i);
                TS_ASSERT(found && state == Modified);
                TS_ASSERT(loc >= 0);
                TS_ASSERT(static_cast<u64>(loc) < 
                          l1d_size / l1d_blk_size);
            }
            {
                auto [found, state, loc] = L1d.load(ptr + i, 0u);
                TS_ASSERT(found && state == Modified);
                TS_ASSERT(loc >= 0);
                TS_ASSERT(static_cast<u64>(loc) <
                          l1d_size / l1d_blk_size);
            }
        }
    }

    void test_conflict()
    {
        auto num_sets = l1d_size / (l1d_blk_size * l1d_assoc);
        auto n_ixbits = logbase2(num_sets);
        auto n_offbits = logbase2(l1d_blk_size);
        auto n_tagbits = sizeof(void *) * 8 - n_ixbits - n_offbits;

        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<u64> tag_dist(0u, (1ull << n_tagbits) - 1);
        uniform_int_distribution<u64> set_dist(0u, num_sets - 1);
        uniform_int_distribution<u64> off_dist(0u, l1d_blk_size - 1);

        auto repeat = [&](const std::vector<u64> &v, u64 tag) -> bool {
            bool b = std::any_of(begin(v), end(v), [&](u64 cmp) {
                auto cmp_tag = cmp >> (n_ixbits + n_tagbits);
                return cmp_tag == tag;
            });
            return b;
        };

        for (auto iter = 0u; iter < 100u; iter++) {
            cache L1d(l1d_size, l1d_blk_size, l1d_assoc, PrivateCache);
            std::vector<u64> addrs;
            auto set = set_dist(gen);
            
            for (auto i = 0u; i < l1d_assoc; i++) {
                u64 tag = tag_dist(gen);
                while (repeat(addrs, tag))
                    tag = tag_dist(gen);

                u64 off = off_dist(gen);
                u64 addr = (((tag << n_ixbits) | set) << n_offbits) | off;

                addrs.push_back(addr);
                auto [res, _r1, _r2] = L1d.insert(
                    reinterpret_cast<void *>(addr), Shared
                );
                TS_ASSERT(res == NoEvict);
            }
            
            u64 conflict = ((tag_dist(gen) << n_ixbits) | set) << n_offbits;
            auto [res, _r1, _r2] = L1d.insert(
                reinterpret_cast<void *>(conflict), Shared
            );
            TS_ASSERT(res != NoEvict);
        }
        
        cache L1d(l1d_size, l1d_blk_size, l1d_assoc, PrivateCache);
        vector<vector<u64>> addrs(num_sets, vector<u64>(l1d_assoc));
        for (auto set = 0u; set < num_sets; set++) {
            for (auto i = 0u; i < l1d_assoc; i++) {
                u64 tag = tag_dist(gen);
                while (repeat(addrs[set], tag))
                    tag = tag_dist(gen);
                
                u64 addr = ((tag << n_ixbits) | set) << n_offbits;
                addrs[set][i] = addr;
                auto [res, _r1, _r2] = L1d.insert(
                    reinterpret_cast<void *>(addr), Modified
                );
                TS_ASSERT(res == NoEvict);
            }
        }
        for (auto set = 0u; set < num_sets; set++) {
            for (auto iter = 0u; iter < 100u; iter++) {
                u64 tag = tag_dist(gen);
                while (repeat(addrs[set], tag))
                    tag = tag_dist(gen);

                u64 addr = ((tag << n_ixbits) | set) << n_offbits;
                auto [res, _r1, loc] = L1d.insert(
                    reinterpret_cast<void *>(addr), Modified
                );
                TS_ASSERT(loc / l1d_assoc == set);
                addrs[set][loc % l1d_assoc] = addr;
                TS_ASSERT(res == WriteBack);
            }
        }
    }
};

#endif // __CACHE_TEST_HPP__
