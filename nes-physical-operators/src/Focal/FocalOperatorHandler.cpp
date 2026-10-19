#include <memory>
#include <mutex>
#include <shared_mutex>

#include <Focal/FocalOperatorHandler.hpp>
#include <Focal/FocalPolicy.hpp>
#include <Focal/FocalSlice.hpp>
#include <ErrorHandling.hpp>
#include <SliceStore/WindowSlicesStoreInterface.hpp>

namespace NES
{
using Raster::TilePosition;

template <typename Policy>
FocalOperatorHandlerImpl<Policy>::FocalOperatorHandlerImpl(
    const std::vector<OriginId>& inputOrigins,
    OriginId outputOriginId,
    std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore,
    const uint64_t windowSize,
    const uint64_t windowSlide,
    Raster::RasterGeometry geometry,
    const Raster::FocalSpec& focalSpec,
    double noData)
    : FocalOperatorHandler(inputOrigins, outputOriginId, std::move(sliceAndWindowStore))
    , geometry(std::move(geometry))
    , noData(noData)
    , sliceAssigner(windowSize, windowSlide)
    , tileReach(Raster::computeTileReach(geometry, focalSpec.n.radius))
    , tileSpec{
        Raster::WireFormat::Text,
        Raster::CellType::Float64,
        static_cast<uint32_t>(geometry.tileW),
        static_cast<uint32_t>(geometry.tileH),
        noData}
{
}

template <typename Policy>
std::function<std::vector<std::shared_ptr<Slice>>(SliceStart, SliceEnd)>
FocalOperatorHandlerImpl<Policy>::getCreateNewSlicesFunction(const CreateNewSlicesArguments& newSlicesArguments) const
{
    PRECONDITION(
        numberOfWorkerThreads > 0,
        "Number of worker threads not set for window based operator. Was setWorkerThreads() being called?");

    (void) newSlicesArguments;

    return std::function(
    [geometry = geometry, noData = noData, outputOriginId = outputOriginId](
        SliceStart sliceStart, SliceEnd sliceEnd) -> std::vector<std::shared_ptr<Slice>>
    {
        NES_TRACE("Creating new focal slice {}-{} for output origin {}", sliceStart, sliceEnd, outputOriginId);
        return {std::make_shared<FocalSlice<Policy>>(sliceStart, sliceEnd, geometry, noData)};
    });
}



template <typename Policy>
std::shared_ptr<FocalWindowState<Policy>> FocalOperatorHandlerImpl<Policy>::getOrCreateWindowState(WindowInfo windowInfo)
{
    // Fast path
    {
        std::shared_lock lock(windowStatesMutex);
        if (auto it = windowStates.find(windowInfo); it != windowStates.end())
        {
            return it->second;
        }
    }
    // Slow path
    // create and seed under unique lock
    std::unique_lock lock(windowStatesMutex);
    auto it = windowStates.find(windowInfo);
    if (it == windowStates.end())
    {
        auto state = std::make_shared<FocalWindowState<Policy>>(geometry);
        state->initializeCounters(tileReach);
        it = windowStates.emplace(windowInfo, std::move(state)).first;
        NES_TRACE("Created focal window state for window {}-{}",
            windowInfo.windowStart, windowInfo.windowEnd);
    }
    return it->second;

}

template <typename Policy>
std::vector<std::shared_ptr<Slice>> FocalOperatorHandlerImpl<Policy>::slicesForWindow(WindowInfo windowInfo)
{
    std::vector<std::shared_ptr<Slice>> windowSlices;
    auto& store = getSliceAndWindowStore();

    uint64_t cursor = windowInfo.windowStart.getRawValue();
    const uint64_t windowEnd = windowInfo.windowEnd.getRawValue();

    while (cursor < windowEnd)
    {
        const SliceEnd sliceEnd = sliceAssigner.getSliceEndTs(Timestamp(cursor));
        INVARIANT(sliceEnd.getRawValue() > cursor, "SliceAssigner made no progress at ts {}", cursor);

        if (auto slice = store.getSliceBySliceEnd(sliceEnd))
        {
            windowSlices.push_back(*slice);
        }
        cursor = sliceEnd.getRawValue();
    }
    return windowSlices;
}

template <typename Policy>
void FocalOperatorHandlerImpl<Policy>::onTileArrival(
    int32_t tileX,
    int32_t tileY,
    Timestamp eventTime,
    const int8_t* payload,
    uint64_t payloadLen,
    PipelineExecutionContext* pipelineCtx)
{
    const TilePosition pos{tileX, tileY};

    // Resolve/create the window's slice for this tile's event time
    auto slices = getSliceAndWindowStore().getSlicesOrCreate(
        eventTime, getCreateNewSlicesFunction(CreateNewSlicesArguments{}));
    INVARIANT(slices.size() == 1, "One atomic slice per timestamp, got {}", slices.size());
    auto& focalSlice = dynamic_cast<FocalSlice<Policy>&>(*slices.front());

    // Allocate buffer for cell payload, decode into it
    const uint64_t neededBytes = static_cast<uint64_t>(geometry.tileW) * geometry.tileH * sizeof(double);
    auto bufferOpt = pipelineCtx->getBufferManager()->getUnpooledBuffer(neededBytes);
    if (not bufferOpt.has_value())
    {
        throw CannotAllocateBuffer("{}B for a tile were requested", neededBytes);
    }
    auto tileBuffer = bufferOpt.value();

    const auto cellSpan = tileBuffer.getAvailableMemoryArea<double>();
    Raster::GridView unusedView;
    if (Raster::decode(payload, payloadLen, tileSpec, cellSpan.data(), cellSpan.size(), unusedView) != Raster::CodecStatus::Ok)
    {
        NES_WARNING("Dropping malformed tile ({},{})", tileX, tileY);
        return;  // Malformed tiles are dropped, not stored
    }

    // Store the tile's cells once in its slice
    // windowRefCount = how many windows share this slice and will need this tile
    const auto windows = sliceAssigner.getAllWindowsForSlice(*slices.front());
    focalSlice.storeTile(pos, std::move(tileBuffer), static_cast<uint32_t>(windows.size()));

    // Fan out to every window containing this slice. Completeness tracked independently per window
    for (const auto& windowInfo : windows)
    {
        auto windowState = getOrCreateWindowState(windowInfo);
        // Count each position toward a window at most once => same position can arrive in multiple slice of on (sliding) window and recounting would double decrement missing
        if (windowState->testAndMarkPresent(pos))
        {
            continue;
        }

        // For each output this tile feeds, decrement its missing count => an output reaching zero has a complete neighbourhood and is emitted eagerly
        Raster::forEachNeighbourTile(geometry, tileX, tileY, tileReach, [&](int32_t outX, int32_t outY)
        {
            if (windowState->decrementMissing({outX, outY}))
            {
                // TODO: implement eager eviction in addition to eager emission. For now tiles are evicted at window-close
                emitOutput(windowInfo, outX, outY, pipelineCtx);
            }
        });
    }
}

template <typename Policy>
void FocalOperatorHandlerImpl<Policy>::triggerSlices(
    const std::map<WindowInfoAndSequenceNumber, std::vector<std::shared_ptr<Slice>>>& slicesAndWindowInfo,
    PipelineExecutionContext* pipelineCtx)
{
    for (const auto& [windowAndSeq, slices] : slicesAndWindowInfo)
    {
        const WindowInfo windowInfo = windowAndSeq.windowInfo;

        std::shared_ptr<FocalWindowState<Policy>> windowState;
        {
            std::unique_lock lock(windowStatesMutex);
            if (auto it = windowStates.find(windowInfo); it != windowStates.end())
            {
                windowState = std::move(it->second);
                windowStates.erase(it);
            }
        }
        // Everything complete was already emitted eagerly => force emit on stragglers, with whatever partial neighbourhood exists
        if (windowState)
        {
            windowState->forEachIncompleteOutput([&](int32_t outX, int32_t outY)
            {
                emitOutput(windowInfo, outX, outY, pipelineCtx);
            });
        }

        // Release this window's hold on every slice's tiles
        for (const auto& baseSlice : slices)
        {
            dynamic_cast<FocalSlice<Policy>&>(*baseSlice).releaseWindowAndEvict();
        }
    }
}

template <typename Policy>
void FocalOperatorHandlerImpl<Policy>::emitOutput(
    WindowInfo windowInfo, int32_t outX, int32_t outY, PipelineExecutionContext* pipelineCtx)
{
    // Find all slices
    const auto windowSlices = slicesForWindow(windowInfo);
    std::vector<FocalSlice<Policy>*> focalSlices;

    focalSlices.reserve(windowSlices.size());
    for (const auto& baseSlice : windowSlices)
    {
        focalSlices.push_back(&dynamic_cast<FocalSlice<Policy>&>(*baseSlice));
    }

    // Allocate an unpooled buffer sized for EmittedFocalOutput
    // Allocate first, before drawing a sequence number => failed allocation should never consume a sequence number it won't emit
    constexpr auto neededBufferSize = sizeof(EmittedFocalOutput);
    const auto bufferOpt = pipelineCtx->getBufferManager()->getUnpooledBuffer(neededBufferSize);
    if (not bufferOpt.has_value())
    {
        throw CannotAllocateBuffer("{}B for a focal output descriptor were requested", neededBufferSize);
    }
    auto buffer = bufferOpt.value();

    // unpooled buffer may hold stale bytes => zero the buffer before placement
    std::ranges::fill(buffer.getAvailableMemoryArea<>(), std::byte{0});

    uint64_t presenceMask = 0;
    uint32_t neighbourIndex = 0;
    uint32_t expectedKey = 0; // number of present neigbbours attached so far
    Raster::forEachNeighbourTile(geometry, outX, outY, tileReach,
        [&](int32_t depX, int32_t depY)
        {
            INVARIANT(neighbourIndex < 64,
                "presenceMask overflow: neighbourhood exceeds 64 tiles at output ({}, {})", outX, outY);

            for (auto* focalSlice: focalSlices)
            {
                if (auto tile = focalSlice->tryGetBuffer({depX, depY}))
                {
                    const uint32_t key = buffer.storeChildBuffer(*tile).getRawIndex();
                    INVARIANT(key == expectedKey, "child buffer key {} out of sequence (expected {}); probe addresses children by attach order", key, expectedKey);
                    presenceMask |= (uint64_t{1} << neighbourIndex);
                    ++expectedKey;
                    break;
                }
            }
            ++neighbourIndex;
        });

    buffer.setOriginId(outputOriginId);
    buffer.setChunkNumber(ChunkNumber(ChunkNumber::INITIAL));
    buffer.setLastChunk(true);
    buffer.setWatermark(windowInfo.windowStart);
    buffer.setNumberOfTuples(1); // one descriptor = one output tile the probe will consume
    buffer.setCreationTimestampInMS(Timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count()));

    auto memArea = buffer.getAvailableMemoryArea();
    new (memArea.data()) EmittedFocalOutput{windowInfo, outX, outY, presenceMask};

    buffer.setSequenceNumber(SequenceNumber(outputSequenceNumber.fetch_add(1)));
    pipelineCtx->emitBuffer(buffer);
    NES_TRACE("Emitted focal output ({},{}) window {}-{} seq {} deps {}",
        outX, outY, windowInfo.windowStart, windowInfo.windowEnd,
        buffer.getSequenceNumber(), std::popcount(presenceMask));


}

template class FocalOperatorHandlerImpl<SingleThreadedPolicy>;
template class FocalOperatorHandlerImpl<MultiThreadedPolicy>;

}