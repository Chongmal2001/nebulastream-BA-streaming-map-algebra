#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

#include <MapletCodec.hpp>

namespace NES
{

/// Reads tile store frames lazily, maplet by maplet; first frame header is the anchor
class TileStoreReader
{
public:
    /// Row fields besides the maplet
    struct MapletInfo
    {
        uint64_t eventTimeMs;  // frame header event time
        uint32_t srcFrameId;   // frame header frame_id (lineage)
    };

    /// Lists the frame files, reads the anchor
    explicit TileStoreReader(std::filesystem::path store);

    [[nodiscard]] const Raster::FrameHeader& anchor() const { return anchorHeader; }
    /// Size of every maplet of the store
    [[nodiscard]] uint64_t mapletSize() const { return anchorHeader.mapletSize(); }

    /// No maplet left; opens the next frame when needed
    [[nodiscard]] bool atEnd();

    /// Reads, checks and renumbers the next maplet; requires !atEnd()
    MapletInfo next(std::span<int8_t> out);

private:
    std::filesystem::path store;
    std::vector<std::filesystem::path> frameFiles;
    size_t nextFrame = 0;

    Raster::FrameHeader anchorHeader;
    Raster::FrameHeader currentHeader;
    std::ifstream currentFile;
    uint32_t remainingTiles = 0;
    std::vector<bool> seenPositions;  // per tile of the grid, for the current frame
    uint32_t engineFrameId = 0;       // of the current frame
    std::optional<uint64_t> lastEventTime;

    /// Opens and checks the next frame; false if none
    bool openNextFrame();
};

}
