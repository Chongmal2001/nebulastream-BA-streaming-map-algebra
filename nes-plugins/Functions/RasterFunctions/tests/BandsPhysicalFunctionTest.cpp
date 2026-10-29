/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        https://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include <BandsPhysicalFunction.hpp>
#include <MapletCodec.hpp>
#include "MapletGolden.hpp"

using namespace NES;
using namespace RasterGolden;

namespace
{
template <std::size_t N>
std::vector<int8_t> copyOf(const std::array<uint8_t, N>& a)
{
    return {reinterpret_cast<const int8_t*>(a.data()), reinterpret_cast<const int8_t*>(a.data()) + N};
}

/// Runs the kernels the way execute() does, without the JIT.
std::vector<int8_t> selectBands(std::vector<int8_t> input, const std::vector<uint32_t>& bands)
{
    const auto k = static_cast<uint32_t>(bands.size());
    std::vector<int8_t> output(BandsKernels::outputSize(input.data(), input.size(), k));
    BandsKernels::writePreamble(input.data(), output.data(), k);
    for (uint32_t slot = 0; slot < k; ++slot)
    {
        BandsKernels::copyPlane(input.data(), input.size(), output.data(), slot, bands[slot]);
    }
    return output;
}

Raster::GridView decode(const std::vector<int8_t>& maplet, uint8_t bands, std::vector<double>& scratch, Raster::MapletHeader& header)
{
    Raster::TileSpec spec{Raster::WireFormat::Binary, Raster::CellType::Float64, 3, 2, std::numeric_limits<double>::quiet_NaN()};
    spec.bands = bands;
    scratch.assign(spec.cellCount(), 0.0);
    Raster::GridView view;
    EXPECT_EQ(Raster::decode(maplet.data(), maplet.size(), spec, scratch.data(), scratch.size(), view, &header), Raster::CodecStatus::Ok);
    return view;
}
}

TEST(BandsPhysicalFunctionTest, SelectsOneBandAndKeepsPlacement)
{
    const auto out = selectBands(copyOf(GOLDEN_2BAND), {1});
    ASSERT_EQ(out.size(), Raster::BINARY_HEADER_SIZE + (6 * sizeof(double)));
    std::vector<double> scratch;
    Raster::MapletHeader header;
    const auto view = decode(out, 1, scratch, header);
    EXPECT_EQ(view.at(0, 0), -1.0);    // band 1 of the golden
    EXPECT_EQ(view.at(1, 2), 30.0);
    EXPECT_EQ(header.frameId, 7U);     // preamble travels unchanged apart from the band count
    EXPECT_EQ(header.tileX, 1);
    EXPECT_EQ(header.rasterWidth, 6U);
}

TEST(BandsPhysicalFunctionTest, ReordersBands)
{
    const auto out = selectBands(copyOf(GOLDEN_2BAND), {1, 0});
    std::vector<double> scratch;
    Raster::MapletHeader header;
    const auto view = decode(out, 2, scratch, header);
    EXPECT_EQ(view.at(0, 0, 0), -1.0);
    EXPECT_EQ(view.at(1, 1, 2), 6.0);
}

TEST(BandsPhysicalFunctionTest, IdentitySelectionIsByteIdentical)
{
    const auto input = copyOf(GOLDEN_2BAND);
    EXPECT_EQ(selectBands(input, {0, 1}), input);
}

TEST(BandsPhysicalFunctionTest, NestedSelectionEqualsDirectSelection)
{
    // BANDS(BANDS(x, 1, 0), 0) = BANDS(x, 1)
    const auto inner = selectBands(copyOf(GOLDEN_2BAND), {1, 0});
    EXPECT_EQ(selectBands(inner, {0}), selectBands(copyOf(GOLDEN_2BAND), {1}));
}

TEST(BandsPhysicalFunctionTest, RejectsMissingBandAndMalformedInput)
{
    EXPECT_ANY_THROW(selectBands(copyOf(GOLDEN_2BAND), {2}));       // only bands 0 and 1 exist
    auto oldFormat = copyOf(GOLDEN_1BAND);
    oldFormat[0] = 2;
    EXPECT_ANY_THROW(selectBands(oldFormat, {0}));
    auto truncated = copyOf(GOLDEN_1BAND);
    truncated.pop_back();
    EXPECT_ANY_THROW(selectBands(truncated, {0}));
    std::vector<int8_t> text{'1', ' ', '2'};
    EXPECT_ANY_THROW(selectBands(text, {0}));
}