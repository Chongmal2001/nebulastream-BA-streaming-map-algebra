
#include <MapletCodec.hpp>
#include <tests/MapletGolden.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace Raster;

// ================================================================ text format

namespace
{
TileSpec textSpec(uint32_t h, uint32_t w, double nd = std::numeric_limits<double>::quiet_NaN(), uint8_t bands = 1)
{
    TileSpec s{WireFormat::Text, CellType::Float64, w, h, nd};
    s.bands = bands;
    return s;
}

CodecStatus decodeTextBlob(const std::string& blob, const TileSpec& spec, std::vector<double>& scratch, GridView& v)
{
    return decode(reinterpret_cast<const int8_t*>(blob.data()), blob.size(), spec, scratch.data(), scratch.size(), v);
}

// decode a text blob, return the cells (fails the test on codec error)
std::vector<double> decodeText(const std::string& blob, const TileSpec& spec)
{
    std::vector<double> scratch(spec.cellCount());
    GridView v;
    EXPECT_EQ(decodeTextBlob(blob, spec, scratch, v), CodecStatus::Ok);
    return {v.cells, v.cells + v.cellCount()};
}
}  // namespace

TEST(MapletCodec, TextDecodeRowMajor)
{
    auto c = decodeText("12.0 13.5 NaN 14.0", textSpec(2, 2));
    EXPECT_DOUBLE_EQ(c[0], 12.0);
    EXPECT_DOUBLE_EQ(c[1], 13.5);
    EXPECT_TRUE(std::isnan(c[2]));   // NaN sentinel
    EXPECT_DOUBLE_EQ(c[3], 14.0);
}

TEST(MapletCodec, TextAcceptsNaNSpellingsAndWhitespace)
{
    auto c = decodeText("  nan\t-NaN\n+NAN\r\n 1e-3  ", textSpec(2, 2));
    EXPECT_TRUE(std::isnan(c[0]));
    EXPECT_TRUE(std::isnan(c[1]));
    EXPECT_TRUE(std::isnan(c[2]));
    EXPECT_DOUBLE_EQ(c[3], 1e-3);
}

TEST(MapletCodec, TextTooFewTokens)
{
    std::vector<double> s(4);
    GridView v;
    EXPECT_EQ(decodeTextBlob("1 2 3", textSpec(2, 2), s, v), CodecStatus::TruncatedInput);
}

TEST(MapletCodec, TextTooManyTokens)
{
    std::vector<double> s(4);
    GridView v;
    EXPECT_EQ(decodeTextBlob("1 2 3 4 5", textSpec(2, 2), s, v), CodecStatus::SizeMismatch);
}

TEST(MapletCodec, ScratchTooSmall)
{
    std::vector<double> s(3);   // one short
    GridView v;
    EXPECT_EQ(decodeTextBlob("1 2 3 4", textSpec(2, 2), s, v), CodecStatus::ScratchTooSmall);
}

TEST(MapletCodec, MalformedNumber)
{
    std::vector<double> s(2);
    GridView v;
    EXPECT_EQ(decodeTextBlob("1 2x", textSpec(1, 2), s, v), CodecStatus::MalformedNumber);
    EXPECT_EQ(decodeTextBlob("1 1e999", textSpec(1, 2), s, v), CodecStatus::MalformedNumber);   // overflow to inf
}

// The one people skip: encode->decode must be bit-exact on awkward doubles.
TEST(MapletCodec, TextRoundTripFidelity)
{
    auto spec = textSpec(1, 4);
    std::vector<double> in = {0.1, 1e308, std::numeric_limits<double>::denorm_min(), -0.0};
    GridView src{in.data(), 1, 4, spec.noData};

    std::vector<int8_t> out(encodedSizeTextUpperBound(spec));
    uint64_t n = 0;
    ASSERT_EQ(encode(src, spec, nullptr, out.data(), out.size(), n), CodecStatus::Ok);   // text: no placement

    std::vector<double> scratch(4);
    GridView back;
    ASSERT_EQ(decode(out.data(), n, spec, scratch.data(), scratch.size(), back), CodecStatus::Ok);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(std::bit_cast<uint64_t>(in[i]), std::bit_cast<uint64_t>(back.cells[i]))
            << "cell " << i;   // bit-exact, so -0.0 and denormals are caught
    }
}

