#ifndef __CACHESIM_HPP__
#define __CACHESIM_HPP__
#include <cstdint>
#include <memory>
#include <vector>
#include <array>

using i8        = int8_t;
using u8        = uint8_t;
using i16       = int16_t;
using u16       = uint16_t;
using i32       = int32_t;
using u32       = uint32_t;
using i64       = int64_t;
using u64       = uint64_t;

namespace cachesim {
template<size_t N>
struct cpu {
private:
        std::array<std::shared_ptr<cache>, N>   unified;
        std::unique_ptr<cache>                  l1d;
        std::unique_ptr<cache>                  l1i;
        u8                                      id;
public:
        cpu(std::array<cache *, N> &unified_, cache *l1d_, cache *l1i_);
        ~cpu() = default;
};

struct cache_line {
        u8 m    : 1;
        u8 e    : 1;
        u8 s    : 1;
        u8 i    : 1;
        u8 u    : 1;
        u64 tag : 59;

        cache_line() = default;
};

template<size_t N, size_t B>
struct cache_set {
private:
       std::array<cache_line, N>        lines;
       const u8                         log2_block_size;
public:
       cache_set();
};

template<size_t N>
struct cache {
private:
        std::array<cache_set, N>        sets;
        u8                              log2_cache_size;
        u8                              log2_cache_size;
        u8                              assoc;
public:
};

} // cachesim
#endif // __CACHESIM_HPP__
