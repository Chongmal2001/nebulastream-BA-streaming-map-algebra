#include <bit>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Identifiers/Identifiers.hpp>
#include <Runtime/BufferManager.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Sequencing/SequenceData.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>
#include <Time/Timestamp.hpp>
#include <Util/Logger/LogLevel.hpp>
#include <Util/Logger/Logger.hpp>
#include <folly/Synchronized.h>
#include <gtest/gtest.h>
#include <BaseUnitTest.hpp>
#include <ErrorHandling.hpp>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>
#include <PipelineExecutionContext.hpp>
#include <SliceCacheConfiguration.hpp>
#include <WindowBasedOperatorHandler.hpp>
#include <Raster/Focal/FocalFrameOperatorHandler.hpp>

namespace NES
{
namespace
{

struct MockedPipelineContext final : PipelineExecutionContext
{
    explicit MockedPipelineContext(std::shared_ptr<BufferManager> bufferManager) : bufferManager(std::move(bufferManager)) { }

    bool emitBuffer(const TupleBuffer& buffer, ContinuationPolicy) override
    {
        buffers.wlock()->emplace_back(buffer);
        return true;
    }
    TupleBuffer allocateTupleBuffer() override { return bufferManager->getBufferBlocking(); }
    TupleBuffer& pinBuffer(TupleBuffer&& tupleBuffer) override
    {
        pinned.emplace_back(std::make_unique<TupleBuffer>(tupleBuffer));
        return *pinned.back();
    }
    [[nodiscard]] WorkerThreadId getWorkerThreadId() const override { return INITIAL<WorkerThreadId>; }
    [[nodiscard]] uint64_t getNumberOfWorkerThreads() const override { return 1; }
    [[nodiscard]] std::shared_ptr<AbstractBufferProvider> getBufferManager() const override { return bufferManager; }
    [[nodiscard]] PipelineId getPipelineId() const override { return PipelineId(1); }
    std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>>& getOperatorHandlers() override { return handlers; }
    void setOperatorHandlers(std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>>&) override { }
    void repeatTask(const TupleBuffer&, std::chrono::milliseconds) override { INVARIANT(false, "not used"); }

    std::shared_ptr<BufferManager> bufferManager;
    folly::Synchronized<std::vector<TupleBuffer>> buffers;
    std::vector<std::unique_ptr<TupleBuffer>> pinned;
    std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>> handlers;
};

/// One emitted descriptor
struct Output
{
    int32_t x;
    int32_t y;
    uint32_t children;
    uint32_t outFrame;
    uint8_t flags;
    bool complete;
    uint64_t windowStart;
    FrameLineage lineage;
    uint8_t bands;
    Raster::WireFormat format;
};

class FocalFrameOperatorHandlerTest : public Testing::BaseUnitTest
{
public:
    static void SetUpTestSuite() { Logger::setupLogging("FocalFrameOperatorHandlerTest.log", LogLevel::LOG_DEBUG); }

protected:
    static constexpr uint64_t WindowSize = 1000;
    const OriginId originA{1};
    const OriginId originB{2};

    /// 3 x 2 tiles of 4 x 4 cells; radius 1 -> tile reach 1, so every output reads both rows
    Raster::RasterGeometry geometry{12, 8, 4, 4};
    Raster::FocalSpec spec{Raster::makeDenseSquare(1), Raster::ReductionOp::Mean, Raster::BoundaryRule::NoData};
    double noData = -9999.0;

    std::shared_ptr<BufferManager> bufferManager = BufferManager::create();
    MockedPipelineContext pipelineCtx{bufferManager};
    std::map<uint64_t, uint64_t> nextSequenceNumber;   // per input origin, contiguous from 1

    std::shared_ptr<FocalFrameOperatorHandler> makeHandler(const std::vector<OriginId>& origins, uint64_t slide = WindowSize)
    {
        auto store = std::make_unique<DefaultTimeBasedSliceStore>(WindowSize, slide, SliceCacheConfiguration{});
        auto handler = std::make_shared<FocalFrameOperatorHandler>(origins, OriginId(10), std::move(store), WindowSize, slide, geometry, spec, noData);
        handler->start(pipelineCtx, 0);
        return handler;
    }

    /// One buffer of `origin` with watermark `ts` has been processed: windows ending before ts are triggered
    void advanceWatermark(FocalFrameOperatorHandler& handler, OriginId origin, uint64_t ts)
    {
        const SequenceData seq(SequenceNumber(++nextSequenceNumber[origin.getRawValue()]), ChunkNumber(ChunkNumber::INITIAL), true);
        handler.checkAndTriggerWindows(BufferMetaData(Timestamp(ts), seq, origin), &pipelineCtx);
    }