TEST(MapletCodec, TextCarriesAllBandsBandMajor)
{
    const auto spec = textSpec(2, 3, std::numeric_limits<double>::quiet_NaN(), 2);
    const double cells[] = {1, 2, 3, 4, 5, 6, -1, std::numeric_limits<double>::quiet_NaN(), 0, 10, 20, 30};
    const GridView view{cells, 2, 3, spec.noData, 2};
    std::vector<int8_t> out(encodedSizeTextUpperBound(spec));
    uint64_t written = 0;
    ASSERT_EQ(encode(view, spec, nullptr, out.data(), out.size(), written), CodecStatus::Ok);
    std::vector<double> scratch(spec.cellCount());
    GridView back;
    ASSERT_EQ(decode(out.data(), written, spec, scratch.data(), scratch.size(), back), CodecStatus::Ok);
    EXPECT_EQ(back.bands, 2);
    EXPECT_EQ(back.at(1, 1, 2), 30.0);
    EXPECT_TRUE(std::isnan(back.at(1, 0, 1)));
    EXPECT_EQ(back.band(1).at(0, 0), -1.0);
}

TEST(MapletCodec, ZeroBandsIsBadDimensions)
{
    std::vector<double> s(4);
    GridView v;
    EXPECT_EQ(decodeTextBlob("1 2 3 4", textSpec(2, 2, 0.0, 0), s, v), CodecStatus::BadDimensions);
}

// ================================================================ binary format

TEST(MapletCodec, BinaryRoundTripWithCustomNoData)
{
    // noData is not stored in the maplet; it comes from the spec on both sides.
    TileSpec spec{WireFormat::Binary, CellType::Float64, 2, 2, -9999.0};
    spec.rasterWidth = 4;
    spec.rasterHeight = 2;
    std::vector<double> in = {1.0, -9999.0, 3.0, 4.0};
    GridView src{in.data(), 2, 2, spec.noData};
    const MapletPlacement p{5, 1, 0, 0};

    std::vector<int8_t> out(encodedSizeBinary(spec));
    uint64_t n = 0;
    ASSERT_EQ(encode(src, spec, &p, out.data(), out.size(), n), CodecStatus::Ok);
    EXPECT_EQ(n, encodedSizeBinary(spec));
    EXPECT_EQ(n, BINARY_HEADER_SIZE + (4 * sizeof(double)));

    std::vector<double> scratch(4);
    GridView back;
    MapletHeader hdr;
    ASSERT_EQ(decode(out.data(), n, spec, scratch.data(), scratch.size(), back, &hdr), CodecStatus::Ok);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(std::bit_cast<uint64_t>(in[i]), std::bit_cast<uint64_t>(back.cells[i]));
    }
    EXPECT_TRUE(back.isNoData(back.at(0, 1)));
    EXPECT_EQ(hdr.frameId, 5U);
    EXPECT_EQ(hdr.tileX, 1);
    EXPECT_EQ(hdr.tileY, 0);
}

TEST(MapletCodec, BinaryPreambleDisagreesWithSpec)
{
    TileSpec encSpec{WireFormat::Binary, CellType::Float64, 2, 2, -9999.0};
    encSpec.rasterWidth = 2;
    encSpec.rasterHeight = 2;
    std::vector<double> in = {1, 2, 3, 4};
    GridView src{in.data(), 2, 2, encSpec.noData};
    const MapletPlacement p{1, 0, 0, 0};
    std::vector<int8_t> out(encodedSizeBinary(encSpec));
    uint64_t n = 0;
    ASSERT_EQ(encode(src, encSpec, &p, out.data(), out.size(), n), CodecStatus::Ok);

    TileSpec wrong = encSpec;
    wrong.width = 3;   // decoder configured differently than the blob
    std::vector<double> scratch(6);
    GridView back;
    EXPECT_EQ(decode(out.data(), n, wrong, scratch.data(), scratch.size(), back), CodecStatus::SizeMismatch);
}

