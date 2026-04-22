#include <random>

int main(void)
{
    size_t N = 1 << 6;
    size_t M = 1 << 6;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-256.0f, 256.0f);

    for (auto iter = 0u; iter < 32u; iter++) {
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

        for (auto i = 0u; i < N; i += 1) {
            for (auto k = 0u; k < M; k += 1) {
                for (auto j = 0u; j < N; j += 1) {
                    float f = *(A+(i*N)+k) * *(B+(k*M)+j);
                    *(C+(i*N)+j) += f;
                }
            }
        }

        delete[] A; delete[] B; delete[] C;
    }

    return 0;
}
