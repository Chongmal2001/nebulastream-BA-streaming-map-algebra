
#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <SliceStore/Slice.hpp>
#include <SliceStore/SliceAssigner.hpp>
#include <WindowBasedOperatorHandler.hpp>
#include <FocalSpec.hpp>
#include <Raster/FrameTable.hpp>
#include <MapletCodec.hpp>
#include <FocalDriver.hpp>

namespace NES
{
/// Output tile descriptor for the probe; inputs attached as child buffers; new members go last
struct EmittedFocalOutput
{
    EmittedFocalOutput(
        WindowInfo windowInfo,
        int32_t outputTileX,
        int32_t outputTileY,
        uint64_t presenceMask,
        uint64_t eventTs = 0,
        uint32_t outFrame = 0,
        uint8_t flags = 0,
        bool complete = true,
        FrameLineage lineage = {},
        uint8_t bands = 1,
        Raster::WireFormat format = Raster::WireFormat::Text)
        : windowInfo(windowInfo)
        , outputTileX(outputTileX)
        , outputTileY(outputTileY)
        , presenceMask(presenceMask)
        , eventTs(eventTs)
        , outFrame(outFrame)
        , flags(flags)
        , complete(complete)
        , srcId(lineage.srcId)
        , srcFrameId(lineage.srcFrameId)
        , bands(bands)
        , format(static_cast<uint8_t>(format))
    {
    }

    WindowInfo windowInfo;
    int32_t outputTileX;
    int32_t outputTileY;
    uint64_t presenceMask;
    uint64_t eventTs;    // event time of the frame
    uint32_t outFrame;   // output frame number; 0 = unassigned
    uint8_t flags;       // maplet flags for the output, e.g. Raster::FLAG_FILLED
    bool complete;       // false: emitted at the deadline with at least one neighbour missing
    uint32_t srcId;      // lineage of the frame (FrameLineage): input store; 0 = unknown
    uint32_t srcFrameId; // lineage of the frame: the store's frame number; 0 = unknown
    uint8_t bands;       // bands of the attached tiles and of the output
    uint8_t format;      // Raster::WireFormat of the output, that of the frame's input: text or binary
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


    /// Called by the build per input tuple; tileX / tileY only for text maplets, lineage 0 if absent
    virtual void onTileArrival(
        OriginId originId,
        int32_t tileX,
        int32_t tileY,
        Timestamp eventTime,
        FrameLineage lineage,
        const int8_t* payload,
        uint64_t payloadLen,
        PipelineExecutionContext* pipelineCtx)
        = 0;

};

}