// ---------------------------------------------------------------- binary format against mabench's golden bytes

namespace
{
using namespace RasterGolden;
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

Raster::TileSpec binSpec(uint8_t bands, uint32_t rasterWidth, uint32_t rasterHeight)
{
    Raster::TileSpec s;
    s.format = Raster::WireFormat::Binary;
    s.cellType = Raster::CellType::Float64;
    s.width = 3;
    s.height = 2;
    s.noData = NaN;  // benchmark stores: NaN is the only NoData
    s.bands = bands;
    s.rasterWidth = rasterWidth;
    s.rasterHeight = rasterHeight;
    return s;
}

template <std::size_t N>
const int8_t* bytes(const std::array<uint8_t, N>& a)
{
    return reinterpret_cast<const int8_t*>(a.data());
}

template <std::size_t N>
std::vector<int8_t> copyOf(const std::array<uint8_t, N>& a)
{
    return std::vector<int8_t>(bytes(a), bytes(a) + N);
}

template <std::size_t N>
Raster::CodecStatus decodeGolden(const std::array<uint8_t, N>& blob, const Raster::TileSpec& s, std::vector<double>& scratch,
                                 Raster::GridView& view, Raster::MapletHeader* hdr = nullptr)
{
    scratch.assign(s.cellCount(), 0.0);
    return Raster::decode(bytes(blob), blob.size(), s, scratch.data(), scratch.size(), view, hdr);
}

template <std::size_t N>
void expectEncodesTo(const Raster::GridView& view, const Raster::TileSpec& s, const Raster::MapletPlacement& p,
                     const std::array<uint8_t, N>& golden)
{
    std::vector<int8_t> out(Raster::encodedSizeBinary(s));
    uint64_t written = 0;
    ASSERT_EQ(Raster::encode(view, s, &p, out.data(), out.size(), written), Raster::CodecStatus::Ok);
    ASSERT_EQ(written, golden.size());
    EXPECT_EQ(std::memcmp(out.data(), golden.data(), golden.size()), 0);
}
}

TEST(MapletCodecGolden, DecodesSingleBandMaplet)
{
    std::vector<double> scratch;
    Raster::GridView view;
    Raster::MapletHeader hdr;
    ASSERT_EQ(decodeGolden(GOLDEN_1BAND, binSpec(1, 3, 2), scratch, view, &hdr), Raster::CodecStatus::Ok);
    EXPECT_EQ(view.bands, 1);
    EXPECT_EQ(view.at(0, 0), 1.5);
    EXPECT_TRUE(view.isNoData(view.at(0, 1)));
    EXPECT_EQ(view.at(0, 2), 0.0);
    EXPECT_FALSE(view.isNoData(view.at(0, 2)));  // zero is data, not NoData
    EXPECT_TRUE(std::signbit(view.at(1, 0)));    // -0.0 survives
    EXPECT_EQ(view.at(1, 1), 5e-324);
    EXPECT_EQ(view.at(1, 2), 1e308);
    EXPECT_EQ(hdr.frameId, 0U);                  // as written by the tiler: unassigned
    EXPECT_EQ(hdr.tileX, 0);
    EXPECT_EQ(hdr.tileY, 0);
    EXPECT_EQ(hdr.totalSize(), GOLDEN_1BAND.size());
}

