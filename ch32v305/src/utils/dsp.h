#pragma once

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <utility>

template<typename T> struct WiderInt;
template<> struct WiderInt<int8_t>   { using type = int16_t;  };
template<> struct WiderInt<int16_t>  { using type = int32_t;  };
template<> struct WiderInt<int32_t>  { using type = int64_t;  };
template<> struct WiderInt<uint8_t>  { using type = uint16_t; };
template<> struct WiderInt<uint16_t> { using type = uint32_t; };
template<> struct WiderInt<uint32_t> { using type = uint64_t; };
template<typename T> using WiderInt_t = typename WiderInt<T>::type;

template<typename T, size_t N>
class CICFilter
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
    static constexpr size_t kShift = std::countr_zero(N);
    static constexpr size_t kMask = N - 1U;

    using Sum = WiderInt_t<T>;

    T hist[N] = {0};
    Sum sum = 0;
    size_t idx = 0U;

public:
    T push(T x)
    {
        T old = hist[idx];
        hist[idx] = x;
        idx = (idx + 1U) & kMask;
        sum += Sum(x) - old;
        return T(sum >> kShift);
    }

    void reset()
    {
        std::fill(hist, hist + N, 0);
        sum = 0;
        idx = 0U;
    }
};

template<typename T, size_t N>
class CICComplexFilter
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
    static constexpr size_t kShift = std::countr_zero(N);
    static constexpr size_t kMask = N - 1U;

    using Sum = WiderInt_t<T>;

    /* Interleaved [i0,q0,i1,q1,...] so each tap fits in one cache line and the index/mask is shared. */
    T hist[2 * N] = {0};
    Sum sum_i = 0;
    Sum sum_q = 0;
    size_t idx = 0U;

public:
    std::pair<T, T> push(T i_now, T q_now)
    {
        size_t base = idx * 2U;
        T old_i = hist[base];
        T old_q = hist[base + 1U];
        hist[base] = i_now;
        hist[base + 1U] = q_now;
        idx = (idx + 1U) & kMask;
        sum_i += Sum(i_now) - old_i;
        sum_q += Sum(q_now) - old_q;
        return {T(sum_i >> kShift), T(sum_q >> kShift)};
    }

    void reset()
    {
        std::fill(hist, hist + 2 * N, 0);
        sum_i = 0;
        sum_q = 0;
        idx = 0U;
    }
};

/*
 * Single-pole IIR: y[n] = (1 - alpha) * x[n] + alpha * y[n-1], coefficients in QFRAC.
 * "One" saturates at type max for QFRAC == bit-width-1 since 2^(N-1) is unrepresentable in signed T.
 *
 * No saturating clamp on the output: with kOne == INT32_MAX (QFRAC=31), the
 * IIR is a strict convex combination — |state| <= max(|x|, |state_prev|), so
 * an input bounded by INT32_MAX keeps state bounded too. The atan2 output
 * feeding the FM path is in Q29 (< pi * 2^29 < 2^31), well within range.
 */
template<typename T, size_t QFRAC>
class SinglePoleIIR
{
    static_assert(QFRAC < sizeof(T) * 8, "QFRAC must be < bit-width of T");
    using Sum = WiderInt_t<T>;
    static constexpr T kOne = (QFRAC == sizeof(T) * 8 - 1)
                                ? std::numeric_limits<T>::max()
                                : T(T(1) << QFRAC);

    T state = 0;

public:
    T push(T x, T alpha)
    {
        Sum mixed = Sum(kOne - alpha) * x + Sum(alpha) * state;
        state = T(mixed >> QFRAC);
        return state;
    }

    T value() const { return state; }
    void seed(T x) { state = x; }
    void reset() { state = 0; }
};
