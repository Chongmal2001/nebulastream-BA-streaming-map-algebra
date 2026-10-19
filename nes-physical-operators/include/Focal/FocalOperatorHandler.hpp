
#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <shared_mutex>
#include <map>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <SliceStore/Slice.hpp>
#include <SliceStore/SliceAssigner.hpp>
#include <WindowBasedOperatorHandler.hpp>
#include <FocalSpec.hpp>
#include <Focal/FocalPolicy.hpp>
#include <Focal/FocalSlice.hpp>
#include <Focal/FocalWindowState.hpp>
#include <FocalDriver.hpp>

namespace NES
{
struct EmittedFocalOutput
{
    EmittedFocalOutput(WindowInfo windowInfo, int32_t outputTileX, int32_t outputTileY, uint64_t presenceMask)
        : windowInfo(windowInfo), outputTileX(outputTileX), outputTileY(outputTileY), presenceMask(presenceMask)
    {
    }

    WindowInfo windowInfo;
    int32_t outputTileX;
    int32_t outputTileY;
    uint64_t presenceMask;
};

class FocalOperatorHandler : public WindowBasedOperatorHandler
{
public:
    FocalOperatorHandler(
        const std::vector<OriginId>& inputOrigins,
        OriginId outputOriginId,
        std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore)
        : WindowBasedOperatorHandler(inputOrigins, outputOriginId, std::move(sliceAndWindowStore))
    {
    }

    ~FocalOperatorHandler() override = default;


    virtual void onTileArrival(
        int32_t tileX,
        int32_t tileY,
        Timestamp eventTime,
        const int8_t* payload,
        uint64_t payloadLen,
        PipelineExecutionContext* pipelineCtx)
        = 0;

};

/// Templated implementation. One source, two instantiations (single/multi),
/// selected in the lowering rule. Holds FocalSlice<Policy>; the policy fixes the
/// counter type and the decrement primitive. Build/probe never name this type.
template <typename Policy>
class FocalOperatorHandlerImpl final : public FocalOperatorHandler
{
public:
    FocalOperatorHandlerImpl(
        const std::vector<OriginId>& inputOrigins,
        OriginId outputOriginId,
        std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore,
        const uint64_t windowSize,
        const uint64_t windowSlide,
        Raster::RasterGeometry geometry,
        const Raster::FocalSpec& focalSpec,
        double noData);

    void onTileArrival(
        int32_t tileX,
        int32_t tileY,
        Timestamp eventTime,
        const int8_t* payload,
        uint64_t payloadLen,
        PipelineExecutionContext* pipelineCtx) override;

    [[nodiscard]] std::function<std::vector<std::shared_ptr<Slice>>(SliceStart, SliceEnd)>
    getCreateNewSlicesFunction(const CreateNewSlicesArguments& newSlicesArguments) const override;

protected:
    void triggerSlices(
        const std::map<WindowInfoAndSequenceNumber, std::vector<std::shared_ptr<Slice>>>& slicesAndWindowInfo,
        PipelineExecutionContext* pipelineCtx) override;

private:

    Raster::RasterGeometry geometry;
    double noData;

    // slice-window arithmetic held here (not just in the store) because eager emission needs it mid-window
    SliceAssigner sliceAssigner;

    // Precomputed dependency geometry
    Raster::TileReach tileReach;
    Raster::TileSpec tileSpec;

    std::atomic<uint64_t> outputSequenceNumber{1};

    // Per window completeness state
    std::map<WindowInfo, std::shared_ptr<FocalWindowState<Policy>>> windowStates;
    Policy::Mutex windowStatesMutex;

    // creates (and seeds) or retrieves completeness state for windowInfo
    std::shared_ptr<FocalWindowState<Policy>> getOrCreateWindowState(WindowInfo windowInfo);

    // Reconstruct the atomic slices composing a window (the store has no direct slices of window W lookup)
    std::vector<std::shared_ptr<Slice>> slicesForWindow(WindowInfo windowInfo);

    // Emits one completed output tile as an EmittedFocalOutput descriptor to the probe
    void emitOutput(WindowInfo windowInfo, int32_t outX, int32_t outY, PipelineExecutionContext* pipelineCtx);
};

}