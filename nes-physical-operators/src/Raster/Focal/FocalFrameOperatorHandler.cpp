#include <Raster/Focal/FocalFrameOperatorHandler.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <Runtime/TupleBuffer.hpp>
#include <SliceStore/WindowSlicesStoreInterface.hpp>
#include <Util/Logger/Logger.hpp>
#include <ErrorHandling.hpp>
#include <PipelineExecutionContext.hpp>

namespace NES
{
using Raster::TilePosition;

FocalFrameOperatorHandler::FocalFrameOperatorHandler(
    const std::vector<OriginId>& inputOrigins,
    OriginId outputOriginId,
    std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore,
    const uint64_t windowSize,
    const uint64_t windowSlide,
    Raster::RasterGeometry geometry,
    const Raster::FocalSpec& focalSpec,
    double noData)
    : FocalOperatorHandler(inputOrigins, outputOriginId, std::move(sliceAndWindowStore))
    , geometry(geometry)
    , sliceAssigner(windowSize, windowSlide)
    , tileReach(Raster::computeTileReach(geometry, focalSpec.n.radius))
    , tileSpec{Raster::WireFormat::Text, Raster::CellType::Float64, static_cast<uint32_t>(geometry.tileW), static_cast<uint32_t>(geometry.tileH), noData}
{
}

std::function<std::vector<std::shared_ptr<Slice>>(SliceStart, SliceEnd)>
FocalFrameOperatorHandler::getCreateNewSlicesFunction(const CreateNewSlicesArguments& newSlicesArguments) const
{
    PRECONDITION(
        numberOfWorkerThreads > 0, "Number of worker threads not set for window based operator. Was setWorkerThreads() being called?");
    const auto& args = dynamic_cast<const CreateNewFocalSliceArguments&>(newSlicesArguments);

    return std::function(
        [bufferProvider = args.bufferProvider, outputOriginId = outputOriginId](
            SliceStart sliceStart, SliceEnd sliceEnd) -> std::vector<std::shared_ptr<Slice>>
        {
            NES_TRACE("Creating new focal slice {}-{} for output origin {}", sliceStart, sliceEnd, outputOriginId);
            return {std::make_shared<Focal::FocalSlice>(sliceStart, sliceEnd, bufferProvider)};
        });
}

void FocalFrameOperatorHandler::onTileArrival(
    OriginId originId,
    int32_t tileX,
    int32_t tileY,
    Timestamp eventTime,
    FrameLineage lineage,
    const int8_t* payload,
    uint64_t payloadLen,
    PipelineExecutionContext* pipelineCtx)
{
    /// Frame key and position: binary maplets carry them, text uses tuple fields and (origin, low 32 bits of ts)
    const auto originIt = std::ranges::find(inputOrigins, originId);
    INVARIANT(originIt != inputOrigins.end(), "Focal received origin {}, which is not an input origin", originId);
    FrameKey key{static_cast<uint32_t>(std::distance(inputOrigins.begin(), originIt)), static_cast<uint32_t>(eventTime.getRawValue())};
    TilePosition pos{tileX, tileY};
    Raster::TileSpec spec = tileSpec;
    if (payloadLen > 0 && payload[0] == static_cast<int8_t>(Raster::WireFormat::Binary))
    {
        Raster::MapletHeader header;
        if (const auto status = Raster::peekHeader(payload, payloadLen, header); status != Raster::CodecStatus::Ok)
        {
            throw FormattingError("Focal: invalid maplet: {}", Raster::toString(status));
        }
        if (header.frameId == 0)
        {
            throw FormattingError("Focal: maplet ({}, {}) has no frame id", header.tileX, header.tileY);
        }
        key.frameId = header.frameId;
        pos = TilePosition{header.tileX, header.tileY};
        spec.format = Raster::WireFormat::Binary;
        spec.bands = header.bands;
        spec.rasterWidth = static_cast<uint32_t>(geometry.rasterW);
        spec.rasterHeight = static_cast<uint32_t>(geometry.rasterH);
    }
    if (pos.x < 0 || pos.y < 0 || pos.x >= geometry.tilesX() || pos.y >= geometry.tilesY())
    {
        throw FormattingError("Focal: tile ({}, {}) outside the {}x{} grid", pos.x, pos.y, geometry.tilesX(), geometry.tilesY());
    }

    /// The tile's slice owns its frame
    CreateNewFocalSliceArguments sliceArgs;
    sliceArgs.numberOfWorkerThreads = numberOfWorkerThreads;
    sliceArgs.bufferProvider = pipelineCtx->getBufferManager();
    auto slices = getSliceAndWindowStore().getSlicesOrCreate(eventTime, getCreateNewSlicesFunction(sliceArgs));
    INVARIANT(slices.size() == 1, "One atomic slice per timestamp, got {}", slices.size());
    auto& slice = dynamic_cast<Focal::FocalSlice&>(*slices.front());
    if (slice.deadlinePassed())
    {
        NES_DEBUG("Focal: dropping late tile ({},{}) at ts {}", pos.x, pos.y, eventTime);
        return;
    }

    auto [fs, created] = slice.frameTable().getOrCreate(
        key,
        Focal::FocalFrameStateT::bytesFor(geometry),
        [&](std::span<std::byte> memory)
        {
            return Focal::FocalFrameStateT::create(
                memory,
                key,
                eventTime.getRawValue(),
                outputFrameCounter.fetch_add(1),
                geometry,
                tileReach,
                lineage,
                Focal::FrameFormat{spec.format, spec.bands});
        });
    if (fs->lineage() != lineage)
    {
        throw FormattingError(
            "Focal: frame {} seen with lineage ({}, {}) and ({}, {})",
            key.frameId,
            fs->lineage().srcId,
            fs->lineage().srcFrameId,
            lineage.srcId,
            lineage.srcFrameId);
    }
    if (fs->format() != Focal::FrameFormat{spec.format, spec.bands})
    {
        throw FormattingError("Focal: frame {} mixes tile formats or band counts ({} bands, then {})", key.frameId, fs->format().bands, spec.bands);
    }
    if (fs->ts() != eventTime.getRawValue())
    {
        throw FormattingError("Focal: frame {} seen with ts {} and {}", key.frameId, fs->ts(), eventTime);
    }

    /// Decode once into its own buffer; every output that reads the tile uses that buffer
    DecodedTile tile;
    const uint64_t neededBytes = spec.cellCount() * sizeof(double);
    auto bufferOpt = pipelineCtx->getBufferManager()->getUnpooledBuffer(neededBytes);
    if (not bufferOpt.has_value())
    {
        throw CannotAllocateBuffer("{}B for a tile were requested", neededBytes);
    }
    tile.cells = std::move(bufferOpt.value());
    const auto cells = tile.cells.getAvailableMemoryArea<double>();
    if (const auto status = Raster::decode(payload, payloadLen, spec, cells.data(), cells.size(), tile.view); status != Raster::CodecStatus::Ok)
    {
        throw FormattingError("Focal: cannot decode tile ({}, {}): {}", pos.x, pos.y, Raster::toString(status));
    }

    /// Duplicates are dropped before any counter changes
    if (fs->admit(pos) == Focal::Admission::Duplicate)
    {
        NES_DEBUG("Focal: dropping duplicate tile ({},{}) of frame {}", pos.x, pos.y, key.frameId);
        return;
    }
    fs->storeTile(pos, std::move(tile));

    /// Emit every output this tile completes, for every window containing the slice
    std::vector<WindowInfo> windows;
    fs->forEachCompletedBy(
        pos,
        [&](TilePosition o)
        {
            if (windows.empty())
            {
                windows = sliceAssigner.getAllWindowsForSlice(slice);
            }
            emitOutput(windows, *fs, o, pipelineCtx);
        });
}

void FocalFrameOperatorHandler::triggerSlices(
    const std::map<WindowInfoAndSequenceNumber, std::vector<std::shared_ptr<Slice>>>& slicesAndWindowInfo,
    PipelineExecutionContext* pipelineCtx)
{
    for (const auto& [windowAndSeq, slices] : slicesAndWindowInfo)
    {
        for (const auto& baseSlice : slices)
        {
            /// Deadline: first trigger emits all remaining outputs of the slice
            auto& slice = dynamic_cast<Focal::FocalSlice&>(*baseSlice);
            if (slice.deadlinePassed())
            {
                continue;
            }
            slice.markDeadlinePassed();
            const auto windows = sliceAssigner.getAllWindowsForSlice(slice);
            slice.frameTable().forEach(
                [&](Focal::FocalFrameStateT& fs) { fs.forEachUnemittedOutput([&](TilePosition o) { emitOutput(windows, fs, o, pipelineCtx); }); });
        }
    }
}

void FocalFrameOperatorHandler::emitOutput(
    const std::vector<WindowInfo>& windows, Focal::FocalFrameStateT& fs, TilePosition o, PipelineExecutionContext* pipelineCtx)
{
    if (!fs.claimEmission(o))
    {
        return;
    }
    const bool complete = fs.missingInputs(o) == 0;
    const uint8_t flags = fs.isPresent(o) ? 0 : Raster::FLAG_FILLED;

    for (const auto& windowInfo : windows)
    {
        /// Allocate before drawing a sequence number => a failed allocation never consumes one
        constexpr auto neededBufferSize = sizeof(EmittedFocalOutput);
        auto bufferOpt = pipelineCtx->getBufferManager()->getUnpooledBuffer(neededBufferSize);
        if (not bufferOpt.has_value())
        {
            throw CannotAllocateBuffer("{}B for a focal output descriptor were requested", neededBufferSize);
        }
        auto buffer = bufferOpt.value();
        std::ranges::fill(buffer.getAvailableMemoryArea(), std::byte{0});

        /// Attach the stored neighbour tiles in neighbourhood order; the descriptor holds its own references
        uint64_t presenceMask = 0;
        uint32_t neighbourIndex = 0;
        uint32_t expectedKey = 0;
        Raster::forEachNeighbourTile(
            geometry,
            o.x,
            o.y,
            tileReach,
            [&](int32_t x, int32_t y)
            {
                INVARIANT(neighbourIndex < 64, "presenceMask overflow: neighbourhood exceeds 64 tiles at output ({}, {})", o.x, o.y);
                const TilePosition i{x, y};
                if (fs.isPresent(i) && !fs.tile(i).empty())
                {
                    TupleBuffer child = fs.tile(i).cells;
                    const uint32_t key = buffer.storeChildBuffer(child).getRawIndex();
                    INVARIANT(key == expectedKey, "child buffer key {} out of sequence (expected {})", key, expectedKey);
                    presenceMask |= (uint64_t{1} << neighbourIndex);
                    ++expectedKey;
                }
                ++neighbourIndex;
            });

        buffer.setOriginId(outputOriginId);
        buffer.setChunkNumber(ChunkNumber(ChunkNumber::INITIAL));
        buffer.setLastChunk(true);
        buffer.setWatermark(windowInfo.windowStart);
        buffer.setNumberOfTuples(1);
        buffer.setCreationTimestampInMS(Timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count()));

        new (buffer.getAvailableMemoryArea().data())
            EmittedFocalOutput{
                windowInfo, o.x, o.y, presenceMask, fs.ts(), fs.outFrame(), flags, complete, fs.lineage(), fs.format().bands, fs.format().format};

        buffer.setSequenceNumber(SequenceNumber(outputSequenceNumber.fetch_add(1)));
        pipelineCtx->emitBuffer(buffer);
        NES_TRACE(
            "Emitted focal output ({},{}) frame {} window {}-{} seq {} deps {}",
            o.x,
            o.y,
            fs.outFrame(),
            windowInfo.windowStart,
            windowInfo.windowEnd,
            buffer.getSequenceNumber(),
            std::popcount(presenceMask));
    }

    /// Eager eviction: release inputs no unemitted output reads any more
    fs.forEachReleasableAfter(o, [&](TilePosition i) { fs.releaseTile(i); });
    (void)fs.finishOutput();
}

}
