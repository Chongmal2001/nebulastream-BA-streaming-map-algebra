#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <FocalSpec.hpp>
#include <MapletCodec.hpp>
#include <Raster/DecodedTile.hpp>
#include <Raster/Focal/FocalSlice.hpp>
#include <Raster/FrameTableBuffers.hpp>
#include <Runtime/BufferManager.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Time/Timestamp.hpp>

namespace NES::Focal
{
namespace
{

class RasterSliceTest : public ::testing::Test
{
protected:
    /// 3 x 2 tiles of 4 x 4 cells, reach 1
    Raster::RasterGeometry geo{12, 8, 4, 4};
    Raster::TileReach reach{1, 1};

    /// Declared first, destroyed last: every buffer must be back before the manager goes.
    std::shared_ptr<BufferManager> bufferManager = BufferManager::create();

    [[nodiscard]] size_t unpooled() const { return bufferManager->getNumOfUnpooledBuffers(); }

    std::unique_ptr<FocalSlice> makeSlice() { return std::make_unique<FocalSlice>(Timestamp(0), Timestamp(1000), bufferManager); }

    std::pair<FocalFrameStateT*, bool> getOrCreate(FocalSlice& slice, FrameKey key, uint32_t outFrame = 1)
    {
        return slice.frameTable().getOrCreate(
            key,
            FocalFrameStateT::bytesFor(geo),
            [&](std::span<std::byte> memory) { return FocalFrameStateT::create(memory, key, 100, outFrame, geo, reach); });
    }

    /// A decoded tile whose cells live in their own unpooled buffer, as the operator stores it.
    DecodedTile makeTile(double value)
    {
        const size_t cells = static_cast<size_t>(geo.tileW) * static_cast<size_t>(geo.tileH);
        auto buffer = bufferManager->getUnpooledBuffer(cells * sizeof(double));
        EXPECT_TRUE(buffer.has_value());
        auto area = buffer->getAvailableMemoryArea<double>();
        for (size_t i = 0; i < cells; ++i)
        {
            area[i] = value;
        }
        DecodedTile tile;
        tile.view.cells = area.data();
        tile.view.w = static_cast<uint32_t>(geo.tileW);
        tile.view.h = static_cast<uint32_t>(geo.tileH);
        tile.view.bands = 1;
        tile.cells = std::move(*buffer);
        return tile;
    }
};

TEST_F(RasterSliceTest, FrameTableBuffersAllocatesAlignedUnpooledBuffers)
{
    {
        FrameTableBuffers buffers(bufferManager);
        EXPECT_EQ(unpooled(), 1U);   // the root
        for (const size_t bytes : {1UL, 17UL, 64UL, 1000UL, 100000UL})
        {
            auto pending = buffers.allocate(bytes);
            ASSERT_GE(pending.bytes().size(), bytes);
            EXPECT_EQ(reinterpret_cast<uintptr_t>(pending.bytes().data()) % alignof(std::max_align_t), 0U);
            buffers.adopt(std::move(pending));
        }
        EXPECT_EQ(unpooled(), 6U);

        /// a block that is never adopted (lost race) is released when it goes out of scope
        {
            auto dropped = buffers.allocate(128);
            EXPECT_EQ(unpooled(), 7U);
        }
        EXPECT_EQ(unpooled(), 6U);
    }
    EXPECT_EQ(unpooled(), 0U);
}

TEST_F(RasterSliceTest, SliceKeepsWindowBounds)
{
    auto slice = makeSlice();
    EXPECT_EQ(slice->getSliceStart(), Timestamp(0));
    EXPECT_EQ(slice->getSliceEnd(), Timestamp(1000));
    EXPECT_EQ(slice->frameTable().size(), 0U);
}

/// 20 frames of one origin: root + 3 table pages (S = 8) + 20 frame states = 24 NES buffers, all released with the slice.
TEST_F(RasterSliceTest, FramesAreNesBuffersReleasedWithTheSlice)
{
    auto slice = makeSlice();
    for (uint32_t frame = 1; frame <= 20; ++frame)
    {
        auto [state, created] = getOrCreate(*slice, FrameKey{0, frame}, frame);
        ASSERT_TRUE(created);
        EXPECT_EQ(state->outFrame(), frame);
    }
    EXPECT_EQ(slice->frameTable().size(), 20U);
    EXPECT_EQ(slice->frameTable().pageCount(), 3U);
    EXPECT_EQ(unpooled(), 24U);

    auto [again, created] = getOrCreate(*slice, FrameKey{0, 7});
    EXPECT_FALSE(created);
    EXPECT_EQ(again->key(), (FrameKey{0, 7}));
    EXPECT_EQ(unpooled(), 24U);   // find hit: nothing allocated

    slice.reset();
    EXPECT_EQ(unpooled(), 0U);
}

/// releaseTile frees a tile buffer at once; the rest go with the slice
TEST_F(RasterSliceTest, StoredTilesAreReleasedEagerlyOrWithTheSlice)
{
    auto slice = makeSlice();
    auto [fs, created] = getOrCreate(*slice, FrameKey{0, 1});
    ASSERT_TRUE(created);
    const size_t base = unpooled();   // root + page + frame state

    for (const Raster::TilePosition p : {Raster::TilePosition{0, 0}, Raster::TilePosition{1, 0}})
    {
        ASSERT_EQ(fs->admit(p), Admission::New);
        fs->storeTile(p, makeTile(static_cast<double>(p.x)));
    }
    EXPECT_EQ(unpooled(), base + 2);
    EXPECT_FALSE(fs->tile({1, 0}).empty());
    EXPECT_EQ(fs->tile({1, 0}).view.cells[5], 1.0);

    fs->releaseTile({0, 0});
    EXPECT_TRUE(fs->tile({0, 0}).empty());
    EXPECT_EQ(unpooled(), base + 1);

    slice.reset();   // tile (1, 0) is still stored
    EXPECT_EQ(unpooled(), 0U);
}

/// Threads create the same frames concurrently; lost races free their buffers at once
TEST_F(RasterSliceTest, ConcurrentFrameCreation)
{
    constexpr size_t Threads = 8;
    constexpr uint32_t Frames = 50;
    auto slice = makeSlice();

    std::atomic<size_t> creations{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < Threads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                while (!go.load(std::memory_order_acquire))
                {
                }
                for (uint32_t i = 0; i < Frames; ++i)
                {
                    const uint32_t frame = 1 + ((i + static_cast<uint32_t>(t) * 7) % Frames);   // different order per thread
                    auto [state, created] = getOrCreate(*slice, FrameKey{static_cast<uint32_t>(t % 2), frame});
                    ASSERT_NE(state, nullptr);
                    if (created)
                    {
                        creations.fetch_add(1);
                    }
                }
            });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads)
    {
        thread.join();
    }

    const size_t keys = 2 * Frames;   // origins 0 and 1
    EXPECT_EQ(creations.load(), keys);
    EXPECT_EQ(slice->frameTable().size(), keys);
    const size_t pages = slice->frameTable().pageCount();
    EXPECT_EQ(pages, (keys + 7) / 8);
    EXPECT_EQ(unpooled(), 1 + pages + keys);

    slice.reset();
    EXPECT_EQ(unpooled(), 0U);
}

}
}
