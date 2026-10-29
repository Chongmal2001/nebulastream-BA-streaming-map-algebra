#include <TileStoreReader.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <span>
#include <utility>

#include <ErrorHandling.hpp>
#include <MapletCodec.hpp>

namespace NES
{
namespace
{
/// Raster facts every frame shares with the anchor; NaN NoData equals NaN
bool sameRaster(const Raster::FrameHeader& lhs, const Raster::FrameHeader& rhs)
{
    const bool sameNoData = lhs.noData == rhs.noData || (std::isnan(lhs.noData) && std::isnan(rhs.noData));
    return lhs.rasterWidth == rhs.rasterWidth && lhs.rasterHeight == rhs.rasterHeight && lhs.tileWidth == rhs.tileWidth
        && lhs.tileHeight == rhs.tileHeight && lhs.bands == rhs.bands && sameNoData && lhs.crsEpsg == rhs.crsEpsg
        && lhs.transform == rhs.transform;
}

Raster::FrameHeader readFrameHeader(std::ifstream& file, const std::filesystem::path& path)
{
    std::array<int8_t, Raster::FRAME_HEADER_SIZE> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Raster::FrameHeader header;
    const auto status = file.gcount() == static_cast<std::streamsize>(bytes.size())
        ? Raster::peekFrameHeader(bytes.data(), bytes.size(), header)
        : Raster::CodecStatus::TruncatedInput;
    if (status != Raster::CodecStatus::Ok)
    {
        throw CannotFormatSourceData("RasterFile: invalid frame header in {}: {}", path.string(), Raster::toString(status));
    }
    return header;
}
}

TileStoreReader::TileStoreReader(std::filesystem::path store) : store(std::move(store))
{
    const auto framesDir = this->store / "frames";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(framesDir, error))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".mpl")
        {
            frameFiles.push_back(entry.path());
        }
    }
    if (error)
    {
        throw CannotOpenSource("RasterFile: cannot list {}: {}", framesDir.string(), error.message());
    }
    if (frameFiles.empty())
    {
        throw CannotOpenSource("RasterFile: no frame files (*.mpl) in {}", framesDir.string());
    }
    std::ranges::sort(frameFiles);

    /// First frame header is the anchor
    std::ifstream first(frameFiles.front(), std::ios::binary);
    if (not first)
    {
        throw CannotOpenSource("RasterFile: cannot open {}", frameFiles.front().string());
    }
    anchorHeader = readFrameHeader(first, frameFiles.front());
}

bool TileStoreReader::atEnd()
{
    while (remainingTiles == 0)
    {
        if (not openNextFrame())
        {
            return true;
        }
    }
    return false;
}

bool TileStoreReader::openNextFrame()
{
    if (nextFrame == frameFiles.size())
    {
        currentFile.close();
        return false;
    }
    const auto& path = frameFiles[nextFrame++];
    currentFile = std::ifstream(path, std::ios::binary);
    if (not currentFile)
    {
        throw CannotFormatSourceData("RasterFile: cannot open {}", path.string());
    }
    currentHeader = readFrameHeader(currentFile, path);

    if (not sameRaster(currentHeader, anchorHeader))
    {
        throw CannotFormatSourceData("RasterFile: {} describes a different raster than {}", path.string(), frameFiles.front().string());
    }
    if (lastEventTime.has_value() && currentHeader.eventTimeMs <= *lastEventTime)
    {
        throw CannotFormatSourceData(
            "RasterFile: event time {} of {} does not increase (previous frame: {})", currentHeader.eventTimeMs, path.string(), *lastEventTime);
    }
    const uint64_t tiles = static_cast<uint64_t>(currentHeader.tilesX()) * currentHeader.tilesY();
    if (currentHeader.nTiles > tiles)
    {
        throw CannotFormatSourceData("RasterFile: {} has {} tiles, the grid only {}", path.string(), currentHeader.nTiles, tiles);
    }
    if (std::error_code error; std::filesystem::file_size(path, error) != currentHeader.fileSize() || error)
    {
        throw CannotFormatSourceData(
            "RasterFile: {} is not {} bytes (header + {} maplets of {} bytes)",
            path.string(),
            currentHeader.fileSize(),
            currentHeader.nTiles,
            currentHeader.mapletSize());
    }

    lastEventTime = currentHeader.eventTimeMs;
    remainingTiles = currentHeader.nTiles;
    seenPositions.assign(tiles, false);
    ++engineFrameId;
    return true;
}

TileStoreReader::MapletInfo TileStoreReader::next(std::span<int8_t> out)
{
    PRECONDITION(remainingTiles > 0, "TileStoreReader::next called at the end of the store");
    PRECONDITION(out.size() == mapletSize(), "Maplet buffer of {} bytes, expected {}", out.size(), mapletSize());
    const auto& path = frameFiles[nextFrame - 1];

    currentFile.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (currentFile.gcount() != static_cast<std::streamsize>(out.size()))
    {
        throw CannotFormatSourceData("RasterFile: {} ends inside a maplet", path.string());
    }
    --remainingTiles;

    /// Format, cell type, length, grid position
    Raster::MapletHeader maplet;
    if (const auto status = Raster::peekHeader(out.data(), out.size(), maplet); status != Raster::CodecStatus::Ok)
    {
        throw CannotFormatSourceData("RasterFile: invalid maplet in {}: {}", path.string(), Raster::toString(status));
    }
    if (maplet.bands != currentHeader.bands || maplet.height != currentHeader.tileHeight || maplet.width != currentHeader.tileWidth
        || maplet.rasterWidth != currentHeader.rasterWidth || maplet.rasterHeight != currentHeader.rasterHeight)
    {
        throw CannotFormatSourceData(
            "RasterFile: maplet ({}, {}) in {} does not match the frame header", maplet.tileX, maplet.tileY, path.string());
    }
    const auto position = (static_cast<uint64_t>(maplet.tileY) * currentHeader.tilesX()) + static_cast<uint64_t>(maplet.tileX);
    if (seenPositions[position])
    {
        throw CannotFormatSourceData("RasterFile: tile ({}, {}) appears twice in {}", maplet.tileX, maplet.tileY, path.string());
    }
    seenPositions[position] = true;

    Raster::setFrameId(out.data(), out.size(), engineFrameId);
    return MapletInfo{currentHeader.eventTimeMs, currentHeader.frameId};
}

}
