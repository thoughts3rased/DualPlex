#include "fft.h"

#include <math.h>

void fft_forward(float* re, float* im, int n) {
    if (n < 2) return;

    // --- Bit-reversal permutation -------------------------------------
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    // --- Butterflies, stage by stage --------------------------------
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wlen_r = cosf(ang);
        float wlen_i = sinf(ang);
        for (int base = 0; base < n; base += len) {
            float wr = 1.0f, wi = 0.0f;
            int half = len >> 1;
            for (int k = 0; k < half; k++) {
                float ur = re[base + k];
                float ui = im[base + k];
                float pr = re[base + k + half];
                float pi = im[base + k + half];
                float vr = pr * wr - pi * wi;
                float vi = pr * wi + pi * wr;
                re[base + k] = ur + vr;
                im[base + k] = ui + vi;
                re[base + k + half] = ur - vr;
                im[base + k + half] = ui - vi;
                // advance the twiddle by one step: w *= wlen
                float nwr = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = nwr;
            }
        }
    }
}
