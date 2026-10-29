#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <vector>

#include <FocalSpec.hpp>
#include <MapletCodec.hpp>

namespace Raster
{
/// Tile position -> decoded cells (all bands), nullptr if absent
using TileResolver = std::function<const GridView*(int32_t tx, int32_t ty)>;

struct FocalContext
{
    RasterGeometry geo;
    int32_t        outTileX = 0;   // position o of the output tile
    int32_t        outTileY = 0;
    TileResolver   resolve;        // (tx,ty) -> cells | nullptr
    double         noData = 0.0;
    uint32_t       band = 0;       // band read by resolveRead / written by evaluateInto; evaluateBands sets it
};

/// Raster geometry from a binary maplet preamble
inline RasterGeometry geometryOf(const MapletHeader& h)
{
    return RasterGeometry{h.rasterWidth, h.rasterHeight, static_cast<int32_t>(h.width), static_cast<int32_t>(h.height)};
}

namespace detail
{
inline int64_t reflectIndex(int64_t p, int64_t n)  // reflect-101 (edge not repeated)
{
    if (n == 1)
    {
        return 0;
    }
    const int64_t period = 2 * (n - 1);
    p = ((p % period) + period) % period;
    return p >= n ? period - p : p;
}

/// One neighbour read at (sx, sy); false = NoData (absent tile, NoData cell, off-raster under NoData)
inline bool resolveRead(const FocalContext& ctx, BoundaryRule beta, int64_t sx, int64_t sy, double& out)
{
    const auto& g = ctx.geo;
    if (sx < 0 || sy < 0 || sx >= g.rasterW || sy >= g.rasterH)
    {
        switch (beta)
        {
            case BoundaryRule::NoData:
                return false;
            case BoundaryRule::Clamp:
                sx = std::clamp<int64_t>(sx, 0, g.rasterW - 1);
                sy = std::clamp<int64_t>(sy, 0, g.rasterH - 1);
                break;
            case BoundaryRule::Reflect:
                sx = reflectIndex(sx, g.rasterW);
                sy = reflectIndex(sy, g.rasterH);
                break;
        }
    }
    const int32_t tx = static_cast<int32_t>(sx / g.tileW);
    const int32_t ty = static_cast<int32_t>(sy / g.tileH);
    const GridView* v = ctx.resolve(tx, ty);
    if (v == nullptr)  // owning on-grid tile absent -> ν
    {
        return false;
    }
    const uint32_t lc = static_cast<uint32_t>(sx - static_cast<int64_t>(tx) * g.tileW);
    const uint32_t lr = static_cast<uint32_t>(sy - static_cast<int64_t>(ty) * g.tileH);
    const double cell = v->at(ctx.band, lr, lc);
    if (v->isNoData(cell))  // NoData cell within a present tile -> skip
    {
        return false;
    }
    out = cell;
    return true;
}
}

namespace detail
{
/// Source index of p on an axis of length n under beta, -1 = NoData
inline int64_t sourceIndex(int64_t p, int64_t n, BoundaryRule beta)
{
    if (p >= 0 && p < n)
    {
        return p;
    }
    switch (beta)
    {
        case BoundaryRule::NoData:
            return -1;
        case BoundaryRule::Clamp:
            return std::clamp<int64_t>(p, 0, n - 1);
        case BoundaryRule::Reflect:
            return reflectIndex(p, n);
    }
    return -1;
}

/// Per-thread scratch, reused across tiles
struct HaloScratch
{
    std::vector<double> cells;
    std::vector<int32_t> colTile;
    std::vector<uint32_t> colLocal;
    std::vector<const GridView*> tileOf;
    std::vector<int32_t> tileOfRow;
    std::vector<int64_t> delta;
    std::vector<double> rowSum;
    std::vector<uint32_t> rowCnt;
};
inline HaloScratch& haloScratch()
{
    thread_local HaloScratch s;
    return s;
}

/// Halo of the output tile, (tileW + 2R) x (tileH + 2R): what resolveRead returns, NaN where it skips
inline void fillHalo(const FocalContext& ctx, BoundaryRule beta, int64_t x0, int64_t y0, int64_t haloW, int64_t haloH,
                     HaloScratch& s)
{
    const auto& g = ctx.geo;
    constexpr double skip = std::numeric_limits<double>::quiet_NaN();
    const auto w = static_cast<size_t>(haloW);
    s.cells.resize(w * static_cast<size_t>(haloH));
    s.colTile.resize(w);
    s.colLocal.resize(w);
    for (size_t i = 0; i < w; ++i)
    {
        const int64_t sx = sourceIndex(x0 + static_cast<int64_t>(i), g.rasterW, beta);
        const int32_t tx = sx < 0 ? -1 : static_cast<int32_t>(sx / g.tileW);
        s.colTile[i] = tx;
        s.colLocal[i] = sx < 0 ? 0 : static_cast<uint32_t>(sx - static_cast<int64_t>(tx) * g.tileW);
    }
    s.tileOf.assign(static_cast<size_t>(g.tilesX()), nullptr);
    s.tileOfRow.assign(static_cast<size_t>(g.tilesX()), -1);
    for (int64_t j = 0; j < haloH; ++j)
    {
        double* dst = s.cells.data() + static_cast<size_t>(j) * w;
        const int64_t sy = sourceIndex(y0 + j, g.rasterH, beta);
        if (sy < 0)
        {
            std::fill(dst, dst + w, skip);
            continue;
        }
        const int32_t ty = static_cast<int32_t>(sy / g.tileH);
        const uint32_t lr = static_cast<uint32_t>(sy - static_cast<int64_t>(ty) * g.tileH);
        for (size_t i = 0; i < w; ++i)
        {
            if (s.colTile[i] < 0)
            {
                dst[i] = skip;
                continue;
            }
            const auto tx = static_cast<size_t>(s.colTile[i]);
            if (s.tileOfRow[tx] != ty)
            {
                s.tileOf[tx] = ctx.resolve(s.colTile[i], ty);
                s.tileOfRow[tx] = ty;
            }
            const GridView* v = s.tileOf[tx];
            if (v == nullptr)
            {
                dst[i] = skip;
                continue;
            }
            const double cell = v->at(ctx.band, lr, s.colLocal[i]);
            dst[i] = v->isNoData(cell) ? skip : cell;
        }
    }
}
}

/// Evaluate band ctx.band of the output tile at o into `out` (t_w*t_h cells, row-major: one plane).
template <typename Reducer>
void evaluateInto(const FocalContext& ctx, const Neighbourhood& nbh, BoundaryRule beta, Reducer& reducer, double* out)
{
    const auto& g = ctx.geo;
    int32_t reach = 0;
    for (const Offset& off : nbh.offsets)
    {
        reach = std::max({reach, std::abs(off.dx), std::abs(off.dy)});
    }
    const int64_t haloW = static_cast<int64_t>(g.tileW) + 2 * reach;
    const int64_t haloH = static_cast<int64_t>(g.tileH) + 2 * reach;
    const int64_t x0 = static_cast<int64_t>(ctx.outTileX) * g.tileW;
    const int64_t y0 = static_cast<int64_t>(ctx.outTileY) * g.tileH;

    auto& s = detail::haloScratch();
    detail::fillHalo(ctx, beta, x0 - reach, y0 - reach, haloW, haloH, s);
    s.delta.resize(nbh.offsets.size());
    for (size_t k = 0; k < nbh.offsets.size(); ++k)
    {
        s.delta[k] = static_cast<int64_t>(nbh.offsets[k].dy) * haloW + nbh.offsets[k].dx;
    }
    const int64_t* delta = s.delta.data();
    const Offset* offsets = nbh.offsets.data();
    const size_t n = nbh.offsets.size();

    const int32_t validCols = static_cast<int32_t>(std::clamp<int64_t>(g.rasterW - x0, 0, g.tileW));
    const int32_t validRows = static_cast<int32_t>(std::clamp<int64_t>(g.rasterH - y0, 0, g.tileH));
    const auto tileW = static_cast<size_t>(g.tileW);
    std::fill(out, out + tileW * static_cast<size_t>(g.tileH), ctx.noData);  // padding region of an edge tile
    for (int32_t r = 0; r < validRows; ++r)
    {
        const double* row = s.cells.data() + static_cast<size_t>(((r + reach) * haloW) + reach);
        double* cell = out + (static_cast<size_t>(r) * tileW);
        for (int32_t c = 0; c < validCols; ++c)
        {
            const double* centre = row + c;
            reducer.reset();
            for (size_t k = 0; k < n; ++k)
            {
                const double v = centre[delta[k]];
                if (v == v)
                {
                    reducer.accept(v, offsets[k].dx, offsets[k].dy);
                }
            }
            cell[c] = reducer.finalize(ctx.noData);
        }
    }
}

/// R if nbh is the dense (2R+1)^2 square, else -1
inline int32_t denseSquareRadius(const Neighbourhood& nbh)
{
    int32_t reach = 0;
    for (const Offset& off : nbh.offsets)
    {
        reach = std::max({reach, std::abs(off.dx), std::abs(off.dy)});
    }
    const auto side = static_cast<size_t>(2 * reach + 1);
    if (nbh.offsets.size() != side * side)
    {
        return -1;
    }
    std::vector<bool> seen(side * side, false);
    for (const Offset& off : nbh.offsets)
    {
        const size_t i = (static_cast<size_t>(off.dy + reach) * side) + static_cast<size_t>(off.dx + reach);
        if (seen[i])
        {
            return -1;
        }
        seen[i] = true;
    }
    return reach;
}

namespace detail
{
#if defined(__FAST_MATH__)
#error "Summation::Kahan needs IEEE semantics: -ffast-math lets the compiler remove the compensation"
#endif
/// Compensated: Kahan-Babuska (Neumaier), also exact when a value larger than the sum enters
template <bool Compensated>
struct RunningSum
{
    double sum = 0.0;
    double comp = 0.0;
    void add(double v)
    {
        if constexpr (Compensated)
        {
            const double t = sum + v;
            comp += std::fabs(sum) >= std::fabs(v) ? (sum - t) + v : (v - t) + sum;
            sum = t;
        }
        else
        {
            sum += v;
        }
    }
    [[nodiscard]] double value() const
    {
        if constexpr (Compensated)
        {
            return sum + comp;
        }
        else
        {
            return sum;
        }
    }
};
}

/// Sum/Mean over the dense square of radius R: separable running sums on the halo, O(1) per cell
template <bool Mean, bool Compensated>
void evaluateRunningInto(const FocalContext& ctx, int32_t reach, BoundaryRule beta, double* out)
{
    const auto& g = ctx.geo;
    const int64_t haloW = static_cast<int64_t>(g.tileW) + 2 * reach;
    const int64_t haloH = static_cast<int64_t>(g.tileH) + 2 * reach;
    const int64_t x0 = static_cast<int64_t>(ctx.outTileX) * g.tileW;
    const int64_t y0 = static_cast<int64_t>(ctx.outTileY) * g.tileH;
    auto& s = detail::haloScratch();
    detail::fillHalo(ctx, beta, x0 - reach, y0 - reach, haloW, haloH, s);

    const auto tileW = static_cast<size_t>(g.tileW);
    std::fill(out, out + tileW * static_cast<size_t>(g.tileH), ctx.noData);  // padding region of an edge tile
    const auto cols = static_cast<size_t>(std::clamp<int64_t>(g.rasterW - x0, 0, g.tileW));
    const auto rows = static_cast<size_t>(std::clamp<int64_t>(g.rasterH - y0, 0, g.tileH));
    if (cols == 0 || rows == 0)
    {
        return;
    }
    const auto side = static_cast<size_t>(2 * reach + 1);
    const size_t hRows = rows + side - 1;
    s.rowSum.resize(hRows * cols);
    s.rowCnt.resize(hRows * cols);

    // horizontal pass: window [c, c + 2R] of halo row j
    for (size_t j = 0; j < hRows; ++j)
    {
        const double* h = s.cells.data() + (j * static_cast<size_t>(haloW));
        detail::RunningSum<Compensated> acc;
        uint32_t n = 0;
        for (size_t x = 0; x < side; ++x)
        {
            if (h[x] == h[x]) { acc.add(h[x]); ++n; }
        }
        for (size_t c = 0;; ++c)
        {
            s.rowSum[(j * cols) + c] = acc.value();
            s.rowCnt[(j * cols) + c] = n;
            if (c + 1 == cols)
            {
                break;
            }
            const double enter = h[c + side];
            const double leave = h[c];
            if (enter == enter) { acc.add(enter); ++n; }
            if (leave == leave) { acc.add(-leave); --n; }
        }
    }

    // vertical pass over the row sums
    for (size_t c = 0; c < cols; ++c)
    {
        detail::RunningSum<Compensated> acc;
        uint32_t n = 0;
        auto rowValue = [&](size_t j) { return s.rowSum[(j * cols) + c]; };
        for (size_t j = 0; j < side; ++j)
        {
            acc.add(rowValue(j));
            n += s.rowCnt[(j * cols) + c];
        }
        for (size_t r = 0;; ++r)
        {
            double& cell = out[(r * tileW) + c];
            if (n > 0)
            {
                const double total = acc.value();
                cell = Mean ? total / static_cast<double>(n) : total;
            }
            else
            {
                cell = ctx.noData;
            }
            if (r + 1 == rows)
            {
                break;
            }
            acc.add(rowValue(r + side));
            acc.add(-rowValue(r));
            n += s.rowCnt[((r + side) * cols) + c];
            n -= s.rowCnt[(r * cols) + c];
        }
    }
}

/// evaluateRunningInto per band (band-major)
template <bool Mean, bool Compensated>
void evaluateRunningBands(FocalContext ctx, uint32_t bands, int32_t reach, BoundaryRule beta, double* out)
{
    const size_t planeCells = static_cast<size_t>(ctx.geo.tileW) * static_cast<size_t>(ctx.geo.tileH);
    for (uint32_t b = 0; b < bands; ++b)
    {
        ctx.band = b;
        evaluateRunningInto<Mean, Compensated>(ctx, reach, beta, out + (b * planeCells));
    }
}

/// evaluateInto per band (band-major); bands are independent
template <typename Reducer>
void evaluateBands(FocalContext ctx, uint32_t bands, const Neighbourhood& nbh, BoundaryRule beta, Reducer& reducer, double* out)
{
    const size_t planeCells = static_cast<size_t>(ctx.geo.tileW) * static_cast<size_t>(ctx.geo.tileH);
    for (uint32_t b = 0; b < bands; ++b)
    {
        ctx.band = b;
        evaluateInto(ctx, nbh, beta, reducer, out + (b * planeCells));
    }
}
}