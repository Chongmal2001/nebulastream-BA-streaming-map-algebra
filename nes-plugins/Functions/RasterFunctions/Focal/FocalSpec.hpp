#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Raster
{
enum class ReductionOp  : uint8_t { Mean, Max, Min, Sum };
enum class BoundaryRule : uint8_t { NoData, Clamp, Reflect };
/// Sum/Mean on dense squares: Exact (default), Running sums, Kahan (compensated running sums)
enum class Summation : uint8_t { Exact, Running, Kahan };

inline std::optional<Summation> parseSummation(std::string_view name)
{
    if (name.empty() || name == "exact") return Summation::Exact;
    if (name == "running") return Summation::Running;
    if (name == "kahan") return Summation::Kahan;
    return std::nullopt;
}

struct Offset { int32_t dx; int32_t dy; };

/// Cells read per output cell, as signed offsets
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
    Summation     summation = Summation::Exact;
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

/// Disc of radius r
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

/// Tiles the neighbourhood spans in each direction
struct TileReach { int32_t horizontal; int32_t vertical; };
inline TileReach computeTileReach(const RasterGeometry& g, int32_t radius)
{
    return {(radius + g.tileW - 1) / g.tileW, (radius + g.tileH - 1) / g.tileH};
}

/// Calls fn on every existing neighbour tile of the centre tile
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

/// Tile position in tile indices
struct TilePosition { int32_t x; int32_t y; };

[[nodiscard]] inline size_t tileIndex(const RasterGeometry& g, TilePosition pos)
{
    return static_cast<size_t>(pos.y) * g.tilesX() + pos.x;
}
}