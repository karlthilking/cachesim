#ifndef __CACHE_PARAMS_HPP__
#define __CACHE_PARAMS_HPP__

namespace cachesim {
constexpr size_t  ncpus           = 8;
constexpr size_t  l1i_size        = 32768;
constexpr size_t  l1i_blk_size    = 64;
constexpr int     l1i_assoc       = 8;
constexpr size_t  l1d_size        = 32768;
constexpr size_t  l1d_blk_size    = 64;
constexpr int     l1d_assoc       = 8;
constexpr size_t  l2_size         = 1048576;
constexpr size_t  l2_blk_size     = 64;
constexpr int     l2_assoc        = 16;
constexpr size_t  l3_size         = 16777216;
constexpr size_t  l3_blk_size     = 64;
constexpr int     l3_assoc        = 8;
} // cachesim
#endif // __CACHE_PARAMS_HPP__