    void sendText(FocalFrameOperatorHandler& handler, OriginId origin, int32_t x, int32_t y, uint64_t ts)
    {
        std::vector<double> cells(16, static_cast<double>((y * 10) + x));
        const Raster::GridView grid{cells.data(), 4, 4, noData};
        const Raster::TileSpec textSpec{Raster::WireFormat::Text, Raster::CellType::Float64, 4, 4, noData};
        std::vector<int8_t> maplet(Raster::encodedSizeTextUpperBound(textSpec));
        uint64_t written = 0;
        ASSERT_EQ(Raster::encode(grid, textSpec, nullptr, maplet.data(), maplet.size(), written), Raster::CodecStatus::Ok);
        handler.onTileArrival(origin, x, y, Timestamp(ts), FrameLineage{}, maplet.data(), written, &pipelineCtx);
    }

    void sendBinary(
        FocalFrameOperatorHandler& handler, OriginId origin, uint32_t frameId, int32_t x, int32_t y, uint64_t ts, FrameLineage lineage = {}, uint8_t bands = 1)
    {
        std::vector<double> cells(16 * bands, static_cast<double>((y * 10) + x));
        const Raster::GridView grid{cells.data(), 4, 4, noData, bands};
        Raster::TileSpec binarySpec{Raster::WireFormat::Binary, Raster::CellType::Float64, 4, 4, noData, bands};
        binarySpec.rasterWidth = 12;
        binarySpec.rasterHeight = 8;
        const Raster::MapletPlacement placement{frameId, x, y, 0};
        std::vector<int8_t> maplet(Raster::encodedSizeBinary(binarySpec));
        uint64_t written = 0;
        ASSERT_EQ(Raster::encode(grid, binarySpec, &placement, maplet.data(), maplet.size(), written), Raster::CodecStatus::Ok);
        /// position fields are ignored for binary maplets
        handler.onTileArrival(origin, -1, -1, Timestamp(ts), lineage, maplet.data(), written, &pipelineCtx);
    }

    /// Sends all tiles of a 3x2 frame in row-major order, optionally skipping one position
    void sendFrame(FocalFrameOperatorHandler& handler, OriginId origin, uint64_t ts, std::pair<int32_t, int32_t> skip = {-1, -1})
    {
        for (int32_t y = 0; y < 2; ++y)
        {
            for (int32_t x = 0; x < 3; ++x)
            {
                if (std::pair{x, y} != skip)
                {
                    sendText(handler, origin, x, y, ts);
                }
            }
        }
    }

    std::vector<Output> outputs()
    {
        std::vector<Output> result;
        for (const auto& buffer : *pipelineCtx.buffers.rlock())
        {
            const auto* d = reinterpret_cast<const EmittedFocalOutput*>(buffer.getAvailableMemoryArea().data());
            result.push_back(
                {d->outputTileX,
                 d->outputTileY,
                 buffer.getNumberOfChildBuffers(),
                 d->outFrame,
                 d->flags,
                 d->complete,
                 d->windowInfo.windowStart.getRawValue(),
                 FrameLineage{d->srcId, d->srcFrameId},
                 d->bands,
                 static_cast<Raster::WireFormat>(d->format)});
        }
        return result;
    }

    /// Every (frame, position, window) exactly once
    static void expectEachOutputOnce(const std::vector<Output>& out, size_t expected)
    {
        std::set<std::tuple<uint32_t, int32_t, int32_t, uint64_t>> seen;
        for (const auto& o : out)
        {
            EXPECT_TRUE(seen.insert({o.outFrame, o.x, o.y, o.windowStart}).second) << "(" << o.x << "," << o.y << ") emitted twice";
        }
        EXPECT_EQ(seen.size(), expected);
    }

