#include "MathOps.h"
#include <immintrin.h>
#include <iostream>

void (*optimizedAdd)(const float*, const float*, float*, size_t) = nullptr;

// **SSE2 用の加算**
void Add_SSE2(const float* a, const float* b, float* result, size_t size) {
    for (size_t i = 0; i < size; i++) {
        result[i] = a[i] + b[i];
    }
}

// **SSE4.2 用の加算**
void Add_SSE4_2(const float* a, const float* b, float* result, size_t size) {
    for (size_t i = 0; i < size; i++) {
        result[i] = a[i] + b[i];
    }
}

// **AVX 用の加算**
void Add_AVX(const float* a, const float* b, float* result, size_t size) {
    size_t i;
    for (i = 0; i + 8 <= size; i += 8) {
        __m256 av = _mm256_loadu_ps(&a[i]);
        __m256 bv = _mm256_loadu_ps(&b[i]);
        __m256 rv = _mm256_add_ps(av, bv);
        _mm256_storeu_ps(&result[i], rv);
    }
    for (; i < size; i++) {
        result[i] = a[i] + b[i];
    }
}

// **AVX2 用の加算**
void Add_AVX2(const float* a, const float* b, float* result, size_t size) {
    size_t i;
    for (i = 0; i + 8 <= size; i += 8) {
        __m256 av = _mm256_loadu_ps(&a[i]);
        __m256 bv = _mm256_loadu_ps(&b[i]);
        __m256 rv = _mm256_add_ps(av, bv);
        _mm256_storeu_ps(&result[i], rv);
    }
    for (; i < size; i++) {
        result[i] = a[i] + b[i];
    }
}

// **AVX-512 用の加算**
void Add_AVX512(const float* a, const float* b, float* result, size_t size) {
    size_t i;
    for (i = 0; i + 16 <= size; i += 16) {
        __m512 av = _mm512_loadu_ps(&a[i]);
        __m512 bv = _mm512_loadu_ps(&b[i]);
        __m512 rv = _mm512_add_ps(av, bv);
        _mm512_storeu_ps(&result[i], rv);
    }
    for (; i < size; i++) {
        result[i] = a[i] + b[i];
    }
}

// **最適な関数を設定**
void InitMathOps(const std::string& selectedFeature) {
    if (selectedFeature == "AVX512") {
        optimizedAdd = Add_AVX512;
        std::cout << "[INFO] Using AVX-512 for addition." << std::endl;
    }
    else if (selectedFeature == "AVX2") {
        optimizedAdd = Add_AVX2;
        std::cout << "[INFO] Using AVX2 for addition." << std::endl;
    }
    else if (selectedFeature == "AVX") {
        optimizedAdd = Add_AVX;
        std::cout << "[INFO] Using AVX for addition." << std::endl;
    }
    else if (selectedFeature == "SSE4.2") {
        optimizedAdd = Add_SSE4_2;
        std::cout << "[INFO] Using SSE4.2 for addition." << std::endl;
    }
    else if (selectedFeature == "SSE2") {
        optimizedAdd = Add_SSE2;
        std::cout << "[INFO] Using SSE2 for addition." << std::endl;
    }
    else {
        optimizedAdd = Add_SSE2;
        std::cout << "[INFO] Fallback to SSE2." << std::endl;
    }
}
