#pragma once
#include <cstdint>
#include <limits>

namespace Raster
{

struct MeanReducer
{
    double sum = 0.0;
    uint64_t n = 0;
    void reset() { sum = 0.0; n = 0; }
    void accept(double v, int32_t, int32_t) { sum += v; ++n; }
    [[nodiscard]] double finalize(double noData) const { return n ? sum / static_cast<double>(n) : noData; }
};

struct SumReducer
{
    double sum = 0.0;
    uint64_t n = 0;
    void reset() { sum = 0.0; n = 0; }
    void accept(double v, int32_t, int32_t) { sum += v; ++n; }
    [[nodiscard]] double finalize(double noData) const { return n ? sum : noData; }
};

struct MaxReducer
{
    double m = -std::numeric_limits<double>::infinity();
    uint64_t n = 0;
    void reset() { m = -std::numeric_limits<double>::infinity(); n = 0; }
    void accept(double v, int32_t, int32_t) { m = v > m ? v : m; ++n; }
    [[nodiscard]] double finalize(double noData) const { return n ? m : noData; }
};

struct MinReducer
{
    double m = std::numeric_limits<double>::infinity();
    uint64_t n = 0;
    void reset() { m = std::numeric_limits<double>::infinity(); n = 0; }
    void accept(double v, int32_t, int32_t) { m = v < m ? v : m; ++n; }
    [[nodiscard]] double finalize(double noData) const { return n ? m : noData; }
};
}