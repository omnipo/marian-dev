#pragma once

#include "tensors/tensor.h"
#include "tensors/tensor_allocator.h"
#include "tensors/tensor_operators.h"

#include <emmintrin.h>
#include <immintrin.h>
#include <tmmintrin.h>
#include <xmmintrin.h>

namespace marian {
namespace cpu {

/* TODO: CPUID dispatch, maybe integrate with intgemm */

// This operates on floats after processing so doesn't care about int8_t vs int16_t.
static void AddBias(marian::Tensor C, const marian::Tensor Bias) {
    float* y = C->data();
    const float* x = C->data();
    const float* bias = Bias->data();

    int m = C->shape().elements() / C->shape()[-1];
    int n = C->shape()[-1];
#ifdef __AVX512F__
    int n16 = n & ~15;
#else
    int n4 = (n / 4) * 4;
#endif

    for(int j = 0; j < m; ++j) {
        int i = 0;
#ifdef __AVX512F__
        for (; i < n16; i += 16) {
            __m512 ai = _mm512_loadu_ps(x + j * n + i);
            __m512 bi = _mm512_loadu_ps(bias + i);
            __m512 yi = _mm512_add_ps(ai, bi);
            _mm512_storeu_ps(y + j * n + i, yi);
        }
#else
        for (; i < n4; i += 4) {
            __m128 ai = _mm_loadu_ps(x + j * n + i);
            __m128 bi = _mm_loadu_ps(bias + i);
            __m128 yi = _mm_add_ps(ai, bi);
            _mm_storeu_ps(y + j * n + i, yi);
        }
#endif
        for (; i < n; i++) {
            y[j * n + i] = x[j * n + i] + bias[i];
        }
    }
}

}
}