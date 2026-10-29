#pragma once

#include <Raster/Focal/FocalFrameState.hpp>
#include <Raster/DecodedTile.hpp>
#include <Raster/RasterSlice.hpp>

namespace NES::Focal
{

/// The focal operator's slice: a RasterSlice whose frame table holds FocalFrameStates storing DecodedTiles.
using FocalFrameStateT = FocalFrameState<DecodedTile>;
using FocalSlice = RasterSlice<FocalFrameStateT>;

}

/// Compiled once in FocalSlice.cpp instead of in every file that includes this header.
extern template class NES::Focal::FocalFrameState<NES::DecodedTile>;
extern template class NES::RasterSlice<NES::Focal::FocalFrameState<NES::DecodedTile>>;