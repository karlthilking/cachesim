#include <random>
#include <cstdlib>
#include <vector>
#include <thread>

int main(int argc, char *argv[])
{
    size_t N = 1 << 6;
    size_t M = 1 << 6;
    unsigned int iters = (argc > 1) ? 
                         std::strtoul(argv[1], nullptr, 10) : 8u;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-256.0f, 256.0f);

    for (auto iter = 0u; iter < iters; iter++) {
        float *A = new float[N * M];
        float *B = new float[M * N];
        float *C = new float[N * N];

        for (float *f = A; f < (A + (N * M)); f++) {
            *f = dist(gen);
        }
        
        for (float *f = B; f < (B + (M * N)); f++) {
            *f = dist(gen);
        }

        for (float *f = C; f < (C + (N * N)); f++) {
            *f = 0.0f;
        }
        
        const size_t n_threads = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        threads.reserve(n_threads);

        auto work = [&](size_t l, size_t r) -> void {
            for (auto i = l; i < r; i++)
                for (auto k = 0u; k < M; k++)
                    for (auto j = 0u; j < N; j++)
                        *(C+(i*N)+j) += *(A+(i*N)+k) * *(B+(k*M)+j);
        };
        
        const auto step = N / n_threads;
        for (auto i = 0u; i + step <= N; i += step)
            threads.emplace_back(work, i, i + step);

        for (auto &t : threads)
            t.join();

        delete[] A; delete[] B; delete[] C;
    }

    return 0;
}
