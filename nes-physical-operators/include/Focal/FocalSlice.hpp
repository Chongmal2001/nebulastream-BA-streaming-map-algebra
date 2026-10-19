#pragma once

#include <cstdint>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <memory>
#include <SliceStore/Slice.hpp>
#include <FocalSpec.hpp>
#include <MapletCodec.hpp>
#include <Runtime/TupleBuffer.hpp>

namespace NES
{

template <typename Policy>
class FocalSlice final : public Slice
{
using TilePosition = Raster::TilePosition;

public:

    FocalSlice(SliceStart start, SliceEnd end, const Raster::RasterGeometry& geometry, double noData);

    void setWindowRefCount(TilePosition pos, uint32_t windowCount);
    [[nodiscard]] bool decrementWindowRefCount(TilePosition pos); // returns true when a tile's windowRefCount hits zero

    void storeTile(TilePosition pos, TupleBuffer cells, uint32_t windowCount);
    void evictTile(TilePosition pos);

    /// Returns a read cursor over the tile at pos if this slice holds it, else nullptr
    [[nodiscard]] const Raster::GridView* tryGetView(TilePosition pos) const
    {
        auto it = store.find(tileIndex(geo, pos));
        return it == store.end() ? nullptr : &it->second->view;
    }

    /// Retained copy of the tile's buffer if this slice holds it
    [[nodiscard]] std::optional<TupleBuffer> tryGetBuffer(TilePosition pos) const
    {
        std::shared_lock lock(tileMutex);
        auto it = store.find(tileIndex(geo, pos));
        return it == store.end() ? std::nullopt : std::optional<TupleBuffer>{it->second->cells};
    }

    /// Frees memory after one window is done with all its tiles
    void releaseWindowAndEvict();

    [[nodiscard]] const Raster::RasterGeometry& geometry() const { return geo; }

private:

    double noData;
    Raster::RasterGeometry geo;

    struct StoredTile
    {
        TupleBuffer cells;
        Raster::GridView view;
        Policy::Counter windowRefCount{0};
    };
    std::unordered_map<size_t, std::unique_ptr<StoredTile>> store;   // sparse => only present, unevicted tiles
    mutable Policy::Mutex tileMutex; // guards store
};
}
