#include <Focal/FocalWindowState.hpp>
#include <Focal/FocalPolicy.hpp>
#include <utility>

namespace NES
{

template <typename Policy>
FocalWindowState<Policy>::FocalWindowState(const Raster::RasterGeometry& geometry)
    : geo(geometry)
    , present(static_cast<size_t>(geometry.tilesX()) * geometry.tilesY())   // zero init
    , missing(static_cast<size_t>(geometry.tilesX()) * geometry.tilesY())   // filled by handler via initMissing
    , readers(static_cast<size_t>(geometry.tilesX()) * geometry.tilesY())   // filled by handler via initReaders
{
}

using Raster::tileIndex;

template <typename Policy>
void FocalWindowState<Policy>::initMissing(TilePosition pos, uint32_t count)
{
    Policy::storeCounter(missing[tileIndex(geo, pos)], count);
}

template <typename Policy>
void FocalWindowState<Policy>::initReaders(TilePosition pos, uint32_t count)
{
    Policy::storeCounter(readers[tileIndex(geo, pos)], count);
}

template <typename Policy>
void FocalWindowState<Policy>::markPresent(TilePosition pos)
{
    Policy::markBit(present[tileIndex(geo, pos)]);
}

template <typename Policy>
bool FocalWindowState<Policy>::isPresent(TilePosition pos) const
{
    return Policy::testBit(present[tileIndex(geo, pos)]);
}

template <typename Policy>
bool FocalWindowState<Policy>::testAndMarkPresent(TilePosition pos)
{
    return Policy::testAndSetPresent(present[tileIndex(geo, pos)]);
}

template <typename Policy>
bool FocalWindowState<Policy>::decrementMissing(TilePosition pos)
{
    return Policy::decrementAndIsZero(missing[tileIndex(geo, pos)]);
}

template <typename Policy>
bool FocalWindowState<Policy>::decrementReaders(TilePosition pos)
{
    return Policy::decrementAndIsZero(readers[tileIndex(geo, pos)]);
}

template <typename Policy>
void FocalWindowState<Policy>::markEmitted(TilePosition pos)
{
    Policy::storeCounter(missing[tileIndex(geo, pos)], ALREADY_EMITTED);
}

template <typename Policy>
bool FocalWindowState<Policy>::isEmitted(TilePosition pos)
{
    return Policy::loadCounter(missing[tileIndex(geo, pos)]) == ALREADY_EMITTED;
}

template <typename Policy>
void FocalWindowState<Policy>::initializeCounters(Raster::TileReach tileReach)
{
    for (int32_t y = 0; y < geo.tilesY(); ++y)
        for (int32_t x = 0; x < geo.tilesX(); ++x)
        {
            uint32_t count = 0;
            Raster::forEachNeighbourTile(geo, x, y, tileReach, [&](int32_t, int32_t) { ++count; });
            initMissing({x, y}, count);
            initReaders({x, y}, count);
        }
}
template class FocalWindowState<SingleThreadedPolicy>;
template class FocalWindowState<MultiThreadedPolicy>;
}