TEST(MapletCodecGolden, DecodesMultiBandMapletBandMajorWithPlacement)
{
    std::vector<double> scratch;
    Raster::GridView view;
    Raster::MapletHeader hdr;
    ASSERT_EQ(decodeGolden(GOLDEN_2BAND, binSpec(2, 6, 2), scratch, view, &hdr), Raster::CodecStatus::Ok);
    ASSERT_EQ(view.bands, 2);
    EXPECT_EQ(view.cellCount(), 12U);
    EXPECT_EQ(view.planeCells(), 6U);
    EXPECT_EQ(view.at(0, 1, 2), 6.0);
    EXPECT_EQ(view.at(1, 0, 0), -1.0);
    EXPECT_TRUE(view.isNoData(view.at(1, 0, 1)));
    const auto band1 = view.band(1);
    EXPECT_EQ(band1.bands, 1);
    EXPECT_EQ(band1.at(1, 2), 30.0);
    EXPECT_EQ(hdr.frameId, 7U);
    EXPECT_EQ(hdr.tileX, 1);
    EXPECT_EQ(hdr.tileY, 0);
    EXPECT_EQ(hdr.rasterWidth, 6U);
    EXPECT_EQ(hdr.rasterHeight, 2U);
    EXPECT_EQ(hdr.flags, 0);
}

TEST(MapletCodecGolden, EncodesByteIdenticalMaplets)
{
    {
        const double cells[] = {1.5, NaN, 0.0, -0.0, 5e-324, 1e308};
        expectEncodesTo(Raster::GridView{cells, 2, 3, NaN, 1}, binSpec(1, 3, 2), Raster::MapletPlacement{0, 0, 0, 0}, GOLDEN_1BAND);
    }
    {
        const double cells[] = {1, 2, 3, 4, 5, 6, -1, NaN, 0, 10, 20, 30};
        expectEncodesTo(Raster::GridView{cells, 2, 3, NaN, 2}, binSpec(2, 6, 2), Raster::MapletPlacement{7, 1, 0, 0}, GOLDEN_2BAND);
    }
    {
        const double cells[] = {9, NaN, NaN, NaN, NaN, NaN};
        expectEncodesTo(Raster::GridView{cells, 2, 3, NaN, 1}, binSpec(1, 4, 3), Raster::MapletPlacement{0, 1, 1, 0}, GOLDEN_EDGE);
    }
    {
        const double cells[] = {NaN, NaN, NaN, NaN, NaN, NaN};
        expectEncodesTo(Raster::GridView{cells, 2, 3, NaN, 1}, binSpec(1, 4, 3),
                        Raster::MapletPlacement{3, 0, 1, Raster::FLAG_FILLED}, GOLDEN_FILLED);
    }
}

TEST(MapletCodecGolden, PeekHeaderReadsPreambleOnly)
{
    Raster::MapletHeader edge;
    ASSERT_EQ(Raster::peekHeader(bytes(GOLDEN_EDGE), GOLDEN_EDGE.size(), edge), Raster::CodecStatus::Ok);
    EXPECT_EQ(edge.validRows(), 1U);
    EXPECT_EQ(edge.validCols(), 1U);
    Raster::MapletHeader filled;
    ASSERT_EQ(Raster::peekHeader(bytes(GOLDEN_FILLED), GOLDEN_FILLED.size(), filled), Raster::CodecStatus::Ok);
    EXPECT_EQ(filled.flags, Raster::FLAG_FILLED);
    EXPECT_EQ(filled.frameId, 3U);
}

