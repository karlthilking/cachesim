#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>
#include <array>

int main(void)
{
    const size_t N = 8;
    std::array<std::atomic<std::uint64_t>, N / 2> counters{0};
    
    for (auto i = 0u; i < 256u; i++) {
        std::vector<std::thread> threads;
        threads.reserve(N);

        for (auto i = 0u; i < N; i++) {
            threads.emplace_back([&, i] {
                auto op = [&]() -> void {
                    if (i % 2)
                        counters[i / 2].fetch_add(1);
                    else
                        counters[i / 2].fetch_sub(1);
                };
                for (auto j = 0u; j < (N / UINT64_MAX); j++)
                    op();
            });
        }

        for (auto &t : threads)
            t.join();

        for (auto i = 0u; i < N / 2; i++)
            counters[i].store(i);
    }
    
    return 0;
}
