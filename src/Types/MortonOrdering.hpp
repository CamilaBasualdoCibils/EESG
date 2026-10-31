#pragma once
#include "Types.hpp"
#include <cstdint>
#include <immintrin.h>
#include <iostream>
namespace Lighthouse {
namespace Morton {
namespace SIMD {
// -------------------- 2D Morton (SIMD) --------------------

inline __m256i Part1By1SIMD(__m256i x) {
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi32(x, 16)),
                       _mm256_set1_epi32(0x0000FFFF));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi32(x, 8)),
                       _mm256_set1_epi32(0x00FF00FF));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi32(x, 4)),
                       _mm256_set1_epi32(0x0F0F0F0F));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi32(x, 2)),
                       _mm256_set1_epi32(0x33333333));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi32(x, 1)),
                       _mm256_set1_epi32(0x55555555));
  return x;
}

inline __m256i Compact1By1SIMD(__m256i x) {
  x = _mm256_and_si256(x, _mm256_set1_epi32(0x55555555));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi32(x, 1)),
                       _mm256_set1_epi32(0x33333333));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi32(x, 2)),
                       _mm256_set1_epi32(0x0F0F0F0F));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi32(x, 4)),
                       _mm256_set1_epi32(0x00FF00FF));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi32(x, 8)),
                       _mm256_set1_epi32(0x0000FFFF));
  return x;
}

// -------------------- 3D Morton (SIMD) --------------------

inline __m256i Part1By2SIMD(__m256i x) {
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi64(x, 32)),
                       _mm256_set1_epi64x(0x1F00000000FFFFULL));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi64(x, 16)),
                       _mm256_set1_epi64x(0x1F0000FF0000FFULL));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi64(x, 8)),
                       _mm256_set1_epi64x(0x100F00F00F00F00FULL));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi64(x, 4)),
                       _mm256_set1_epi64x(0x10C30C30C30C30C3ULL));
  x = _mm256_and_si256(_mm256_or_si256(x, _mm256_slli_epi64(x, 2)),
                       _mm256_set1_epi64x(0x1249249249249249ULL));
  return x;
}

inline __m256i Compact1By2SIMD(__m256i x) {
  x = _mm256_and_si256(x, _mm256_set1_epi64x(0x1249249249249249ULL));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi64(x, 2)),
                       _mm256_set1_epi64x(0x10C30C30C30C30C3ULL));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi64(x, 4)),
                       _mm256_set1_epi64x(0x100F00F00F00F00FULL));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi64(x, 8)),
                       _mm256_set1_epi64x(0x1F0000FF0000FFULL));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi64(x, 16)),
                       _mm256_set1_epi64x(0x1F00000000FFFFULL));
  x = _mm256_and_si256(_mm256_xor_si256(x, _mm256_srli_epi64(x, 32)),
                       _mm256_set1_epi64x(0x00FFFFFFULL));
  return x;
}

template <size_t Dim>
inline Vec<Dim, uint32_t> MortonDecode(uint64_t mortonIndex) {
  static_assert(Dim == 2 || Dim == 3, "2D or 3D supported");

  if constexpr (Dim == 2) {
    uint32_t x = _mm256_extract_epi32(
        Compact1By1SIMD(_mm256_set1_epi32((uint32_t)mortonIndex)), 0);
    uint32_t y = _mm256_extract_epi32(
        Compact1By1SIMD(_mm256_set1_epi32((uint32_t)(mortonIndex >> 1))), 0);
    return {x, y};
  } else {
    uint32_t x = _mm256_extract_epi64(
        Compact1By2SIMD(_mm256_set1_epi64x(mortonIndex)), 0);
    uint32_t y = _mm256_extract_epi64(
        Compact1By2SIMD(_mm256_set1_epi64x(mortonIndex >> 1)), 0);
    uint32_t z = _mm256_extract_epi64(
        Compact1By2SIMD(_mm256_set1_epi64x(mortonIndex >> 2)), 0);
    return {x, y, z};
  }
}

