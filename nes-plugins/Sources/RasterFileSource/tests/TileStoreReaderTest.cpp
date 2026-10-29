#include <TileStoreReader.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <MapletCodec.hpp>

namespace NES
{
namespace
{
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/// One frame file to write: header fields and the tiles (positions in file order)
struct FrameSpec
{
    std::string fileName;
    uint32_t frameId = 0;
    uint64_t eventTimeMs = 0;
    uint32_t rasterWidth = 4;
    uint32_t rasterHeight = 4;
    uint32_t tileSize = 2;
    uint8_t bands = 1;
    std::vector<std::pair<int32_t, int32_t>> tiles{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
};

/// Frame header bytes as the tiler writes them
std::array<int8_t, Raster::FRAME_HEADER_SIZE> frameHeaderBytes(const FrameSpec& frame)
{
    std::array<int8_t, Raster::FRAME_HEADER_SIZE> bytes{};
    const auto put = [&](size_t offset, const auto& value) { std::memcpy(bytes.data() + offset, &value, sizeof value); };
    std::memcpy(bytes.data(), Raster::FRAME_MAGIC.data(), Raster::FRAME_MAGIC.size());
    put(8, frame.frameId);
    put(12, static_cast<uint32_t>(frame.tiles.size()));
    put(16, frame.eventTimeMs);
    put(24, frame.rasterWidth);
    put(28, frame.rasterHeight);
    put(32, frame.tileSize);
    put(36, frame.tileSize);
    put(40, frame.bands);
    put(41, static_cast<uint8_t>(Raster::CellType::Float64));
    put(48, NaN);
    put(56, uint32_t{32633});
    put(64, std::array<double, 6>{10.0, 0.0, 0.0, 0.0, -10.0, 0.0});
    return bytes;
}

/// Binary maplet at (x, y), frame id 0, cells = 10 * y + x
std::vector<int8_t> mapletBytes(const FrameSpec& frame, int32_t x, int32_t y)
{
    const std::vector<double> cells(static_cast<size_t>(frame.bands) * frame.tileSize * frame.tileSize, (10.0 * y) + x);
    Raster::TileSpec spec{Raster::WireFormat::Binary, Raster::CellType::Float64, frame.tileSize, frame.tileSize, NaN, frame.bands};
    spec.rasterWidth = frame.rasterWidth;
    spec.rasterHeight = frame.rasterHeight;
    std::vector<int8_t> out(Raster::encodedSizeBinary(spec));
    uint64_t written = 0;
    const Raster::MapletPlacement placement{0, x, y, 0};
    const auto status = Raster::encode(
        Raster::GridView{cells.data(), frame.tileSize, frame.tileSize, NaN, frame.bands}, spec, &placement, out.data(), out.size(), written);
    EXPECT_EQ(status, Raster::CodecStatus::Ok);
    return out;
}

class TileStoreReaderTest : public ::testing::Test
{
protected:
    std::filesystem::path store;

    void SetUp() override
    {
        store = std::filesystem::temp_directory_path()
            / ("tile-store-reader-test-" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()});
        std::filesystem::remove_all(store);
        std::filesystem::create_directories(store / "frames");
    }

    void TearDown() override { std::filesystem::remove_all(store); }

    void writeFrame(const FrameSpec& frame) const
    {
        std::ofstream file(store / "frames" / frame.fileName, std::ios::binary);
        const auto header = frameHeaderBytes(frame);
        file.write(reinterpret_cast<const char*>(header.data()), header.size());
        for (const auto& [x, y] : frame.tiles)
        {
            const auto maplet = mapletBytes(frame, x, y);
            file.write(reinterpret_cast<const char*>(maplet.data()), static_cast<std::streamsize>(maplet.size()));
        }
    }

    struct Read
    {
        uint64_t eventTimeMs;
        uint32_t srcFrameId;
        Raster::MapletHeader maplet;
    };

