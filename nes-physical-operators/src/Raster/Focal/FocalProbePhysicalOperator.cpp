#include <Raster/Focal/FocalProbePhysicalOperator.hpp>
#include <cstdlib>
#include <utility>

#include <Raster/Focal/FocalOperatorHandler.hpp>
#include <Interface/NautilusBuffer.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <ExecutionContext.hpp>
#include <FocalSpec.hpp>
#include <FocalDriver.hpp>
#include <FocalKernels.hpp>
#include <MapletCodec.hpp>

namespace NES
{
namespace
{
/// NES_FOCAL_SUMMATION = exact | running | kahan (default exact), read once
Raster::Summation focalSummation()
{
    static const Raster::Summation mode = []
    {
        const char* value = std::getenv("NES_FOCAL_SUMMATION");
        const auto parsed = Raster::parseSummation(value == nullptr ? "" : value);
        INVARIANT(parsed.has_value(), "NES_FOCAL_SUMMATION must be exact, running or kahan, got '{}'", value);
        return *parsed;
    }();
    return mode;
}
}

FocalProbePhysicalOperator::FocalProbePhysicalOperator(
    OperatorHandlerId operatorHandlerId,
    WindowMetaData windowMetaData,
    Raster::RasterGeometry geometry,
    Raster::FocalSpec focalSpec,
    Raster::TileSpec tileSpec,
    double noData,
    Record::RecordFieldIdentifier outputTileXField,
    Record::RecordFieldIdentifier outputTileYField,
    Record::RecordFieldIdentifier outputPayloadField,
    std::optional<Record::RecordFieldIdentifier> outputSrcIdField,
    std::optional<Record::RecordFieldIdentifier> outputSrcFrameIdField,
    std::optional<Record::RecordFieldIdentifier> outputTsField)
    : WindowProbePhysicalOperator(operatorHandlerId, std::move(windowMetaData))
    , geometry(geometry)
    , focalSpec(focalSpec)
    , tileReach(Raster::computeTileReach(geometry, focalSpec.n.radius))
    , tileSpec(tileSpec)
    , noData(noData)
    , outputTileXField(std::move(outputTileXField))
    , outputTileYField(std::move(outputTileYField))
    , outputPayloadField(std::move(outputPayloadField))
    , outputSrcIdField(std::move(outputSrcIdField))
    , outputSrcFrameIdField(std::move(outputSrcFrameIdField))
    , outputTsField(std::move(outputTsField))
{
}

void FocalProbePhysicalOperator::open(ExecutionContext& executionCtx, RecordBuffer& recordBuffer) const
{
    executionCtx.watermarkTs = recordBuffer.getWatermarkTs();
    executionCtx.currentTs = recordBuffer.getCreatingTs();
    executionCtx.sequenceNumber = recordBuffer.getSequenceNumber();
    executionCtx.chunkNumber = recordBuffer.getChunkNumber();
    executionCtx.lastChunk = recordBuffer.isLastChunk();
    executionCtx.originId = recordBuffer.getOriginId();
    openChild(executionCtx, recordBuffer);

    // Read descriptor header out of buffer payload
    const auto descriptorRef = static_cast<nautilus::val<EmittedFocalOutput*>>(recordBuffer.getMemArea());
    const auto outX = readValueFromMemRef<int32_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::outputTileX));
    const auto outY = readValueFromMemRef<int32_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::outputTileY));
    const auto presenceMask = readValueFromMemRef<uint64_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::presenceMask));
    const auto windowInfoRef = getMemberRef(descriptorRef, &EmittedFocalOutput::windowInfo);
    const nautilus::val<Timestamp> windowStart{readValueFromMemRef<uint64_t>(getMemberRef(windowInfoRef, &WindowInfo::windowStart))};
    const nautilus::val<Timestamp> windowEnd  {readValueFromMemRef<uint64_t>(getMemberRef(windowInfoRef, &WindowInfo::windowEnd))};
    // Frame identity and encoding
    const auto outFrame = readValueFromMemRef<uint32_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::outFrame));
    const auto flags = readValueFromMemRef<uint8_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::flags));
    const auto bands = readValueFromMemRef<uint8_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::bands));
    const auto format = readValueFromMemRef<uint8_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::format));

    // Native call: reconstruct, kernel, encode; config passed as args
    OwnedNautilusBuffer outputPayloadBuffer;
    const nautilus::val<uint64_t> payloadSize = nautilus::invoke(
        +[](TupleBuffer* parent, AbstractBufferProvider* bufferProvider, TupleBuffer* outPayload,
            int32_t outX, int32_t outY, uint64_t presenceMask,
            uint32_t outFrame, uint8_t flags, uint8_t bands, uint8_t format, // frame identity and encoding
            int64_t rasterW, int64_t rasterH, int32_t tileW, int32_t tileH,   // geometry
            int32_t reachH, int32_t reachV,                                   // tileReach
            int32_t radius, uint8_t kappa, uint8_t beta, uint8_t disc,        // focalSpec
            double noData) -> uint64_t
        {
            const Raster::RasterGeometry geo{rasterW, rasterH, tileW, tileH};
            const Raster::TileReach reach{reachH, reachV};
            Raster::FocalSpec spec{
                disc ? Raster::makeDisc(radius) : Raster::makeDenseSquare(radius),
                static_cast<Raster::ReductionOp>(kappa),
                static_cast<Raster::BoundaryRule>(beta),
                focalSummation()};

            // Tile gather, same order as emitOutput; keyed by absolute tileIndex
            std::unordered_map<size_t, Raster::GridView> tiles;
            std::vector<TupleBuffer> held;                       // keep child buffers alive for the kernel
            uint32_t neighbourIndex = 0, childCursor = 0;
            Raster::forEachNeighbourTile(geo, outX, outY, reach, [&](int32_t tx, int32_t ty)
            {

                if (presenceMask & (uint64_t{1} << neighbourIndex))
                {
                    INVARIANT(childCursor < parent->getNumberOfChildBuffers(),"presenceMask/child-buffer count mismatch at output ({},{})", outX, outY);
                    TupleBuffer child = parent->loadChildBuffer(VariableSizedAccess::Index{childCursor++});
                    tiles.emplace(Raster::tileIndex(geo, {tx, ty}),
                                  Raster::GridView{child.getAvailableMemoryArea<double>().data(),
                                                   static_cast<uint32_t>(tileH), static_cast<uint32_t>(tileW), noData, bands});
                    held.push_back(std::move(child));
                }
                ++neighbourIndex;
            });
            const Raster::FocalContext ctx{
                geo, outX, outY,
                [&tiles, &geo](int32_t tx, int32_t ty) -> const Raster::GridView*
                {
                    const auto it = tiles.find(Raster::tileIndex(geo, {tx, ty}));
                    return it == tiles.end() ? nullptr : &it->second;
                },
                noData
            };

            // All bands, band-major, into scratch
            const uint64_t cellCount = static_cast<uint64_t>(bands) * static_cast<uint64_t>(tileW) * static_cast<uint64_t>(tileH);
            std::vector<double> outCells(cellCount);
            const auto status = Raster::evaluateTile(spec, ctx, outCells.data(), cellCount, bands);
            INVARIANT(status == Raster::FocalStatus::Ok, "focal evaluate failed: status {}", static_cast<int32_t>(status));

            // Encode like the input: text, or binary maplet with placement
            const Raster::GridView outGrid{outCells.data(), static_cast<uint32_t>(tileH), static_cast<uint32_t>(tileW), noData, bands};
            const auto wireFormat = static_cast<Raster::WireFormat>(format);
            const Raster::TileSpec outSpec{
                wireFormat, Raster::CellType::Float64,
                static_cast<uint32_t>(tileW), static_cast<uint32_t>(tileH), noData, bands,
                static_cast<uint32_t>(rasterW), static_cast<uint32_t>(rasterH)};
            const Raster::MapletPlacement placement{outFrame, outX, outY, flags};
            const bool binary = wireFormat == Raster::WireFormat::Binary;

            // Binary has a fixed size; text is variable-length, so over-allocate and trust bytesWritten.
            const uint64_t capacity = binary ? Raster::encodedSizeBinary(outSpec) : cellCount * 32 + 1;
            auto outBuf = bufferProvider->getUnpooledBuffer(capacity);
            if (not outBuf.has_value()) throw CannotAllocateBuffer("{}B for encoded focal output tile", capacity);

            uint64_t bytesWritten = 0;
            const auto codecStatus = Raster::encode(
                outGrid, outSpec, binary ? &placement : nullptr, outBuf->getAvailableMemoryArea<int8_t>().data(), capacity, bytesWritten);
            INVARIANT(codecStatus == Raster::CodecStatus::Ok, "focal encode failed: status {}", static_cast<int32_t>(codecStatus));

            *outPayload = outBuf.value();
            return bytesWritten;
        },
        recordBuffer.getReference(),
        executionCtx.pipelineMemoryProvider.bufferProvider,
        outputPayloadBuffer.asArg(),
        outX, outY, presenceMask,
        outFrame, flags, bands, format,
        nautilus::val<int64_t>{geometry.rasterW}, nautilus::val<int64_t>{geometry.rasterH},
        nautilus::val<int32_t>{geometry.tileW},   nautilus::val<int32_t>{geometry.tileH},
        nautilus::val<int32_t>{tileReach.horizontal}, nautilus::val<int32_t>{tileReach.vertical},
        nautilus::val<int32_t>{focalSpec.n.radius},
        nautilus::val<uint8_t>{static_cast<uint8_t>(focalSpec.kappa)},
        nautilus::val<uint8_t>{static_cast<uint8_t>(focalSpec.beta)},
        nautilus::val<uint8_t>{static_cast<uint8_t>(Raster::denseSquareRadius(focalSpec.n) < 0)},
        nautilus::val<double>{noData});

    // Emit downstream
    Record outputRecord;
    outputRecord.write(outputTileXField, outX);
    outputRecord.write(outputTileYField, outY);
    const VariableSizedData payload{outputPayloadBuffer.data(), payloadSize};
    outputRecord.write(outputPayloadField, payload);
    outputRecord.write(windowMetaData.startField.getFullyQualifiedName(), windowStart.convertToValue());
    outputRecord.write(windowMetaData.endField.getFullyQualifiedName(),   windowEnd.convertToValue());
    if (outputTsField)
    {
        outputRecord.write(*outputTsField, readValueFromMemRef<uint64_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::eventTs)));
    }
    if (outputSrcIdField && outputSrcFrameIdField)
    {
        outputRecord.write(*outputSrcIdField, readValueFromMemRef<uint32_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::srcId)));
        outputRecord.write(*outputSrcFrameIdField, readValueFromMemRef<uint32_t>(getMemberRef(descriptorRef, &EmittedFocalOutput::srcFrameId)));
    }
    executeChild(executionCtx, outputRecord);
}

}