TEST(MapletCodecGolden, PeekHeaderRejectsMalformedInput)
{
    Raster::MapletHeader h;
    EXPECT_EQ(Raster::peekHeader(bytes(GOLDEN_1BAND), 31, h), Raster::CodecStatus::TruncatedInput);
    EXPECT_EQ(Raster::peekHeader(bytes(GOLDEN_1BAND), GOLDEN_1BAND.size() - 8, h), Raster::CodecStatus::TruncatedInput);
    auto longer = copyOf(GOLDEN_1BAND);
    longer.push_back(0);
    EXPECT_EQ(Raster::peekHeader(longer.data(), longer.size(), h), Raster::CodecStatus::SizeMismatch);
    auto oldFormat = copyOf(GOLDEN_1BAND);
    oldFormat[0] = 2;  // the retired 24-byte layout
    EXPECT_EQ(Raster::peekHeader(oldFormat.data(), oldFormat.size(), h), Raster::CodecStatus::UnsupportedVersion);
    auto cellType = copyOf(GOLDEN_1BAND);
    cellType[1] = 2;
    EXPECT_EQ(Raster::peekHeader(cellType.data(), cellType.size(), h), Raster::CodecStatus::UnsupportedCellType);
    auto outside = copyOf(GOLDEN_2BAND);
    outside[8] = 2;  // tileX = 2 in a 6-wide raster of 3-wide tiles (grid is 2x1)
    EXPECT_EQ(Raster::peekHeader(outside.data(), outside.size(), h), Raster::CodecStatus::PositionOutOfRange);
    auto huge = copyOf(GOLDEN_1BAND);
    const uint32_t big = 0xFFFFFFFFU;
    std::memcpy(huge.data() + 16, &big, 4);  // height * width would overflow the size computation
    std::memcpy(huge.data() + 20, &big, 4);
    EXPECT_EQ(Raster::peekHeader(huge.data(), huge.size(), h), Raster::CodecStatus::TruncatedInput);
}

TEST(MapletCodecGolden, DecodeRejectsSpecMismatches)
{
    std::vector<double> scratch;
    Raster::GridView view;
    EXPECT_EQ(decodeGolden(GOLDEN_2BAND, binSpec(1, 6, 2), scratch, view), Raster::CodecStatus::SizeMismatch);   // bands
    EXPECT_EQ(decodeGolden(GOLDEN_1BAND, binSpec(2, 3, 2), scratch, view), Raster::CodecStatus::SizeMismatch);   // bands
    EXPECT_EQ(decodeGolden(GOLDEN_1BAND, binSpec(1, 4, 2), scratch, view), Raster::CodecStatus::SizeMismatch);   // raster width
    EXPECT_EQ(decodeGolden(GOLDEN_1BAND, binSpec(1, 0, 0), scratch, view), Raster::CodecStatus::Ok);             // raster size unchecked
    EXPECT_EQ(decodeGolden(GOLDEN_1BAND, binSpec(0, 3, 2), scratch, view), Raster::CodecStatus::BadDimensions);
    auto s = binSpec(1, 3, 2);
    s.width = 2;
    EXPECT_EQ(decodeGolden(GOLDEN_1BAND, s, scratch, view), Raster::CodecStatus::SizeMismatch);
    std::vector<double> small(5);
    EXPECT_EQ(Raster::decode(bytes(GOLDEN_1BAND), GOLDEN_1BAND.size(), binSpec(1, 3, 2), small.data(), small.size(), view),
              Raster::CodecStatus::ScratchTooSmall);
}

TEST(MapletCodecGolden, EncodeNeedsPlacementAndRasterSize)
{
    const double cells[] = {1, 2, 3, 4, 5, 6};
    const Raster::GridView view{cells, 2, 3, NaN, 1};
    std::vector<int8_t> out(Raster::encodedSizeBinary(binSpec(1, 3, 2)));
    uint64_t written = 0;
    EXPECT_EQ(Raster::encode(view, binSpec(1, 3, 2), nullptr, out.data(), out.size(), written), Raster::CodecStatus::MissingPlacement);
    const Raster::MapletPlacement p{1, 0, 0, 0};
    EXPECT_EQ(Raster::encode(view, binSpec(1, 0, 0), &p, out.data(), out.size(), written), Raster::CodecStatus::BadDimensions);
    const Raster::MapletPlacement outside{1, 1, 0, 0};
    EXPECT_EQ(Raster::encode(view, binSpec(1, 3, 2), &outside, out.data(), out.size(), written), Raster::CodecStatus::PositionOutOfRange);
    EXPECT_EQ(Raster::encode(view, binSpec(1, 3, 2), &p, out.data(), out.size() - 1, written), Raster::CodecStatus::OutputTooSmall);
    EXPECT_EQ(Raster::encode(view, binSpec(2, 3, 2), &p, out.data(), out.size(), written), Raster::CodecStatus::SizeMismatch);
}