    /// All maplets of the store, in emission order
    std::vector<Read> readAll() const
    {
        TileStoreReader reader(store);
        std::vector<Read> result;
        std::vector<int8_t> buffer(reader.mapletSize());
        while (not reader.atEnd())
        {
            const auto info = reader.next(buffer);
            Read read{info.eventTimeMs, info.srcFrameId, {}};
            EXPECT_EQ(Raster::peekHeader(buffer.data(), buffer.size(), read.maplet), Raster::CodecStatus::Ok);
            result.push_back(read);
        }
        return result;
    }
};

/// File order, engine frame ids 1, 2, lineage = header frame_id
TEST_F(TileStoreReaderTest, ReadsFramesInOrderAndAssignsFrameIds)
{
    writeFrame({.fileName = "f000001.mpl", .frameId = 1, .eventTimeMs = 50});
    writeFrame({.fileName = "f000000.mpl", .frameId = 0, .eventTimeMs = 0});

    const auto maplets = readAll();
    ASSERT_EQ(maplets.size(), 8U);
    for (size_t i = 0; i < maplets.size(); ++i)
    {
        const bool second = i >= 4;
        EXPECT_EQ(maplets[i].eventTimeMs, second ? 50U : 0U);
        EXPECT_EQ(maplets[i].srcFrameId, second ? 1U : 0U);
        EXPECT_EQ(maplets[i].maplet.frameId, second ? 2U : 1U);
    }
    EXPECT_EQ(maplets[1].maplet.tileX, 1);
    EXPECT_EQ(maplets[2].maplet.tileY, 1);
}

TEST_F(TileStoreReaderTest, AnchorIsTheFirstFrameHeader)
{
    writeFrame({.fileName = "f000000.mpl", .bands = 2});
    const TileStoreReader reader(store);
    EXPECT_EQ(reader.anchor().bands, 2);
    EXPECT_EQ(reader.anchor().tilesX(), 2U);
    EXPECT_EQ(reader.mapletSize(), Raster::BINARY_HEADER_SIZE + (2 * 2 * 2 * sizeof(double)));
}

/// A frame may lack tiles (focal reads them as NoData at the deadline)
TEST_F(TileStoreReaderTest, FrameWithMissingTile)
{
    writeFrame({.fileName = "f000000.mpl", .tiles = {{0, 0}, {1, 0}, {0, 1}}});
    EXPECT_EQ(readAll().size(), 3U);
}

TEST_F(TileStoreReaderTest, NoFramesThrows)
{
    EXPECT_ANY_THROW(TileStoreReader{store});
}

TEST_F(TileStoreReaderTest, NoStoreThrows)
{
    EXPECT_ANY_THROW(TileStoreReader{store / "missing"});
}

TEST_F(TileStoreReaderTest, OtherRecordVersionThrows)
{
    writeFrame({.fileName = "f000000.mpl"});
    std::fstream file(store / "frames" / "f000000.mpl", std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(7);
    file.put('3');
    file.close();
    EXPECT_ANY_THROW(TileStoreReader{store});
}

/// Frames are checked when reached: the first frame is read before the second one fails
TEST_F(TileStoreReaderTest, EventTimeNotIncreasingThrowsWhenReached)
{
    writeFrame({.fileName = "f000000.mpl", .eventTimeMs = 50});
    writeFrame({.fileName = "f000001.mpl", .eventTimeMs = 50});
    TileStoreReader reader(store);
    std::vector<int8_t> buffer(reader.mapletSize());
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_FALSE(reader.atEnd());
        reader.next(buffer);
    }
    EXPECT_ANY_THROW(static_cast<void>(reader.atEnd()));
}

TEST_F(TileStoreReaderTest, DifferentRasterThrows)
{
    writeFrame({.fileName = "f000000.mpl", .eventTimeMs = 0});
    writeFrame({.fileName = "f000001.mpl", .eventTimeMs = 50, .bands = 2});
    EXPECT_ANY_THROW(readAll());
}

TEST_F(TileStoreReaderTest, TruncatedFrameThrows)
{
    writeFrame({.fileName = "f000000.mpl"});
    const auto path = store / "frames" / "f000000.mpl";
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 8);
    EXPECT_ANY_THROW(readAll());
}

TEST_F(TileStoreReaderTest, DuplicatePositionThrows)
{
    writeFrame({.fileName = "f000000.mpl", .tiles = {{0, 0}, {0, 0}}});
    EXPECT_ANY_THROW(readAll());
}

/// A maplet whose raster size disagrees with its frame header
TEST_F(TileStoreReaderTest, MapletOfAnotherRasterThrows)
{
    FrameSpec frame{.fileName = "f000000.mpl", .tiles = {{0, 0}}};
    writeFrame(frame);
    FrameSpec other = frame;
    other.rasterWidth = 6;
    const auto maplet = mapletBytes(other, 0, 0);
    std::fstream file(store / "frames" / "f000000.mpl", std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(Raster::FRAME_HEADER_SIZE);
    file.write(reinterpret_cast<const char*>(maplet.data()), static_cast<std::streamsize>(maplet.size()));
    file.close();
    EXPECT_ANY_THROW(readAll());
}

}
}
