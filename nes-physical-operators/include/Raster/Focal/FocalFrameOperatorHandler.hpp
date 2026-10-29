#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <Runtime/AbstractBufferProvider.hpp>
#include <SliceStore/Slice.hpp>
#include <SliceStore/SliceAssigner.hpp>
#include <SliceStore/WindowSlicesStoreInterface.hpp>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>
#include <Raster/Focal/FocalOperatorHandler.hpp>
#include <Raster/Focal/FocalSlice.hpp>

namespace NES
{

/// Slice creation arguments: buffer provider for the frame table
struct CreateNewFocalSliceArguments final : CreateNewSlicesArguments
{
    std::shared_ptr<AbstractBufferProvider> bufferProvider;
};

/// Focal handler: per-frame state in the slice, every output emitted once per window
class FocalFrameOperatorHandler final : public FocalOperatorHandler
{
public:
    FocalFrameOperatorHandler(
        const std::vector<OriginId>& inputOrigins,
        OriginId outputOriginId,
        std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore,
        uint64_t windowSize,
        uint64_t windowSlide,
        Raster::RasterGeometry geometry,
        const Raster::FocalSpec& focalSpec,
        double noData);

    void onTileArrival(
        OriginId originId,
        int32_t tileX,
        int32_t tileY,
        Timestamp eventTime,
        FrameLineage lineage,
        const int8_t* payload,
        uint64_t payloadLen,
        PipelineExecutionContext* pipelineCtx) override;

    /// Expects CreateNewFocalSliceArguments.
    [[nodiscard]] std::function<std::vector<std::shared_ptr<Slice>>(SliceStart, SliceEnd)>
    getCreateNewSlicesFunction(const CreateNewSlicesArguments& newSlicesArguments) const override;

protected:
    void triggerSlices(
        const std::map<WindowInfoAndSequenceNumber, std::vector<std::shared_ptr<Slice>>>& slicesAndWindowInfo,
        PipelineExecutionContext* pipelineCtx) override;

private:
    /// Finalises output o, emits it per window, releases inputs no longer needed
    void emitOutput(
        const std::vector<WindowInfo>& windows, Focal::FocalFrameStateT& fs, Raster::TilePosition o, PipelineExecutionContext* pipelineCtx);

    Raster::RasterGeometry geometry;
    SliceAssigner sliceAssigner;
    Raster::TileReach tileReach;
    Raster::TileSpec tileSpec;   // text maplets; binary maplets take format, bands and raster size from here + their header

    std::atomic<uint64_t> outputSequenceNumber{1};
    std::atomic<uint32_t> outputFrameCounter{1};
};

}
