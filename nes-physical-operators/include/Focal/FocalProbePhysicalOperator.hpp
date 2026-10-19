#pragma once

#include <cstdint>
#include <Interface/Record.hpp>
#include <Interface/RecordBuffer.hpp>
#include <WindowProbePhysicalOperator.hpp>
#include <Operators/Windows/WindowMetaData.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>

namespace NES
{

class FocalProbePhysicalOperator final : public WindowProbePhysicalOperator
{
public:
    FocalProbePhysicalOperator(
        OperatorHandlerId operatorHandlerId,
        WindowMetaData windowMetaData,
        Raster::RasterGeometry geometry,
        Raster::FocalSpec focalSpec,
        Raster::TileSpec tileSpec,
        double noData,
        Record::RecordFieldIdentifier outputTileXField,
        Record::RecordFieldIdentifier outputTileYField,
        Record::RecordFieldIdentifier outputPayloadField);

    void open(ExecutionContext& executionCtx, RecordBuffer& recordBuffer) const override;

private:
    // TODO: cleanup => generalize into FocalConfig instead of carrying members individually
    Raster::RasterGeometry geometry;
    Raster::FocalSpec focalSpec; // kernel params
    Raster::TileReach tileReach; // derived from geometry and focalSpec
    Raster::TileSpec tileSpec; // for encoding the output tile payload
    double noData;

    // output schema field identifiers that the probe writes into
    Record::RecordFieldIdentifier outputTileXField;
    Record::RecordFieldIdentifier outputTileYField;
    Record::RecordFieldIdentifier outputPayloadField;
};


}
