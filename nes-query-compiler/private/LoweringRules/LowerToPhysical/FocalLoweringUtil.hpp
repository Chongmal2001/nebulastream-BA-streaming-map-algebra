#pragma once

#include <cstdint>
#include <utility>
#include <Operators/Windows/WindowedFocalFrameLogicalOperator.hpp>
#include <FocalDriver.hpp>
#include <FocalSpec.hpp>
#include <ErrorHandling.hpp>
#include <magic_enum/magic_enum.hpp>

/// Conversions from the logical focal parameters (FOCAL_* arguments) to the raster plugin's types
namespace NES
{

inline Raster::RasterGeometry toRasterGeometry(const FocalParameters& params)
{
    return Raster::RasterGeometry{
        params.rasterWidth,
        params.rasterHeight,
        params.tileWidth,
        params.tileHeight,
    };
}

inline Raster::ReductionOp toReductionOp(const FocalReductionOp op)
{
    switch (op)
    {
        case FocalReductionOp::Sum:  return Raster::ReductionOp::Sum;
        case FocalReductionOp::Mean: return Raster::ReductionOp::Mean;
        case FocalReductionOp::Max:  return Raster::ReductionOp::Max;
        case FocalReductionOp::Min:  return Raster::ReductionOp::Min;
            throw NotImplemented("FOCAL reduction '{}' is not yet implemented in the raster plugin",
                                 magic_enum::enum_name(op));
    }
    std::unreachable();
}

inline Raster::BoundaryRule toBoundaryRule(const FocalBoundaryRule rule)
{
    switch (rule)
    {
        case FocalBoundaryRule::NoData:  return Raster::BoundaryRule::NoData;
        case FocalBoundaryRule::Clamp:   return Raster::BoundaryRule::Clamp;
        case FocalBoundaryRule::Reflect: return Raster::BoundaryRule::Reflect;
    }
    std::unreachable();
}

inline Raster::Neighbourhood toNeighbourhood(const FocalNeighbourhoodShape shape, const int32_t radius)
{
    switch (shape)
    {
        case FocalNeighbourhoodShape::Square: return Raster::makeDenseSquare(radius);
        case FocalNeighbourhoodShape::Disc:   return Raster::makeDisc(radius);
    }
    std::unreachable();
}

inline Raster::FocalSpec toFocalSpec(const FocalParameters& params)
{
    return Raster::FocalSpec{
        toNeighbourhood(params.neighbourhood, params.radius),
        toReductionOp(params.reduction),
        toBoundaryRule(params.boundary)};
}

}