TEST(MapletCodecGolden, SetFrameIdPatchesOnlyThoseFourBytes)
{
    auto m = copyOf(GOLDEN_1BAND);
    ASSERT_EQ(Raster::setFrameId(m.data(), m.size(), 0x01020304U), Raster::CodecStatus::Ok);
    for (std::size_t i = 0; i < m.size(); ++i)
    {
        if (i < Raster::OFFSET_FRAME_ID || i >= Raster::OFFSET_FRAME_ID + 4)
        {
            EXPECT_EQ(static_cast<uint8_t>(m[i]), GOLDEN_1BAND[i]) << "byte " << i;
        }
    }
    Raster::MapletHeader h;
    ASSERT_EQ(Raster::peekHeader(m.data(), m.size(), h), Raster::CodecStatus::Ok);
    EXPECT_EQ(h.frameId, 0x01020304U);
    ASSERT_EQ(Raster::setFlags(m.data(), m.size(), Raster::FLAG_FILLED), Raster::CodecStatus::Ok);
    ASSERT_EQ(Raster::peekHeader(m.data(), m.size(), h), Raster::CodecStatus::Ok);
    EXPECT_EQ(h.flags, Raster::FLAG_FILLED);
    auto old = copyOf(GOLDEN_1BAND);
    old[0] = 2;
    EXPECT_EQ(Raster::setFrameId(old.data(), old.size(), 1), Raster::CodecStatus::UnsupportedVersion);
}

TEST(MapletCodecGolden, SelectBandsReordersAndKeepsPlacement)
{
    Raster::MapletHeader in;
    ASSERT_EQ(Raster::peekHeader(bytes(GOLDEN_2BAND), GOLDEN_2BAND.size(), in), Raster::CodecStatus::Ok);
    const uint8_t idx[] = {1, 0, 1};
    std::vector<int8_t> out(Raster::selectedSize(in, 3));
    uint64_t written = 0;
    ASSERT_EQ(Raster::selectBands(bytes(GOLDEN_2BAND), GOLDEN_2BAND.size(), idx, 3, out.data(), out.size(), written),
              Raster::CodecStatus::Ok);
    ASSERT_EQ(written, Raster::BINARY_HEADER_SIZE + (3 * 6 * 8));
    Raster::MapletHeader h;
    ASSERT_EQ(Raster::peekHeader(out.data(), written, h), Raster::CodecStatus::Ok);
    EXPECT_EQ(h.bands, 3);
    EXPECT_EQ(h.frameId, 7U);  // placement travels unchanged
    EXPECT_EQ(h.tileX, 1);
    std::vector<double> scratch(18);
    Raster::GridView v;
    auto s = binSpec(3, 6, 2);
    ASSERT_EQ(Raster::decode(out.data(), written, s, scratch.data(), scratch.size(), v), Raster::CodecStatus::Ok);
    EXPECT_EQ(v.at(0, 0, 0), -1.0);  // band 1 first
    EXPECT_EQ(v.at(1, 1, 2), 6.0);   // then band 0
    EXPECT_EQ(v.at(2, 1, 2), 30.0);  // band 1 again
}

