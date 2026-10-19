#include <Focal/FocalSlice.hpp>
#include <ErrorHandling.hpp>
#include <algorithm>
#include <utility>

#include <Focal/FocalPolicy.hpp>
#include <boost/asio/detail/mutex.hpp>


namespace NES
{
using Raster::tileIndex;

template <typename Policy>
FocalSlice<Policy>::FocalSlice(SliceStart start, SliceEnd end, const Raster::RasterGeometry& geometry, double noData)
    : Slice(start, end)
    , noData(noData)
    , geo(geometry)
{
}

template <typename Policy>
void FocalSlice<Policy>::setWindowRefCount(TilePosition pos, uint32_t windowCount)
{
    std::unique_lock lock(tileMutex);
    auto &storedTile = store.at(tileIndex(geo, pos));
    Policy::storeCounter(storedTile->windowRefCount, windowCount);

}

template <typename Policy>
bool FocalSlice<Policy>::decrementWindowRefCount(TilePosition pos)
{
    std::unique_lock lock(tileMutex);
    auto& windowRefCount = store.at(tileIndex(geo, pos))->windowRefCount;
    INVARIANT(Policy::loadCounter(windowRefCount) > 0, "windowRefCount underflow at ({},{})", pos.x, pos.y);
    return Policy::decrementAndIsZero(windowRefCount);

}

template <typename Policy>
void FocalSlice<Policy>::releaseWindowAndEvict()
{
    std::unique_lock lock(tileMutex);
    for (auto it = store.begin(); it != store.end(); )
    {
        if (Policy::decrementAndIsZero(it->second->windowRefCount))
            it = store.erase(it);
        else
            ++it;
    }

}

template <typename Policy>
void FocalSlice<Policy>::storeTile(TilePosition pos, TupleBuffer cells, uint32_t windowCount)
{

    auto tile = std::make_unique<StoredTile>();
    tile->cells = std::move(cells);
    tile->view = Raster::GridView{
        tile->cells.template getAvailableMemoryArea<double>().data(),
        static_cast<uint32_t>(geo.tileH),
        static_cast<uint32_t>(geo.tileW),
        noData,
    };
    Policy::storeCounter(tile->windowRefCount, windowCount);
    std::unique_lock lock(tileMutex);
    store[tileIndex(geo, pos)] = std::move(tile);
}

template <typename Policy>
void FocalSlice<Policy>::evictTile(TilePosition pos)
{
    std::unique_lock lock(tileMutex);
    store.erase(tileIndex(geo, pos));   // frees cells; its view dies with it
}


template class FocalSlice<SingleThreadedPolicy>;
template class FocalSlice<MultiThreadedPolicy>;

}