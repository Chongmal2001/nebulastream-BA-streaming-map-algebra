#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>

namespace Raster
{
/// Resolves a tile position to its decoded cells, or nullptr if absent in this
/// window. The operator supplies this; the kernel stays ignorant of the store.
using TileResolver = std::function<const GridView*(int32_t tx, int32_t ty)>;

struct FocalContext
{
    RasterGeometry geo;
    int32_t        outTileX = 0;   // position o of the output tile
    int32_t        outTileY = 0;
    TileResolver   resolve;        // (tx,ty) -> cells | nullptr
    double         noData = 0.0;
};

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

/// Resolve one neighbourhood read at global pixel (sx,sy).
/// Returns false when the read yields NoData (absent tile, NoData cell, or
/// β=NoData off-raster); the reducer then skips it. This "skip NoData" policy
/// is the semantic that must be aligned with GeoTrellis — flag it in the thesis.
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
    const double cell = v->at(lr, lc);
    if (v->isNoData(cell))  // NoData cell within a present tile -> skip
    {
        return false;
    }
    out = cell;
    return true;
}
}

/// Evaluate the output tile at o into `out` (t_w*t_h cells, row-major).
template <typename Reducer>
void evaluateInto(const FocalContext& ctx, const Neighbourhood& nbh, BoundaryRule beta, Reducer& reducer, double* out)
{
    const auto& g = ctx.geo;
    for (int32_t r = 0; r < g.tileH; ++r)
    {
        for (int32_t c = 0; c < g.tileW; ++c)
        {
            const int64_t gx = static_cast<int64_t>(ctx.outTileX) * g.tileW + c;
            const int64_t gy = static_cast<int64_t>(ctx.outTileY) * g.tileH + r;
            double* cell = &out[static_cast<size_t>(r) * g.tileW + c];

            if (gx >= g.rasterW || gy >= g.rasterH)  // padding region of an edge tile
            {
                *cell = ctx.noData;
                continue;
            }
            reducer.reset();
            for (const Offset& off : nbh.offsets)
            {
                double v;
                if (detail::resolveRead(ctx, beta, gx + off.dx, gy + off.dy, v))
                {
                    reducer.accept(v, off.dx, off.dy);
                }
            }
            *cell = reducer.finalize(ctx.noData);
        }
    }
}
}