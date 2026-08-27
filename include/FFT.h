#pragma once
#include <complex>
#include <vector>

namespace demo {

// Minimal iterative radix-2 Cooley-Tukey FFT, in place. data.size() must be
// a power of two. Verified against identity round-trip, single-bin sinusoid
// detection, and Parseval energy conservation (see scratchpad/fft_test.cpp
// from development - not part of the shipped build).
class FFT {
public:
    static void transform(std::vector<std::complex<double>>& a, bool inverse) {
        const size_t n = a.size();
        if (n <= 1) return;

        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }

        for (size_t len = 2; len <= n; len <<= 1) {
            double ang = 2.0 * M_PI / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
            std::complex<double> wlen(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<double> w(1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k) {
                    std::complex<double> u = a[i + k];
                    std::complex<double> v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }

        if (inverse) {
            for (auto& x : a) x /= static_cast<double>(n);
        }
    }
};

} // namespace demo
