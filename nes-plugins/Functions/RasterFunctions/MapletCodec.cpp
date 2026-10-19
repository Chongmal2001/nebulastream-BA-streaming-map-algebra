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

#include <MapletCodec.hpp>

#include <cstdlib>
#include <charconv>
#include <cmath>
#include <limits>
#include <cstring>

namespace Raster
{
namespace
{
constexpr bool isSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

constexpr char toLower(char ch) { return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch; }

/// NaN-aware equality, so a NaN sentinel matches a NaN preamble field.
bool sentinelEqual(double a, double b)
{
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

/// Parse one text token [b, e). Accepts an optional-signed "nan" spelling
/// (case-insensitive) as NaN; everything else goes through from_chars.
bool parseCell(const char* tokenBegin, const char* tokenEnd, double& parsedValue)
{
    const std::ptrdiff_t tokenLength = tokenEnd - tokenBegin;
    if (tokenLength <= 0)
    {
        return false;
    }

    // Explicit NaN handling keeps the NoData intent legible and independent of
    // strtod's own nan() parsing.
    const char* afterSign = tokenBegin;
    if (afterSign < tokenEnd && (*afterSign == '+' || *afterSign == '-'))
    {
        ++afterSign;
    }
    if (tokenEnd - afterSign == 3 && toLower(afterSign[0]) == 'n' && toLower(afterSign[1]) == 'a'
        && toLower(afterSign[2]) == 'n')
    {
        parsedValue = std::numeric_limits<double>::quiet_NaN();
        return true;
    }

    // Copy into a null-terminated buffer so strtod cannot read past tokenEnd.
    // A shortest-round-trip double is at most ~24 characters, so this bound only
    // rejects pathologically long input, which is reported as a malformed cell.
    constexpr std::size_t MAX_TOKEN_CHARS = 63;
    if (static_cast<std::size_t>(tokenLength) > MAX_TOKEN_CHARS)
    {
        return false;
    }
    char nullTerminatedToken[MAX_TOKEN_CHARS + 1];
    std::memcpy(nullTerminatedToken, tokenBegin, static_cast<std::size_t>(tokenLength));
    nullTerminatedToken[tokenLength] = '\0';

    errno = 0;
    char* parseEnd = nullptr;
    const double value = std::strtod(nullTerminatedToken, &parseEnd);

    // Reject unless the entire token was consumed. Only overflow is treated as a
    // range error: underflow to a denormal reports ERANGE too, but returns a
    // finite value we want to keep, so the round-trip of denorm_min still holds.
    if (parseEnd != nullTerminatedToken + tokenLength)
    {
        return false;
    }
    if (errno == ERANGE && std::isinf(value))
    {
        return false;
    }
    parsedValue = value;
    return true;
}

CodecStatus decodeText(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells,
                       GridView& out)
{
    const uint64_t n = spec.cellCount();
    if (scratchCells < n)
    {
        return CodecStatus::ScratchTooSmall;
    }
    const char* p = reinterpret_cast<const char*>(in);
    const char* end = p + inSize;
    for (uint64_t i = 0; i < n; ++i)
    {
        while (p < end && isSpace(*p))
        {
            ++p;
        }
        if (p == end)
        {
            return CodecStatus::TruncatedInput;  // fewer tokens than the grid needs
        }
        const char* tok = p;
        while (p < end && !isSpace(*p))
        {
            ++p;
        }
        if (!parseCell(tok, p, scratch[i]))
        {
            return CodecStatus::MalformedNumber;
        }
    }
    while (p < end)  // only trailing whitespace may follow the last cell
    {
        if (!isSpace(*p))
        {
            return CodecStatus::SizeMismatch;  // more tokens than the grid needs
        }
        ++p;
    }
    out = GridView{scratch, spec.height, spec.width, spec.noData};
    return CodecStatus::Ok;
}

CodecStatus decodeBinary(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells,
                         GridView& out)
{
    if (inSize < BINARY_HEADER_SIZE)
    {
        return CodecStatus::TruncatedInput;
    }
    if (static_cast<uint8_t>(in[0]) != static_cast<uint8_t>(WireFormat::Binary))
    {
        return CodecStatus::UnsupportedVersion;
    }
    if (static_cast<uint8_t>(in[1]) != static_cast<uint8_t>(CellType::Float64))
    {
        return CodecStatus::UnsupportedCellType;
    }
    if (static_cast<uint8_t>(in[2]) != 1)  // single-band focal path
    {
        return CodecStatus::BadDimensions;
    }
    uint32_t h{};
    uint32_t w{};
    double nd{};
    std::memcpy(&h, in + 4, sizeof h);
    std::memcpy(&w, in + 8, sizeof w);
    std::memcpy(&nd, in + 16, sizeof nd);
    if (h != spec.height || w != spec.width)
    {
        return CodecStatus::SizeMismatch;
    }
    if (!sentinelEqual(nd, spec.noData))
    {
        return CodecStatus::SizeMismatch;
    }
    const uint64_t cells = static_cast<uint64_t>(h) * w;
    const uint64_t need = BINARY_HEADER_SIZE + (cells * sizeof(double));
    if (inSize < need)
    {
        return CodecStatus::TruncatedInput;
    }
    if (inSize > need)
    {
        return CodecStatus::SizeMismatch;  // trailing garbage
    }
    if (scratchCells < cells)
    {
        return CodecStatus::ScratchTooSmall;
    }
    // Copy into caller storage. When the source buffer is 8-byte aligned and the
    // caller retains it, out.cells could alias `in + BINARY_HEADER_SIZE` instead
    // (true zero-copy). Deferred to the v2 retention path.
    std::memcpy(scratch, in + BINARY_HEADER_SIZE, cells * sizeof(double));
    out = GridView{scratch, h, w, spec.noData};
    return CodecStatus::Ok;
}

CodecStatus encodeText(const GridView& grid, int8_t* out, uint64_t capacity, uint64_t& bytesWritten)
{
    char* dst = reinterpret_cast<char*>(out);
    const uint64_t n = grid.cellCount();
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n; ++i)
    {
        if (i != 0)
        {
            if (pos >= capacity)
            {
                return CodecStatus::OutputTooSmall;
            }
            dst[pos++] = ' ';
        }
        const double v = grid.cells[i];
        if (std::isnan(v))
        {
            if (pos + 3 > capacity)
            {
                return CodecStatus::OutputTooSmall;
            }
            dst[pos++] = 'N';
            dst[pos++] = 'a';
            dst[pos++] = 'N';
        }
        else
        {
            const auto res = std::to_chars(dst + pos, dst + capacity, v);
            if (res.ec != std::errc{})
            {
                return CodecStatus::OutputTooSmall;
            }
            pos = static_cast<uint64_t>(res.ptr - dst);
        }
    }
    bytesWritten = pos;
    return CodecStatus::Ok;
}

CodecStatus encodeBinary(const GridView& grid, const TileSpec& spec, int8_t* out, uint64_t capacity,
                         uint64_t& bytesWritten)
{
    const uint64_t cells = grid.cellCount();
    const uint64_t need = BINARY_HEADER_SIZE + (cells * sizeof(double));
    if (capacity < need)
    {
        return CodecStatus::OutputTooSmall;
    }
    std::memset(out, 0, BINARY_HEADER_SIZE);  // zero the reserved bytes
    out[0] = static_cast<int8_t>(WireFormat::Binary);
    out[1] = static_cast<int8_t>(spec.cellType);
    out[2] = static_cast<int8_t>(1);
    const uint32_t h = grid.h;
    const uint32_t w = grid.w;
    std::memcpy(out + 4, &h, sizeof h);
    std::memcpy(out + 8, &w, sizeof w);
    std::memcpy(out + 16, &spec.noData, sizeof(double));
    std::memcpy(out + BINARY_HEADER_SIZE, grid.cells, cells * sizeof(double));
    bytesWritten = need;
    return CodecStatus::Ok;
}
}  // namespace

CodecStatus decode(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells,
                   GridView& out) noexcept
{
    if (spec.height == 0 || spec.width == 0)
    {
        return CodecStatus::BadDimensions;
    }
    if (in == nullptr || scratch == nullptr)
    {
        return CodecStatus::TruncatedInput;
    }
    switch (spec.format)
    {
        case WireFormat::Text:
            return decodeText(in, inSize, spec, scratch, scratchCells, out);
        case WireFormat::Binary:
            return decodeBinary(in, inSize, spec, scratch, scratchCells, out);
    }
    return CodecStatus::UnsupportedVersion;
}

CodecStatus encode(const GridView& grid, const TileSpec& spec, int8_t* out, uint64_t capacity,
                   uint64_t& bytesWritten) noexcept
{
    bytesWritten = 0;
    if (spec.height == 0 || spec.width == 0)
    {
        return CodecStatus::BadDimensions;
    }
    if (grid.h != spec.height || grid.w != spec.width || grid.cells == nullptr)
    {
        return CodecStatus::SizeMismatch;
    }
    if (out == nullptr)
    {
        return CodecStatus::OutputTooSmall;
    }
    switch (spec.format)
    {
        case WireFormat::Text:
            return encodeText(grid, out, capacity, bytesWritten);
        case WireFormat::Binary:
            return encodeBinary(grid, spec, out, capacity, bytesWritten);
    }/// Plugin-neutral focal config. nes-logical-operators must NOT depend on the raster plugin
/// (dependency direction: plugins depend on core, not the reverse), so the logical op describes
/// the operation in primitives + local enums; the lowering rule maps these to Raster::{RasterGeometry,
/// FocalSpec, TileSpec}. (Alternative: include the plugin's FocalSpec.hpp and reuse Raster::ReductionOp
/// etc. — less code, but couples this module to a plugin. I default to decoupled.)
    return CodecStatus::UnsupportedVersion;
}

uint64_t encodedSizeBinary(const TileSpec& spec) noexcept
{
    return BINARY_HEADER_SIZE + (spec.cellCount() * sizeof(double));
}

uint64_t encodedSizeTextUpperBound(const TileSpec& spec) noexcept
{
    return spec.cellCount() * MAX_TEXT_CHARS_PER_CELL;
}
}