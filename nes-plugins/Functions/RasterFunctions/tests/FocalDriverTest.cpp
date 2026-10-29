#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <FocalDriver.hpp>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>

using namespace Raster;

namespace
{
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

struct SumReducer
{
    double sum = 0;
    int count = 0;
    void reset() { sum = 0; count = 0; }
    void accept(double v, int32_t, int32_t) { sum += v; ++count; }
    double finalize(double noData) const { return count > 0 ? sum : noData; }
};

bool sameCell(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

/// A 5x3 raster in 2x2 tiles (3x2 grid, padded edge tiles), two bands with different values, one NoData cell in band 1.
struct TwoBandRaster
{
    static constexpr int W = 5, H = 3, T = 2;
    RasterGeometry geo{W, H, T, T};
    std::map<std::pair<int32_t, int32_t>, std::vector<double>> both, band0, band1;
    std::map<std::pair<int32_t, int32_t>, GridView> viewBoth, view0, view1;

    TwoBandRaster()
    {
        for (int32_t ty = 0; ty < geo.tilesY(); ++ty)
            for (int32_t tx = 0; tx < geo.tilesX(); ++tx)
            {
                std::vector<double> b(2 * T * T), p0(T * T), p1(T * T);
                for (int r = 0; r < T; ++r)
                    for (int c = 0; c < T; ++c)
                    {
                        const int gx = tx * T + c, gy = ty * T + r;
                        const bool inside = gx < W && gy < H;
                        p0[r * T + c] = inside ? gx + 10.0 * gy : NaN;
                        p1[r * T + c] = inside && !(gx == 1 && gy == 1) ? -3.5 * gx + gy : NaN;
                        b[r * T + c] = p0[r * T + c];
                        b[T * T + r * T + c] = p1[r * T + c];
                    }
                both[{tx, ty}] = b;
                band0[{tx, ty}] = p0;
                band1[{tx, ty}] = p1;
            }
        for (auto& [k, c] : both) viewBoth[k] = GridView{c.data(), T, T, NaN, 2};
        for (auto& [k, c] : band0) view0[k] = GridView{c.data(), T, T, NaN, 1};
        for (auto& [k, c] : band1) view1[k] = GridView{c.data(), T, T, NaN, 1};
    }

    static TileResolver resolver(const std::map<std::pair<int32_t, int32_t>, GridView>& views)
    {
        return [&views](int32_t x, int32_t y) -> const GridView*
        {
            const auto it = views.find({x, y});
            return it == views.end() ? nullptr : &it->second;
        };
    }
};
}

TEST(FocalDriverTest, TwoBandTileEqualsTwoSingleBandRuns)
{
    const TwoBandRaster raster;
    constexpr int T = TwoBandRaster::T;
    for (const auto& nbh : {makeDenseSquare(1), makeDisc(2)})
        for (const auto beta : {BoundaryRule::NoData, BoundaryRule::Clamp, BoundaryRule::Reflect})
            for (int32_t ty = 0; ty < raster.geo.tilesY(); ++ty)
                for (int32_t tx = 0; tx < raster.geo.tilesX(); ++tx)
                {
                    SumReducer reducer;
                    std::vector<double> both(2 * T * T), only0(T * T), only1(T * T);
                    evaluateBands(FocalContext{raster.geo, tx, ty, TwoBandRaster::resolver(raster.viewBoth), NaN}, 2, nbh, beta, reducer,
                                  both.data());
                    evaluateInto(FocalContext{raster.geo, tx, ty, TwoBandRaster::resolver(raster.view0), NaN}, nbh, beta, reducer,
                                 only0.data());
                    evaluateInto(FocalContext{raster.geo, tx, ty, TwoBandRaster::resolver(raster.view1), NaN}, nbh, beta, reducer,
                                 only1.data());
                    for (int i = 0; i < T * T; ++i)
                    {
                        EXPECT_TRUE(sameCell(both[i], only0[i])) << "band 0, tile (" << tx << "," << ty << "), cell " << i;
                        EXPECT_TRUE(sameCell(both[T * T + i], only1[i])) << "band 1, tile (" << tx << "," << ty << "), cell " << i;
                    }
                }
}

TEST(FocalDriverTest, SingleBandCallersStillReadBandZero)
{
    const TwoBandRaster raster;
    constexpr int T = TwoBandRaster::T;
    SumReducer reducer;
    std::vector<double> viaDefault(T * T), viaBands(2 * T * T);
    // A context initialised the old way (no band) reads band 0 of a multiband tile.
    evaluateInto(FocalContext{raster.geo, 1, 0, TwoBandRaster::resolver(raster.viewBoth), NaN}, makeDenseSquare(1), BoundaryRule::NoData,
                 reducer, viaDefault.data());
    evaluateBands(FocalContext{raster.geo, 1, 0, TwoBandRaster::resolver(raster.viewBoth), NaN}, 2, makeDenseSquare(1),
                  BoundaryRule::NoData, reducer, viaBands.data());
    for (int i = 0; i < T * T; ++i)
    {
        EXPECT_TRUE(sameCell(viaDefault[i], viaBands[i]));
    }
}

TEST(FocalDriverTest, GeometryFromMapletHeader)
{
    MapletHeader h;
    h.width = 3;
    h.height = 2;
    h.rasterWidth = 4;
    h.rasterHeight = 3;
    const auto g = geometryOf(h);
    EXPECT_EQ(g.tileW, 3);
    EXPECT_EQ(g.tileH, 2);
    EXPECT_EQ(g.tilesX(), 2);
    EXPECT_EQ(g.tilesY(), 2);
}