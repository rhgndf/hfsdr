#include "utils/complex_dsp.h"

namespace {

consteval bool complex_dsp_fast_10log10f_close(float x, float tol_db)
{
    float got = complex_dsp::fast_10log10f(x);
    float want = 10.0f * __builtin_log10f(x);
    float diff = got - want;
    return (diff > -tol_db) && (diff < tol_db);
}

static_assert(complex_dsp_fast_10log10f_close(1.0f,    0.005f));
static_assert(complex_dsp_fast_10log10f_close(10.0f,   0.005f));
static_assert(complex_dsp_fast_10log10f_close(100.0f,  0.005f));
static_assert(complex_dsp_fast_10log10f_close(0.1f,    0.005f));
static_assert(complex_dsp_fast_10log10f_close(0.5f,    0.005f));
static_assert(complex_dsp_fast_10log10f_close(1.5f,    0.005f));
static_assert(complex_dsp_fast_10log10f_close(1e-10f,  0.005f));
static_assert(complex_dsp_fast_10log10f_close(1e10f,   0.005f));

} // namespace