template <size_t Dim>
inline glm::vec<Dim, uint32_t> MortonDecodeBounded(uint64_t code,
                                                   uint32_t maxBounds) {
  static_assert(Dim == 2 || Dim == 3, "Only 2D or 3D supported");

  // Compute required bits per axis (same for all axes)
  uint32_t bitsPerAxis =
      (maxBounds > 1) ? static_cast<uint32_t>(std::ceil(std::log2(maxBounds)))
                      : 1;

  // Mask the code to remove unused bits
  uint64_t mask = (1ULL << (Dim * bitsPerAxis)) - 1;
  code &= mask;

  __m256i v = _mm256_set1_epi64x(code); // broadcast code into SIMD register

  if constexpr (Dim == 2) {
    __m256i x = Compact1By1SIMD(v);
    __m256i y = Compact1By1SIMD(_mm256_srli_epi64(v, 1));

    return glm::vec<2, uint32_t>(_mm256_extract_epi32(x, 0),
                                 _mm256_extract_epi32(y, 0));
  } else // Dim == 3
  {
    __m256i x = Compact1By2SIMD(v);
    __m256i y = Compact1By2SIMD(_mm256_srli_epi64(v, 1));
    __m256i z = Compact1By2SIMD(_mm256_srli_epi64(v, 2));

    return glm::vec<3, uint32_t>(_mm256_extract_epi32(x, 0),
                                 _mm256_extract_epi32(y, 0),
                                 _mm256_extract_epi32(z, 0));
  }
}
template <size_t Dim> uint64_t MortonEncode(glm::vec<Dim, uint32_t> pos) {
  static_assert(Dim == 2 || Dim == 3, "2D or 3D supported");

  if constexpr (Dim == 2) {
    __m256i x = _mm256_set1_epi32(pos.x);
    __m256i y = _mm256_set1_epi32(pos.y);

    __m256i mx = Part1By1SIMD(x);
    __m256i my = Part1By1SIMD(y);

    uint32_t xx = _mm256_extract_epi32(mx, 0);
    uint32_t yy = _mm256_extract_epi32(my, 0);

    return (static_cast<uint64_t>(yy) << 1) | xx;
  } else {
    __m256i x = _mm256_set1_epi64x(pos.x);
    __m256i y = _mm256_set1_epi64x(pos.y);
    __m256i z = _mm256_set1_epi64x(pos.z);

    uint64_t xx = _mm256_extract_epi64(Part1By2SIMD(x), 0);
    uint64_t yy = _mm256_extract_epi64(Part1By2SIMD(y), 0);
    uint64_t zz = _mm256_extract_epi64(Part1By2SIMD(z), 0);

    return (zz << 2) | (yy << 1) | xx;
  }
}
template <size_t Dim>
uint64_t MortonEncodeBounded(glm::vec<Dim, uint32_t> pos, uint32_t maxBounds) {
  static_assert(Dim == 2 || Dim == 3, "Only 2D or 3D supported");

  // Compute required bits per axis
  uint32_t bitsPerAxis =
      (maxBounds > 1) ? static_cast<uint32_t>(std::ceil(std::log2(maxBounds)))
                      : 1;
  uint32_t mask = (1u << bitsPerAxis) - 1;
  if constexpr (Dim == 2) {
    uint32_t x = pos.x & mask;
    uint32_t y = pos.y & mask;

    __m256i mx = Part1By1SIMD(_mm256_set1_epi32(x));
    __m256i my = Part1By1SIMD(_mm256_set1_epi32(y));

    return (static_cast<uint64_t>(_mm256_extract_epi32(my, 0)) << 1) |
           _mm256_extract_epi32(mx, 0);
  } else // Dim == 3
  {
    uint32_t x = pos.x & mask;
    uint32_t y = pos.y & mask;
    uint32_t z = pos.z & mask;

    __m256i mx = Part1By2SIMD(_mm256_set1_epi64x(x));
    __m256i my = Part1By2SIMD(_mm256_set1_epi64x(y));
    __m256i mz = Part1By2SIMD(_mm256_set1_epi64x(z));

    return (static_cast<uint64_t>(_mm256_extract_epi64(mz, 0)) << 2) |
           (static_cast<uint64_t>(_mm256_extract_epi64(my, 0)) << 1) |
           _mm256_extract_epi64(mx, 0);
  }
}
}; // namespace SIMD
namespace Regular {

/// Non SIMD
inline static uint64_t Part1By1(uint32_t x) {
  uint64_t v = x;
  v = (v | (v << 16)) & 0x0000FFFF0000FFFFULL;
  v = (v | (v << 8)) & 0x00FF00FF00FF00FFULL;
  v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0FULL;
  v = (v | (v << 2)) & 0x3333333333333333ULL;
  v = (v | (v << 1)) & 0x5555555555555555ULL;
  return v;
}

inline static uint64_t Part1By2(uint32_t x) {
  uint64_t v = x;
  v = (v | (v << 32)) & 0x1F00000000FFFFULL;
  v = (v | (v << 16)) & 0x1F0000FF0000FFULL;
  v = (v | (v << 8)) & 0x100F00F00F00F00FULL;
  v = (v | (v << 4)) & 0x10C30C30C30C30C3ULL;
  v = (v | (v << 2)) & 0x1249249249249249ULL;
  return v;
}

inline static uint32_t Compact1By1(uint64_t x) {
  x &= 0x5555555555555555ULL;
  x = (x ^ (x >> 1)) & 0x3333333333333333ULL;
  x = (x ^ (x >> 2)) & 0x0F0F0F0F0F0F0F0FULL;
  x = (x ^ (x >> 4)) & 0x00FF00FF00FF00FFULL;
  x = (x ^ (x >> 8)) & 0x0000FFFF0000FFFFULL;
  x = (x ^ (x >> 16)) & 0x00000000FFFFFFFFULL;
  return static_cast<uint32_t>(x);
}

inline static uint32_t Compact1By2(uint64_t x) {
  x &= 0x1249249249249249ULL;
  x = (x ^ (x >> 2)) & 0x10C30C30C30C30C3ULL;
  x = (x ^ (x >> 4)) & 0x100F00F00F00F00FULL;
  x = (x ^ (x >> 8)) & 0x1F0000FF0000FFULL;
  x = (x ^ (x >> 16)) & 0x1F00000000FFFFULL;
  x = (x ^ (x >> 32)) & 0x0000000000FFFFFFULL;
  return static_cast<uint32_t>(x);
}

template <size_t Dim>
static inline vec<Dim, uint32_t> MortonDecode(uint64_t mortonIndex) {
  static_assert(Dim == 2 || Dim == 3, "2D or 3D supported");

  if constexpr (Dim == 2) {
    return {Compact1By1(mortonIndex), Compact1By1(mortonIndex >> 1)};
  } else {
    return {Compact1By2(mortonIndex), Compact1By2(mortonIndex >> 1),
            Compact1By2(mortonIndex >> 2)};
  }
}

template <size_t Dim>
inline glm::vec<Dim, uint32_t, glm::defaultp>
MortonDecodeBounded(uint64_t code, Vec<Dim, uint32_t> max) {
  glm::vec<Dim, uint32_t, glm::defaultp> pos(0);

  // compute required bits per axis
  uint32_t bits[Dim];
  uint32_t maxBits = 0;
  for (size_t d = 0; d < Dim; d++) {
    bits[d] =
        (max[d] > 1) ? static_cast<uint32_t>(std::ceil(std::log2(max[d]))) : 1;
    maxBits = std::max(maxBits, bits[d]);
  }

  for (uint32_t i = 0; i < maxBits; i++) {
    for (size_t d = 0; d < Dim; d++) {
      if (i < bits[d]) {
        uint32_t bit = (code >> (Dim * i + d)) & 1ULL;
        pos[d] |= bit << i;
      }
    }
  }
  return pos;
}
template <size_t Dim> uint64_t MortonEncode(Vec<Dim, uint32_t> pos) {
  static_assert(Dim == 2 || Dim == 3, "2D or 3D supported");

  if constexpr (Dim == 2) {
    return Part1By1(pos.y) << 1 | Part1By1(pos.x);
  } else {
    return (Part1By2(pos.z) << 2) | (Part1By2(pos.y) << 1) | (Part1By2(pos.x));
  }
}
template <size_t Dim>
uint64_t MortonEncodeBounded(Vec<Dim, uint32_t> pos, Vec<Dim, uint32_t> max) {

  // compute required bits per axis
  uint32_t bits[Dim];
  uint32_t maxBits = 0;
  for (size_t d = 0; d < Dim; d++) {
    bits[d] =
        (max[d] > 1) ? static_cast<uint32_t>(std::ceil(std::log2(max[d]))) : 1;
    maxBits = std::max(maxBits, bits[d]);
  }

  // sanity check: Morton codes fit in 64 bits
  // i.e. sum of bits across dimensions ≤ 63
  uint32_t totalBits = 0;
  for (size_t d = 0; d < Dim; d++)
    totalBits += bits[d];
  assert(totalBits <= 63);

  uint64_t code = 0;
  for (uint32_t i = 0; i < maxBits; i++) {
    for (size_t d = 0; d < Dim; d++) {
      if (i < bits[d]) {
        uint64_t bit = (pos[d] >> i) & 1ULL;
        code |= bit << (Dim * i + d);
      }
    }
  }
  return code;
}
}; // namespace Regular
} // namespace Morton
} // namespace Lighthouse