    static uint32_t neighbours(int32_t x) { return x == 1 ? 6 : 4; }   // 3x2 grid, reach 1
};

/// A complete frame is emitted eagerly, before its window is triggered; the trigger adds nothing.
TEST_F(FocalFrameOperatorHandlerTest, EagerEmission)
{
    auto handler = makeHandler({originA});
    sendFrame(*handler, originA, 1500);

    auto out = outputs();
    ASSERT_EQ(out.size(), 6U);
    expectEachOutputOnce(out, 6);
    for (const auto& o : out)
    {
        EXPECT_TRUE(o.complete);
        EXPECT_EQ(o.flags, 0);
        EXPECT_EQ(o.outFrame, 1U);
        EXPECT_EQ(o.windowStart, 1000U);
        EXPECT_EQ(o.children, neighbours(o.x));
    }

    advanceWatermark(*handler, originA, 2001);
    EXPECT_EQ(outputs().size(), 6U);
}

/// One tile missing: rest emitted at the trigger, missing position FILLED; late tiles dropped
TEST_F(FocalFrameOperatorHandlerTest, DeadlineEmitsTheRest)
{
    auto handler = makeHandler({originA});
    sendFrame(*handler, originA, 1500, {2, 1});
    ASSERT_EQ(outputs().size(), 2U);   // column 0 does not read column 2

    advanceWatermark(*handler, originA, 2001);
    const auto out = outputs();
    ASSERT_EQ(out.size(), 6U);
    expectEachOutputOnce(out, 6);
    for (const auto& o : out)
    {
        EXPECT_EQ(o.complete, o.x == 0);
        EXPECT_EQ(o.flags, (o.x == 2 && o.y == 1) ? Raster::FLAG_FILLED : 0);
        EXPECT_EQ(o.children, o.x == 0 ? neighbours(o.x) : neighbours(o.x) - 1);
    }

    sendText(*handler, originA, 2, 1, 1500);
    EXPECT_EQ(outputs().size(), 6U);
}

TEST_F(FocalFrameOperatorHandlerTest, DuplicatesAreDropped)
{
    auto handler = makeHandler({originA});
    sendFrame(*handler, originA, 1500);
    sendFrame(*handler, originA, 1500);
    expectEachOutputOnce(outputs(), 6);
}

/// Two frames in one window are computed and numbered independently.
TEST_F(FocalFrameOperatorHandlerTest, SeveralFramesPerWindow)
{
    auto handler = makeHandler({originA});
    sendFrame(*handler, originA, 1100);
    sendFrame(*handler, originA, 1700);
    const auto out = outputs();
    expectEachOutputOnce(out, 12);
    std::set<uint32_t> frames;
    for (const auto& o : out)
    {
        frames.insert(o.outFrame);
    }
    EXPECT_EQ(frames, (std::set<uint32_t>{1, 2}));
}

/// Binary maplets of two origins with the same frame id are different frames.
TEST_F(FocalFrameOperatorHandlerTest, BinaryFramesOfTwoOrigins)
{
    auto handler = makeHandler({originA, originB});
    for (int32_t y = 0; y < 2; ++y)
    {
        for (int32_t x = 0; x < 3; ++x)
        {
            sendBinary(*handler, originA, 5, x, y, 1500);
            sendBinary(*handler, originB, 5, x, y, 1500);
        }
    }
    const auto out = outputs();
    expectEachOutputOnce(out, 12);
    for (const auto& o : out)
    {
        EXPECT_TRUE(o.complete);
        EXPECT_EQ(o.children, neighbours(o.x));
    }
}

/// Lineage copied to every output; disagreeing lineage rejected
TEST_F(FocalFrameOperatorHandlerTest, LineageReachesEveryOutput)
{
    auto handler = makeHandler({originA});
    const FrameLineage lineage{3, 42};
    for (int32_t y = 0; y < 2; ++y)
    {
        for (int32_t x = 0; x < 3; ++x)
        {
            sendBinary(*handler, originA, 5, x, y, 1500, lineage);
        }
    }
    const auto out = outputs();
    expectEachOutputOnce(out, 6);
    for (const auto& o : out)
    {
        EXPECT_EQ(o.lineage, lineage);
    }

    sendBinary(*handler, originA, 6, 0, 0, 1600, lineage);
    EXPECT_ANY_THROW(sendBinary(*handler, originA, 6, 1, 0, 1600, FrameLineage{3, 43}));
}

/// Outputs encoded like the input frame; mixed band counts rejected
TEST_F(FocalFrameOperatorHandlerTest, FormatReachesEveryOutput)
{
    auto handler = makeHandler({originA});
    sendFrame(*handler, originA, 1100);
    for (int32_t y = 0; y < 2; ++y)
    {
        for (int32_t x = 0; x < 3; ++x)
        {
            sendBinary(*handler, originA, 5, x, y, 1500, {}, 2);
        }
    }
    const auto out = outputs();
    expectEachOutputOnce(out, 12);
    for (const auto& o : out)
    {
        const bool binary = o.outFrame == 2; // the text frame arrived first and got output frame 1
        EXPECT_EQ(o.format, binary ? Raster::WireFormat::Binary : Raster::WireFormat::Text);
        EXPECT_EQ(o.bands, binary ? 2 : 1);
    }

    sendBinary(*handler, originA, 6, 0, 0, 1600, {}, 2);
    EXPECT_ANY_THROW(sendBinary(*handler, originA, 6, 1, 0, 1600, {}, 3));
}

TEST_F(FocalFrameOperatorHandlerTest, BinaryMapletWithoutFrameIdThrows)
{
    auto handler = makeHandler({originA});
    EXPECT_ANY_THROW(sendBinary(*handler, originA, 0, 0, 0, 1500));
}

/// Sliding windows: each output once per window; the first trigger is the deadline
TEST_F(FocalFrameOperatorHandlerTest, SlidingWindows)
{
    auto handler = makeHandler({originA}, WindowSize / 2);
    sendFrame(*handler, originA, 1200, {2, 1});
    ASSERT_EQ(outputs().size(), 4U);   // column 0, two windows

    advanceWatermark(*handler, originA, 1501);
    auto out = outputs();
    ASSERT_EQ(out.size(), 12U);
    expectEachOutputOnce(out, 12);
    std::set<uint64_t> windowStarts;
    for (const auto& o : out)
    {
        windowStarts.insert(o.windowStart);
    }
    EXPECT_EQ(windowStarts, (std::set<uint64_t>{500, 1000}));

    advanceWatermark(*handler, originA, 2001);
    EXPECT_EQ(outputs().size(), 12U);
}

}
}
