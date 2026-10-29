#include <Raster/Focal/FocalSlice.hpp>
 
/// Explicit instantiations: the focal slice and its frame state are compiled here, once.
template class NES::Focal::FocalFrameState<NES::DecodedTile>;
template class NES::RasterSlice<NES::Focal::FocalFrameState<NES::DecodedTile>>;