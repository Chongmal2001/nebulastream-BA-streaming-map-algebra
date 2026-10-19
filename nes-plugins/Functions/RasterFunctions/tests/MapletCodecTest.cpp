#include <MapletCodec.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Raster;

namespace
{
TileSpec textSpec(uint32_t h, uint32_t w, double nd = std::numeric_limits<double>::quiet_NaN())
{
    return TileSpec{WireFormat::Text, CellType::Float64, w, h, nd};
}

// decode a text blob, return the cells (fails the test on codec error)
std::vector<double> decodeText(const std::string& blob, const TileSpec& spec)
{
    std::vector<double> scratch(spec.cellCount());
    GridView v;
    auto st = decode(reinterpret_cast<const int8_t*>(blob.data()), blob.size(),
                     spec, scratch.data(), scratch.size(), v);
    EXPECT_EQ(st, CodecStatus::Ok);
    return {v.cells, v.cells + v.cellCount()};
}
}  // namespace

TEST(MapletCodec, TextDecodeRowMajor)
{
    auto spec = textSpec(2, 2);
    auto c = decodeText("12.0 13.5 NaN 14.0", spec);
    EXPECT_DOUBLE_EQ(c[0], 12.0);
    EXPECT_DOUBLE_EQ(c[1], 13.5);
    EXPECT_TRUE(std::isnan(c[2]));   // NaN sentinel
    EXPECT_DOUBLE_EQ(c[3], 14.0);
}

TEST(MapletCodec, TextTooFewTokens)
{
    auto spec = textSpec(2, 2);
    std::string blob = "1 2 3";
    std::vector<double> s(4);
    GridView v;
    auto st = decode(reinterpret_cast<const int8_t*>(blob.data()), blob.size(),
                     spec, s.data(), s.size(), v);
    EXPECT_EQ(st, CodecStatus::TruncatedInput);
}

TEST(MapletCodec, TextTooManyTokens)
{
    auto spec = textSpec(2, 2);
    std::string blob = "1 2 3 4 5";
    std::vector<double> s(4);
    GridView v;
    auto st = decode(reinterpret_cast<const int8_t*>(blob.data()), blob.size(),
                     spec, s.data(), s.size(), v);
    EXPECT_EQ(st, CodecStatus::SizeMismatch);
}

TEST(MapletCodec, ScratchTooSmall)
{
    auto spec = textSpec(2, 2);
    std::string blob = "1 2 3 4";
    std::vector<double> s(3);   // one short
    GridView v;
    auto st = decode(reinterpret_cast<const int8_t*>(blob.data()), blob.size(),
                     spec, s.data(), s.size(), v);
    EXPECT_EQ(st, CodecStatus::ScratchTooSmall);
}

TEST(MapletCodec, MalformedNumber)
{
    auto spec = textSpec(1, 2);
    std::string blob = "1 2x";
    std::vector<double> s(2);
    GridView v;
    auto st = decode(reinterpret_cast<const int8_t*>(blob.data()), blob.size(),
                     spec, s.data(), s.size(), v);
    EXPECT_EQ(st, CodecStatus::MalformedNumber);
}

// The one people skip: encode->decode must be bit-exact on awkward doubles.
TEST(MapletCodec, TextRoundTripFidelity)
{
    auto spec = textSpec(1, 4);
    std::vector<double> in = {0.1, 1e308, std::numeric_limits<double>::denorm_min(), -0.0};
    GridView src{in.data(), 1, 4, spec.noData};

    std::vector<int8_t> out(encodedSizeTextUpperBound(spec));
    uint64_t n = 0;
    ASSERT_EQ(encode(src, spec, out.data(), out.size(), n), CodecStatus::Ok);

    std::vector<double> scratch(4);
    GridView back;
    ASSERT_EQ(decode(out.data(), n, spec, scratch.data(), scratch.size(), back), CodecStatus::Ok);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(std::bit_cast<uint64_t>(in[i]), std::bit_cast<uint64_t>(back.cells[i]))
            << "cell " << i;   // bit-exact, so -0.0 and denormals are caught
    }
}

TEST(MapletCodec, BinaryRoundTrip)
{
    TileSpec spec{WireFormat::Binary, CellType::Float64, 2, 2, -9999.0};
    std::vector<double> in = {1.0, 2.0, 3.0, 4.0};
    GridView src{in.data(), 2, 2, spec.noData};

    std::vector<int8_t> out(encodedSizeBinary(spec));
    uint64_t n = 0;
    ASSERT_EQ(encode(src, spec, out.data(), out.size(), n), CodecStatus::Ok);
    EXPECT_EQ(n, encodedSizeBinary(spec));

    std::vector<double> scratch(4);
    GridView back;
    ASSERT_EQ(decode(out.data(), n, spec, scratch.data(), scratch.size(), back), CodecStatus::Ok);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_DOUBLE_EQ(in[i], back.cells[i]);
    }
}

TEST(MapletCodec, BinaryPreambleDisagreesWithSpec)
{
    TileSpec encSpec{WireFormat::Binary, CellType::Float64, 2, 2, -9999.0};
    std::vector<double> in = {1, 2, 3, 4};
    GridView src{in.data(), 2, 2, encSpec.noData};
    std::vector<int8_t> out(encodedSizeBinary(encSpec));
    uint64_t n = 0;
    ASSERT_EQ(encode(src, encSpec, out.data(), out.size(), n), CodecStatus::Ok);

    TileSpec wrong = encSpec;
    wrong.width = 3;   // decoder configured differently than the blob
    std::vector<double> scratch(6);
    GridView back;
    EXPECT_EQ(decode(out.data(), n, wrong, scratch.data(), scratch.size(), back),
              CodecStatus::SizeMismatch);
}