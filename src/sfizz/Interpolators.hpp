// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Interpolators.h"
#include "WindowedSinc.h"
#include "MathHelpers.h"
#include "SIMDConfig.h"

namespace sfz {

template <InterpolatorModel M, class R>
class Interpolator;

template <InterpolatorModel M, class R>
inline R interpolate(const R* values, R coeff)
{
    return Interpolator<M, R>::process(values, coeff);
}

//------------------------------------------------------------------------------
// Nearest

template <class R>
class Interpolator<kInterpolatorNearest, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        return values[coeff > static_cast<R>(0.5)];
    }
};

//------------------------------------------------------------------------------
// Linear

template <class R>
class Interpolator<kInterpolatorLinear, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        return values[0] * (static_cast<R>(1.0) - coeff) + values[1] * coeff;
    }
};

//------------------------------------------------------------------------------
// Hermite 3rd order, SSE specialization

#if SFIZZ_HAVE_SSE
template <>
class Interpolator<kInterpolatorHermite3, float>
{
public:
    static inline float process(const float* values, float coeff)
    {
        __m128 x = _mm_sub_ps(_mm_setr_ps(-1, 0, 1, 2), _mm_set1_ps(coeff));
        __m128 h = hermite3x4(x);
        __m128 y = _mm_mul_ps(h, _mm_loadu_ps(values - 1));
        // sum 4 to 1
        __m128 xmm0 = y;
        __m128 xmm1 = _mm_shuffle_ps(xmm0, xmm0, 0xe5);
        __m128 xmm2 = _mm_movehl_ps(xmm0, xmm0);
        xmm1 = _mm_add_ss(xmm1, xmm0);
        xmm0 = _mm_shuffle_ps(xmm0, xmm0, 0xe7);
        xmm2 = _mm_add_ss(xmm2, xmm1);
        xmm0 = _mm_add_ss(xmm0, xmm2);
        return _mm_cvtss_f32(xmm0);
    }
};
#endif

//------------------------------------------------------------------------------
// Hermite 3rd order, generic

template <class R>
class Interpolator<kInterpolatorHermite3, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        R y = 0;
        for (int i = -1; i < 3; ++i) {
            R h = hermite3<R>(i - coeff);
            y += h * values[i];
        }
        return y;
    }
};

//------------------------------------------------------------------------------
// B-spline 3rd order, SSE specialization

#if SFIZZ_HAVE_SSE
template <>
class Interpolator<kInterpolatorBspline3, float>
{
public:
    static inline float process(const float* values, float coeff)
    {
        __m128 x = _mm_sub_ps(_mm_setr_ps(-1, 0, 1, 2), _mm_set1_ps(coeff));
        __m128 h = bspline3x4(x);
        __m128 y = _mm_mul_ps(h, _mm_loadu_ps(values - 1));
        // sum 4 to 1
        __m128 xmm0 = y;
        __m128 xmm1 = _mm_shuffle_ps(xmm0, xmm0, 0xe5);
        __m128 xmm2 = _mm_movehl_ps(xmm0, xmm0);
        xmm1 = _mm_add_ss(xmm1, xmm0);
        xmm0 = _mm_shuffle_ps(xmm0, xmm0, 0xe7);
        xmm2 = _mm_add_ss(xmm2, xmm1);
        xmm0 = _mm_add_ss(xmm0, xmm2);
        return _mm_cvtss_f32(xmm0);
    }
};
#endif

//------------------------------------------------------------------------------
// B-spline 3rd order, generic

template <class R>
class Interpolator<kInterpolatorBspline3, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        R y = 0;
        for (int i = -1; i < 3; ++i) {
            R h = bspline3<R>(i - coeff);
            y += h * values[i];
        }
        return y;
    }
};

//------------------------------------------------------------------------------
// Windowed sinc

namespace SincInterpolatorDetail {
    // See sfizz wiki page "Resampling".
    constexpr size_t PointsMin = 8;
    constexpr size_t PointsMax = 72;

    // Adjust Kaiser window Beta as necessary.
    constexpr double BetaMin = 6.0;
    constexpr double BetaMax = 10.0;

    constexpr double getBetaForNumPoints(size_t points)
    {
        return BetaMin + (BetaMax - BetaMin) *
            (double(points - PointsMin) / double(PointsMax - PointsMin));
    }

