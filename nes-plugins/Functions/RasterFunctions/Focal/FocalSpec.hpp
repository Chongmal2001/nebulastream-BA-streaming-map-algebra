#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace Raster
{
enum class ReductionOp  : uint8_t { Mean, Max, Min, Sum };
enum class BoundaryRule : uint8_t { NoData, Clamp, Reflect };

struct Offset { int32_t dx; int32_t dy; };

/// Neighbourhood: which input cells the kernel reads for each output cell, as signed cell offsets
struct Neighbourhood
{
    std::vector<Offset> offsets;
    int32_t radius = 0;
};

struct FocalSpec
{
    Neighbourhood n;
    ReductionOp   kappa = ReductionOp::Mean;
    BoundaryRule  beta  = BoundaryRule::NoData;
};

/// The locator fields the kernel needs
struct RasterGeometry
{
    int64_t rasterW = 0;
    int64_t rasterH = 0;
    int32_t tileW   = 0;  // tile width
    int32_t tileH   = 0;  // tile height
    [[nodiscard]] int32_t tilesX() const { return static_cast<int32_t>((rasterW + tileW - 1) / tileW); }
    [[nodiscard]] int32_t tilesY() const { return static_cast<int32_t>((rasterH + tileH - 1) / tileH); }
};

/// Dense (2r+1)*(2r+1) kernel (default)
inline Neighbourhood makeDenseSquare(int32_t r)
{
    Neighbourhood nbh;
    nbh.radius = r;
    nbh.offsets.reserve(static_cast<size_t>(2 * r + 1) * (2 * r + 1));
    for (int32_t dy = -r; dy <= r; ++dy)
    {
        for (int32_t dx = -r; dx <= r; ++dx)
        {
            nbh.offsets.push_back({dx, dy});
        }
    }
    return nbh;
}

/// Disc of radius r (proves the shape-agnostic path)
inline Neighbourhood makeDisc(int32_t r)
{
    Neighbourhood nbh;
    nbh.radius = r;
    const int64_t r2 = static_cast<int64_t>(r) * r;
    for (int32_t dy = -r; dy <= r; ++dy)
    {
        for (int32_t dx = -r; dx <= r; ++dx)
        {
            if (static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy <= r2)
            {
                nbh.offsets.push_back({dx, dy});
            }
        }
    }
    return nbh;
}

/// Converts a cell-level kernel radius into tile-level reach => how many tiles the neighbourhood spans in each direction
struct TileReach { int32_t horizontal; int32_t vertical; };
inline TileReach computeTileReach(const RasterGeometry& g, int32_t radius)
{
    return {(radius + g.tileW - 1) / g.tileW, (radius + g.tileH - 1) / g.tileH};
}

/// Calls a function on every existing neighbour tile of the centre tile
template <typename Fn>
void forEachNeighbourTile(const RasterGeometry& g, int32_t centreTileX, int32_t centreTileY, TileReach reach, Fn&& fn)
{
    const int32_t x0 = std::max(0, centreTileX - reach.horizontal);
    const int32_t x1 = std::min(g.tilesX() - 1, centreTileX + reach.horizontal);
    const int32_t y0 = std::max(0, centreTileY - reach.vertical);
    const int32_t y1 = std::min(g.tilesY() - 1, centreTileY + reach.vertical);
    for (int32_t ty = y0; ty <= y1; ++ty)
        for (int32_t tx = x0; tx <= x1; ++tx)
            fn(tx, ty);
}

/// Position of a tile within its raster in tile indices, not cell offsets
struct TilePosition { int32_t x; int32_t y; };

[[nodiscard]] inline size_t tileIndex(const RasterGeometry& g, TilePosition pos)
{
    return static_cast<size_t>(pos.y) * g.tilesX() + pos.x;
}
}