TEST(MapletCodecGolden, SelectBandsRejectsBadIndices)
{
    std::vector<int8_t> out(GOLDEN_2BAND.size());
    uint64_t written = 0;
    const uint8_t bad[] = {2};
    EXPECT_EQ(Raster::selectBands(bytes(GOLDEN_2BAND), GOLDEN_2BAND.size(), bad, 1, out.data(), out.size(), written),
              Raster::CodecStatus::BandOutOfRange);
    const uint8_t ok[] = {0};
    EXPECT_EQ(Raster::selectBands(bytes(GOLDEN_2BAND), GOLDEN_2BAND.size(), ok, 0, out.data(), out.size(), written),
              Raster::CodecStatus::BandOutOfRange);
    EXPECT_EQ(Raster::selectBands(bytes(GOLDEN_2BAND), GOLDEN_2BAND.size(), ok, 1, out.data(), 10, written),
              Raster::CodecStatus::OutputTooSmall);
}

// ================================================================ frame header

namespace
{
const int8_t* goldenFrameHeader()
{
    return reinterpret_cast<const int8_t*>(RasterGolden::GOLDEN_FRAME_HEADER.data());
}
}

TEST(MapletCodecFrameHeader, GoldenFrameHeaderFields)
{
    FrameHeader h;
    ASSERT_EQ(peekFrameHeader(goldenFrameHeader(), FRAME_HEADER_SIZE, h), CodecStatus::Ok);
    EXPECT_EQ(h.frameId, 5U);
    EXPECT_EQ(h.nTiles, 2U);
    EXPECT_EQ(h.eventTimeMs, 1700000000000ULL);
    EXPECT_EQ(h.rasterWidth, 6U);
    EXPECT_EQ(h.rasterHeight, 2U);
    EXPECT_EQ(h.tileWidth, 3U);
    EXPECT_EQ(h.tileHeight, 2U);
    EXPECT_EQ(h.bands, 2);
    EXPECT_EQ(h.flags, 0);
    EXPECT_TRUE(std::isnan(h.noData));
    EXPECT_EQ(h.crsEpsg, 32633U);
    EXPECT_EQ(h.transform, (std::array<double, 6>{10.0, 0.0, 370670.0, 0.0, -10.0, 5835040.0}));
    EXPECT_EQ(h.tilesX(), 2U);
    EXPECT_EQ(h.tilesY(), 1U);
    EXPECT_EQ(h.mapletSize(), 32U + (8U * 2 * 2 * 3));
    EXPECT_EQ(h.fileSize(), 112U + (2 * h.mapletSize()));
}

TEST(MapletCodecFrameHeader, TruncatedHeader)
{
    FrameHeader h;
    EXPECT_EQ(peekFrameHeader(goldenFrameHeader(), FRAME_HEADER_SIZE - 1, h), CodecStatus::TruncatedInput);
    EXPECT_EQ(peekFrameHeader(nullptr, FRAME_HEADER_SIZE, h), CodecStatus::TruncatedInput);
}

TEST(MapletCodecFrameHeader, OtherRecordVersionRejected)
{
    auto bytes = RasterGolden::GOLDEN_FRAME_HEADER;
    bytes[7] = '3'; // "MPLFRM03"
    FrameHeader h;
    EXPECT_EQ(peekFrameHeader(reinterpret_cast<const int8_t*>(bytes.data()), bytes.size(), h), CodecStatus::UnsupportedVersion);
}

TEST(MapletCodecFrameHeader, OtherCellTypeRejected)
{
    auto bytes = RasterGolden::GOLDEN_FRAME_HEADER;
    bytes[41] = 2;
    FrameHeader h;
    EXPECT_EQ(peekFrameHeader(reinterpret_cast<const int8_t*>(bytes.data()), bytes.size(), h), CodecStatus::UnsupportedCellType);
}

TEST(MapletCodecFrameHeader, ZeroBandsRejected)
{
    auto bytes = RasterGolden::GOLDEN_FRAME_HEADER;
    bytes[40] = 0;
    FrameHeader h;
    EXPECT_EQ(peekFrameHeader(reinterpret_cast<const int8_t*>(bytes.data()), bytes.size(), h), CodecStatus::BadDimensions);
}
