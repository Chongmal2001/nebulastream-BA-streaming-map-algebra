#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <string_view>

/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at
        https://www.apache.org/licenses/LICENSE-2.0
    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/// Maplet codec; byte layout in maplet-format.md 5.2 (text = 1, binary = 3, 2 retired)
namespace Raster
{
static_assert(std::endian::native == std::endian::little, "MapletCodec assumes little-endian host; binary path stores native float64");

constexpr uint64_t BINARY_HEADER_SIZE = 32;
constexpr uint64_t MAX_TEXT_CHARS_PER_CELL = 26;     // to_chars(double) shortest <= 24
constexpr uint32_t MAX_BANDS = 255;

/// Byte offsets inside the binary preamble that operators patch in place.
constexpr uint64_t OFFSET_FLAGS = 3;
constexpr uint64_t OFFSET_FRAME_ID = 4;

/// Maplet flags (byte 3).
constexpr uint8_t FLAG_FILLED = 0x01;                // NoData tile synthesised by an operator, not observed data

enum class WireFormat : uint8_t { Text = 1, Binary = 3 };
enum class CellType : uint8_t { Float64 = 1 };       // only float64 supported for now

/// Per-stream tile metadata; new members go last (aggregate init)
struct TileSpec
{
    WireFormat format = WireFormat::Text;
    CellType cellType = CellType::Float64;
    uint32_t width = 0;                                // tile width
    uint32_t height = 0;                               // tile height
    double noData = 0.0;                               // never stored in a maplet
    uint8_t bands = 1;
    uint32_t rasterWidth = 0;                          // binary only, 0 = unknown
    uint32_t rasterHeight = 0;

    [[nodiscard]] uint64_t planeCells() const { return static_cast<uint64_t>(height) * static_cast<uint64_t>(width); }
    /// All cells of all bands. Plane-level loops must use planeCells().
    [[nodiscard]] uint64_t cellCount() const { return static_cast<uint64_t>(bands) * planeCells(); }
    [[nodiscard]] uint32_t tilesX() const { return width == 0 ? 0 : (rasterWidth + width - 1) / width; }
    [[nodiscard]] uint32_t tilesY() const { return height == 0 ? 0 : (rasterHeight + height - 1) / height; }
};

/// Non-owning view over decoded cells, band-major.
struct GridView
{
    const double* cells = nullptr;
    uint32_t h = 0;
    uint32_t w = 0;
    double noData = 0.0;
    uint8_t bands = 1;

    [[nodiscard]] uint64_t planeCells() const { return static_cast<uint64_t>(h) * static_cast<uint64_t>(w); }
    [[nodiscard]] uint64_t cellCount() const { return static_cast<uint64_t>(bands) * planeCells(); }

    /// Band 0.
    [[nodiscard]] double at(uint32_t r, uint32_t c) const { return cells[(static_cast<uint64_t>(r) * w) + c]; }
    [[nodiscard]] double at(uint32_t b, uint32_t r, uint32_t c) const
    {
        return cells[(b * planeCells()) + (static_cast<uint64_t>(r) * w) + c];
    }
    /// Single-band view of band b, so per-band kernels stay unchanged.
    [[nodiscard]] GridView band(uint32_t b) const { return GridView{cells + (b * planeCells()), h, w, noData, 1}; }

    /// NaN counts as NoData: NaN != NaN, so the self-comparison catches it.
    [[nodiscard]] bool isNoData(double v) const { return v == noData || v != v; }

    [[nodiscard]] bool inBounds(int64_t r, int64_t c) const
    {
        return r >= 0 && c >= 0 && r < static_cast<int64_t>(h) && c < static_cast<int64_t>(w);
    }
};

/// The binary preamble, parsed.
struct MapletHeader
{
    uint8_t bands = 0;
    uint8_t flags = 0;
    uint32_t frameId = 0;
    int32_t tileX = 0;
    int32_t tileY = 0;
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t rasterWidth = 0;
    uint32_t rasterHeight = 0;