    constexpr size_t getTableSizeForNumPoints(size_t /*points*/)
    {
        return 1u << 16;
    }
}

///
template <size_t Points>
struct SincInterpolatorTraits {
    static_assert(Points == 8  || Points == 12 || Points == 16 ||
                  Points == 24 || Points == 36 || Points == 48 ||
                  Points == 60 || Points == 72,
                  "Windowed sinc size is not acceptable");

    enum {
        TableSize = SincInterpolatorDetail::getTableSizeForNumPoints(Points)
    };

    static void initialize()
    {
        static const FixedWindowedSinc<Points, TableSize> globalInstance(
            SincInterpolatorDetail::getBetaForNumPoints(Points));
        windowedSinc = &globalInstance;
    }

    static const FixedWindowedSinc<Points, TableSize>* windowedSinc;
};

template <size_t Points>
const FixedWindowedSinc<Points, SincInterpolatorTraits<Points>::TableSize>*
SincInterpolatorTraits<Points>::windowedSinc = nullptr;

///
template <class R, size_t Points>
class SincInterpolator;

//------------------------------------------------------------------------------
// Windowed sinc any order, SSE specialization
#if SFIZZ_HAVE_SSE
template <size_t Points>
class SincInterpolator<float, Points>
{
public:
    static_assert(Points % 4 == 0, "Windowed sinc must be multiple of 4");

    static inline float process(const float* values, float coeff)
    {
        const auto &ws = *SincInterpolatorTraits<Points>::windowedSinc;

        int j0 = 1 - int(Points) / 2;

        __m128 h[Points / 4];
        for (int i = 0; i < int(Points); ++i)
            reinterpret_cast<float*>(h)[i] = ws.getUnchecked(j0 - coeff + i);

        __m128 y = _mm_mul_ps(h[0], _mm_loadu_ps(&values[j0]));
        for (int i = 1; i < int(Points / 4); ++i)
            y = _mm_add_ps(y, _mm_mul_ps(h[i], _mm_loadu_ps(&values[j0 + 4 * i])));

        // sum 4 to 1
        __m128 xmm0 = y;
        __m128 xmm1 = _mm_shuffle_ps(xmm0, xmm0, 0xe5);
        __m128 xmm2 = _mm_movehl_ps(xmm0, xmm0);
        xmm1 = _mm_add_ss(xmm1, xmm0);
        xmm0 = _mm_shuffle_ps(xmm0, xmm0, 0xe7);
        xmm2 = _mm_add_ss(xmm2, xmm1);
        xmm0 = _mm_add_ss(xmm0, xmm2);
        return _mm_cvtss_f32(xmm0);
    }
};
#endif

//------------------------------------------------------------------------------
// Windowed sinc any order, generic
template <class R, size_t Points>
class SincInterpolator
{
public:
    static inline R process(const R* values, R coeff)
    {
        const auto &ws = *SincInterpolatorTraits<Points>::windowedSinc;

        int j0 = 1 - int(Points) / 2;

        R h[Points];
        for (int i = 0; i < int(Points); ++i)
            h[i] = R(ws.getUnchecked(j0 - coeff + i));

        R y = h[0] * values[j0];
        for (int i = 1; i < int(Points); ++i)
            y += h[i] * values[j0 + i];

        return y;
    }
};

template <class R>
class Interpolator<kInterpolatorSinc8, R> : public SincInterpolator<R, 8> {};
template <class R>
class Interpolator<kInterpolatorSinc12, R> : public SincInterpolator<R, 12> {};
template <class R>
class Interpolator<kInterpolatorSinc16, R> : public SincInterpolator<R, 16> {};
template <class R>
class Interpolator<kInterpolatorSinc24, R> : public SincInterpolator<R, 24> {};
template <class R>
class Interpolator<kInterpolatorSinc36, R> : public SincInterpolator<R, 36> {};
template <class R>
class Interpolator<kInterpolatorSinc48, R> : public SincInterpolator<R, 48> {};
template <class R>
class Interpolator<kInterpolatorSinc60, R> : public SincInterpolator<R, 60> {};
template <class R>
class Interpolator<kInterpolatorSinc72, R> : public SincInterpolator<R, 72> {};

} // namespace sfz
