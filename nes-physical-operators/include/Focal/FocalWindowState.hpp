#pragma once

#include <cstdint>
#include <vector>
#include <SliceStore/Slice.hpp>
#include <FocalSpec.hpp>


namespace NES
{

template <typename Policy>
class FocalWindowState
{

    using TilePosition = typename Raster::TilePosition;
    using Flag = typename Policy::Flag;
    using Counter = typename Policy::Counter;

public:
    static constexpr uint32_t ALREADY_EMITTED = 0xFFFFFFFFu;

    explicit FocalWindowState(const Raster::RasterGeometry& geometry);

    /// Manage tile presence, dependency counters and emission state
    void initMissing(TilePosition pos, uint32_t count);
    void initReaders(TilePosition pos, uint32_t count);

    void markPresent(TilePosition pos);
    [[nodiscard]] bool isPresent(TilePosition pos) const;

    /// Atomically records pos present for this window and returns whether it was already present
    [[nodiscard]] bool testAndMarkPresent(TilePosition pos);

    [[nodiscard]] bool decrementMissing(TilePosition pos);
    [[nodiscard]] bool decrementReaders(TilePosition pos);

    void markEmitted(TilePosition pos);
    [[nodiscard]] bool isEmitted(TilePosition pos);

    /// Populates the WindowStates's missing/readers counters for each grid position with its neighbour tile count
    void initializeCounters(Raster::TileReach tileReach);

    /// Iterates over positions where missing > 0
    /// used by FocalOperatorHandler.triggerSlices to force emit outputs whose neighbourhoods never completed
    template <typename Fn>
    void forEachIncompleteOutput(Fn&& fn) const
    {
        for (int32_t y = 0; y < geo.tilesY(); ++y)
            for (int32_t x = 0; x < geo.tilesX(); ++x)
                if (Policy::loadCounter(missing[tileIndex(geo, {x, y})]) > 0)
                    fn(x, y);
    }

private:

    Raster::RasterGeometry geo;
    std::vector<Flag> present;     // input tile presence bitmap
    std::vector<Counter> missing;  // missing[o] = how many input tiles that output o needs are still absent?
    std::vector<Counter> readers;  // readers[i] = how many output tiles still need to read input i
};


}