    [[nodiscard]] uint64_t planeCells() const { return static_cast<uint64_t>(height) * static_cast<uint64_t>(width); }
    [[nodiscard]] uint64_t cellCount() const { return static_cast<uint64_t>(bands) * planeCells(); }
    [[nodiscard]] uint64_t totalSize() const { return BINARY_HEADER_SIZE + (cellCount() * sizeof(double)); }
    /// Rows / columns of this tile inside the raster extent; the rest is padding.
    [[nodiscard]] uint32_t validRows() const;
    [[nodiscard]] uint32_t validCols() const;
};

/// Frame header of a frame file, 112 bytes (maplet-format.md 5.1)
constexpr uint64_t FRAME_HEADER_SIZE = 112;
constexpr std::array<char, 8> FRAME_MAGIC = {'M', 'P', 'L', 'F', 'R', 'M', '0', '4'};

struct FrameHeader
{
    uint32_t frameId = 0;
    uint32_t nTiles = 0;
    uint64_t eventTimeMs = 0;
    uint32_t rasterWidth = 0;
    uint32_t rasterHeight = 0;
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    uint8_t bands = 0;
    uint16_t flags = 0;
    double noData = 0.0;
    uint32_t crsEpsg = 0;
    std::array<double, 6> transform{};

    [[nodiscard]] uint32_t tilesX() const { return (rasterWidth + tileWidth - 1) / tileWidth; }
    [[nodiscard]] uint32_t tilesY() const { return (rasterHeight + tileHeight - 1) / tileHeight; }
    /// Size of every maplet of the frame
    [[nodiscard]] uint64_t mapletSize() const
    {
        return BINARY_HEADER_SIZE + (static_cast<uint64_t>(bands) * tileHeight * tileWidth * sizeof(double));
    }
    /// Header plus all maplets
    [[nodiscard]] uint64_t fileSize() const { return FRAME_HEADER_SIZE + (static_cast<uint64_t>(nTiles) * mapletSize()); }
};

/// Where an encoded binary maplet belongs. Ignored by the text format.
struct MapletPlacement
{
    uint32_t frameId = 0;
    int32_t tileX = 0;
    int32_t tileY = 0;
    uint8_t flags = 0;
};

/// Negative on failure, so one int64_t carries bytes or error (no exceptions across invoke)
enum class CodecStatus : int32_t
{
    Ok = 0,
    TruncatedInput = -1,
    UnsupportedVersion = -2,
    UnsupportedCellType = -3,
    SizeMismatch = -4,          // shape or byte length differs from the spec
    ScratchTooSmall = -5,
    OutputTooSmall = -6,
    MalformedNumber = -7,
    BadDimensions = -8,
    BandOutOfRange = -9,        // band index / slot out of range, or bad k
    PositionOutOfRange = -10,   // tile outside the grid
    MissingPlacement = -11,     // binary encode without a MapletPlacement
};

/// Name of a status for error messages, e.g. "TruncatedInput".
std::string_view toString(CodecStatus status) noexcept;

/// Preamble only: format, cell type, shape, size, grid position
CodecStatus peekHeader(const int8_t* in, uint64_t inSize, MapletHeader& out) noexcept;

/// Frame header: magic, cell type, non-zero sizes; nTiles unchecked
CodecStatus peekFrameHeader(const int8_t* in, uint64_t inSize, FrameHeader& out) noexcept;

/// Decodes into caller scratch; binary shape must match spec, hdrOut gets the preamble
CodecStatus decode(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells, GridView& out,
                   MapletHeader* hdrOut = nullptr) noexcept;

/// Encodes in spec.format; binary needs placement and raster size
CodecStatus encode(const GridView& grid, const TileSpec& spec, const MapletPlacement* placement, int8_t* out, uint64_t capacity,
                   uint64_t& bytesWritten) noexcept;

/// Copies bands idx[0..k) of a binary maplet; preamble kept except the band count
CodecStatus selectBands(const int8_t* in, uint64_t inSize, const uint8_t* idx, uint32_t k, int8_t* out, uint64_t capacity,
                        uint64_t& bytesWritten) noexcept;
uint64_t selectedSize(const MapletHeader& hdr, uint32_t k) noexcept;

/// Band selection one band per call (BANDS); same result as selectBands
CodecStatus beginBandSelection(const int8_t* in, uint64_t inSize, uint32_t k, int8_t* out, uint64_t capacity) noexcept;
CodecStatus copyBand(const int8_t* in, uint64_t inSize, uint32_t band, int8_t* out, uint64_t outSize, uint32_t slot) noexcept;

/// Patches frameId / flags of a binary maplet in place
CodecStatus setFrameId(int8_t* maplet, uint64_t size, uint32_t frameId) noexcept;
CodecStatus setFlags(int8_t* maplet, uint64_t size, uint8_t flags) noexcept;

uint64_t encodedSizeBinary(const TileSpec& spec) noexcept;     // 32 + 8 * bands * height * width
uint64_t encodedSizeTextUpperBound(const TileSpec& spec) noexcept;

}