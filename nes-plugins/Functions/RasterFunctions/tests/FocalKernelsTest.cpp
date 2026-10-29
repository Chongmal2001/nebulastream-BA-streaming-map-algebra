#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include <FocalKernels.hpp>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>

using namespace Raster;

namespace
{
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/// One 3x3 tile that is the whole raster: band 0 = 1..9, band 1 = 10x band 0 with a NoData cell at (1,1).
struct SingleTile
{
    std::vector<double> cells;
    GridView view;
    SingleTile() : cells(18)
    {
        for (int i = 0; i < 9; ++i)
        {
            cells[i] = i + 1;
            cells[9 + i] = 10.0 * (i + 1);
        }
        cells[9 + 4] = NaN;
        view = GridView{cells.data(), 3, 3, NaN, 2};
    }
    FocalContext context() const
    {
        return FocalContext{RasterGeometry{3, 3, 3, 3}, 0, 0, [this](int32_t x, int32_t y) { return x == 0 && y == 0 ? &view : nullptr; }, NaN};
    }
};

FocalSpec spec(ReductionOp op)
{
    return FocalSpec{makeDenseSquare(1), op, BoundaryRule::NoData};
}
}

TEST(FocalKernelsTest, CentreCellOfEveryReductionPerBand)
{
    const SingleTile tile;
    std::vector<double> out(18);
    // Centre cell (1,1): band 0 sees 1..9; band 1 sees 10..90 without its NoData centre (50).
    const struct { ReductionOp op; double band0; double band1; } cases[] = {
        {ReductionOp::Sum, 45.0, 400.0},
        {ReductionOp::Mean, 5.0, 50.0},
        {ReductionOp::Max, 9.0, 90.0},
        {ReductionOp::Min, 1.0, 10.0},
    };
    for (const auto& c : cases)
    {
        ASSERT_EQ(evaluateTile(spec(c.op), tile.context(), out.data(), out.size(), 2), FocalStatus::Ok);
        EXPECT_EQ(out[4], c.band0);
        EXPECT_EQ(out[9 + 4], c.band1);
    }
}

TEST(FocalKernelsTest, CornerSeesOnlyItsNeighbourhood)
{
    const SingleTile tile;
    std::vector<double> out(18);
    ASSERT_EQ(evaluateTile(spec(ReductionOp::Sum), tile.context(), out.data(), out.size(), 2), FocalStatus::Ok);
    EXPECT_EQ(out[0], 1.0 + 2 + 4 + 5);          // band 0, (0,0): off-raster reads are NoData under BoundaryRule::NoData
    EXPECT_EQ(out[9 + 0], 10.0 + 20 + 40);       // band 1, (0,0): the NoData centre cell is skipped
}

TEST(FocalKernelsTest, DefaultIsOneBand)
{
    const SingleTile tile;
    std::vector<double> out(9);
    ASSERT_EQ(evaluateTile(spec(ReductionOp::Sum), tile.context(), out.data(), out.size()), FocalStatus::Ok);
    EXPECT_EQ(out[4], 45.0);
}

TEST(FocalKernelsTest, RejectsBadInput)
{
    const SingleTile tile;
    std::vector<double> out(18);
    EXPECT_EQ(evaluateTile(spec(ReductionOp::Sum), tile.context(), out.data(), 17, 2), FocalStatus::OutputTooSmall);   // needs 2*9
    EXPECT_EQ(evaluateTile(spec(ReductionOp::Sum), tile.context(), out.data(), out.size(), 0), FocalStatus::BadBands);
    auto ctx = tile.context();
    ctx.geo.tileW = 0;
    EXPECT_EQ(evaluateTile(spec(ReductionOp::Sum), ctx, out.data(), out.size(), 2), FocalStatus::BadGeometry);
}