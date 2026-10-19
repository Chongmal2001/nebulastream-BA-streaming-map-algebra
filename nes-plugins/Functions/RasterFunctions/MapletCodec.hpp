#pragma once

#include <cstdint>
#include <bit>


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

#pragma once

namespace Raster
{
static_assert(std::endian::native == std::endian::little, "MapletCodec assumes little-endian host; binary path stores native float64");

constexpr uint64_t BINARY_HEADER_SIZE = 24;
constexpr uint64_t MAX_TEXT_CHARS_PER_CELL = 26; // to_chars(double) shortes <= 24


enum class WireFormat: uint8_t { Text = 1, Binary = 2 };
enum class CellType: uint8_t { Float64 = 1}; // only float64 supported for now

// static, per-stream tile metadata
struct TileSpec
{
    WireFormat format = WireFormat::Text;
    CellType cellType = CellType::Float64;
    uint32_t width = 0;
    uint32_t height = 0;
    double noData = 0.0;
    [[nodiscard]] uint64_t cellCount() const { return static_cast<uint64_t>(height) * static_cast<uint64_t>(width); }
};

/// Non-owning view over decoded cells
struct GridView
{
    const double* cells = nullptr;
    uint32_t h = 0;
    uint32_t w = 0;
    double noData = 0.0;

    [[nodiscard]] uint64_t cellCount() const { return static_cast<uint64_t>(h) * static_cast<uint64_t>(w); }

    [[nodiscard]] double at(uint32_t r, uint32_t c) const { return cells[(static_cast<uint64_t>(r) * w) + c]; }

    /// NaN counts as NoData: NaN != NaN, so the self-comparison catches it.
    [[nodiscard]] bool isNoData(double v) const { return v == noData || v != v; }

    [[nodiscard]] bool inBounds(int64_t r, int64_t c) const
    {
        return r >= 0 && c >= 0 && r < static_cast<int64_t>(h) && c < static_cast<int64_t>(w);
    }
};

/// Negative on failure so a single int64_t return can carry either a byte count
/// or an error. Exceptions cannot cross a nautilus::invoke boundary.
enum class CodecStatus : int32_t
{
    Ok = 0,
    TruncatedInput = -1,
    UnsupportedVersion = -2,
    UnsupportedCellType = -3,
    SizeMismatch = -4,
    ScratchTooSmall = -5,
    OutputTooSmall = -6,
    MalformedNumber = -7,
    BadDimensions = -8,
};

/// Decodes a blob into caller-owned scratch, returns a view over it.
/// Cross-checks its preamble against spec and returns SizeMismatch on disagreement.
CodecStatus decode(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells, GridView& out) noexcept;

/// Writes cells into out, format taken from spec
CodecStatus encode(const GridView& grid, const TileSpec& spec, int8_t* out, uint64_t capacity, uint64_t& bytesWritten) noexcept;

uint64_t encodedSizeBinary(const TileSpec&) noexcept;
uint64_t encodedSizeTextUpperBound(const TileSpec&) noexcept;

}