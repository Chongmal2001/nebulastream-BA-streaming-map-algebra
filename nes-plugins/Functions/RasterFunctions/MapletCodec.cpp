#include <MapletCodec.hpp>

#include <algorithm>
#include <cstdlib>
#include <cerrno>
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

/// Parses one token; optionally signed "nan" (any case) is NaN
bool parseCell(const char* tokenBegin, const char* tokenEnd, double& parsedValue)
{
    const std::ptrdiff_t tokenLength = tokenEnd - tokenBegin;
    if (tokenLength <= 0)
    {
        return false;
    }

    // explicit NaN, independent of strtod's nan()
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

    // null-terminated copy so strtod stays inside the token
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

    // whole token must parse; only overflow is a range error (denormals kept)
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
    out = GridView{scratch, spec.height, spec.width, spec.noData, spec.bands};
    return CodecStatus::Ok;
}

/// Parses and checks the preamble without looking at the cells. Shared by peekHeader, decode and selectBands.
CodecStatus readPreamble(const int8_t* in, uint64_t inSize, MapletHeader& h)
{
    if (in == nullptr || inSize < BINARY_HEADER_SIZE)
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
    h.bands = static_cast<uint8_t>(in[2]);
    h.flags = static_cast<uint8_t>(in[3]);
    std::memcpy(&h.frameId, in + 4, sizeof h.frameId);
    std::memcpy(&h.tileX, in + 8, sizeof h.tileX);
    std::memcpy(&h.tileY, in + 12, sizeof h.tileY);
    std::memcpy(&h.height, in + 16, sizeof h.height);
    std::memcpy(&h.width, in + 20, sizeof h.width);
    std::memcpy(&h.rasterWidth, in + 24, sizeof h.rasterWidth);
    std::memcpy(&h.rasterHeight, in + 28, sizeof h.rasterHeight);
    if (h.bands == 0 || h.height == 0 || h.width == 0)
    {
        return CodecStatus::BadDimensions;
    }
    // planeCells() fits in 64 bits (two u32); bounding it by the input first keeps bands * plane * 8 from overflowing.
    if (h.planeCells() > (inSize - BINARY_HEADER_SIZE) / sizeof(double))
    {
        return CodecStatus::TruncatedInput;
    }
    const uint64_t need = h.totalSize();
    if (inSize < need)
    {
        return CodecStatus::TruncatedInput;
    }
    if (inSize > need)
    {
        return CodecStatus::SizeMismatch;  // trailing bytes
    }
    if (h.rasterWidth != 0 && h.rasterHeight != 0)
    {
        const int64_t tilesX = (static_cast<int64_t>(h.rasterWidth) + h.width - 1) / h.width;
        const int64_t tilesY = (static_cast<int64_t>(h.rasterHeight) + h.height - 1) / h.height;
        if (h.tileX < 0 || h.tileY < 0 || h.tileX >= tilesX || h.tileY >= tilesY)
        {
            return CodecStatus::PositionOutOfRange;
        }
    }
    return CodecStatus::Ok;
}

CodecStatus decodeBinary(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells,
                         GridView& out, MapletHeader* hdrOut)
{
    MapletHeader h;
    if (const auto st = readPreamble(in, inSize, h); st != CodecStatus::Ok)
    {
        return st;
    }
    if (h.bands != spec.bands || h.height != spec.height || h.width != spec.width)
    {
        return CodecStatus::SizeMismatch;  // shape is a per-stream invariant
    }
    if ((spec.rasterWidth != 0 && h.rasterWidth != spec.rasterWidth) || (spec.rasterHeight != 0 && h.rasterHeight != spec.rasterHeight))
    {
        return CodecStatus::SizeMismatch;
    }
    const uint64_t cells = h.cellCount();
    if (scratchCells < cells)
    {
        return CodecStatus::ScratchTooSmall;
    }
    // copy out: VARSIZED payload may be unaligned
    std::memcpy(scratch, in + BINARY_HEADER_SIZE, cells * sizeof(double));
    out = GridView{scratch, h.height, h.width, spec.noData, h.bands};
    if (hdrOut != nullptr)
    {
        *hdrOut = h;
    }
    return CodecStatus::Ok;
}

void writePreamble(int8_t* out, const MapletHeader& h)
{
    out[0] = static_cast<int8_t>(WireFormat::Binary);
    out[1] = static_cast<int8_t>(CellType::Float64);
    out[2] = static_cast<int8_t>(h.bands);
    out[3] = static_cast<int8_t>(h.flags);
    std::memcpy(out + 4, &h.frameId, sizeof h.frameId);
    std::memcpy(out + 8, &h.tileX, sizeof h.tileX);
    std::memcpy(out + 12, &h.tileY, sizeof h.tileY);
    std::memcpy(out + 16, &h.height, sizeof h.height);
    std::memcpy(out + 20, &h.width, sizeof h.width);
    std::memcpy(out + 24, &h.rasterWidth, sizeof h.rasterWidth);
    std::memcpy(out + 28, &h.rasterHeight, sizeof h.rasterHeight);
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

CodecStatus encodeBinary(const GridView& grid, const TileSpec& spec, const MapletPlacement* placement, int8_t* out,
                         uint64_t capacity, uint64_t& bytesWritten)
{
    if (placement == nullptr)
    {
        return CodecStatus::MissingPlacement;
    }
    if (spec.rasterWidth == 0 || spec.rasterHeight == 0)
    {
        return CodecStatus::BadDimensions;  // a binary maplet must say where its raster ends
    }
    if (placement->tileX < 0 || placement->tileY < 0 || static_cast<uint32_t>(placement->tileX) >= spec.tilesX()
        || static_cast<uint32_t>(placement->tileY) >= spec.tilesY())
    {
        return CodecStatus::PositionOutOfRange;
    }
    const MapletHeader h{spec.bands,  placement->flags, placement->frameId, placement->tileX, placement->tileY,
                         spec.height, spec.width,       spec.rasterWidth,   spec.rasterHeight};
    const uint64_t need = h.totalSize();
    if (capacity < need)
    {
        return CodecStatus::OutputTooSmall;
    }
    writePreamble(out, h);
    std::memcpy(out + BINARY_HEADER_SIZE, grid.cells, h.cellCount() * sizeof(double));
    bytesWritten = need;
    return CodecStatus::Ok;
}
}  // namespace

std::string_view toString(CodecStatus status) noexcept
{
    switch (status)
    {
        case CodecStatus::Ok: return "Ok";
        case CodecStatus::TruncatedInput: return "TruncatedInput";
        case CodecStatus::UnsupportedVersion: return "UnsupportedVersion";
        case CodecStatus::UnsupportedCellType: return "UnsupportedCellType";
        case CodecStatus::SizeMismatch: return "SizeMismatch";
        case CodecStatus::ScratchTooSmall: return "ScratchTooSmall";
        case CodecStatus::OutputTooSmall: return "OutputTooSmall";
        case CodecStatus::MalformedNumber: return "MalformedNumber";
        case CodecStatus::BadDimensions: return "BadDimensions";
        case CodecStatus::BandOutOfRange: return "BandOutOfRange";
        case CodecStatus::PositionOutOfRange: return "PositionOutOfRange";
        case CodecStatus::MissingPlacement: return "MissingPlacement";
    }
    return "UnknownStatus";
}

uint32_t MapletHeader::validRows() const
{
    const int64_t v = static_cast<int64_t>(rasterHeight) - (static_cast<int64_t>(tileY) * height);
    return static_cast<uint32_t>(std::clamp<int64_t>(v, 0, height));
}

uint32_t MapletHeader::validCols() const
{
    const int64_t v = static_cast<int64_t>(rasterWidth) - (static_cast<int64_t>(tileX) * width);
    return static_cast<uint32_t>(std::clamp<int64_t>(v, 0, width));
}

CodecStatus peekHeader(const int8_t* in, uint64_t inSize, MapletHeader& out) noexcept
{
    return readPreamble(in, inSize, out);
}

CodecStatus peekFrameHeader(const int8_t* in, uint64_t inSize, FrameHeader& out) noexcept
{
    if (in == nullptr || inSize < FRAME_HEADER_SIZE)
    {
        return CodecStatus::TruncatedInput;
    }
    if (std::memcmp(in, FRAME_MAGIC.data(), FRAME_MAGIC.size()) != 0)
    {
        return CodecStatus::UnsupportedVersion;
    }
    if (static_cast<uint8_t>(in[41]) != static_cast<uint8_t>(CellType::Float64))
    {
        return CodecStatus::UnsupportedCellType;
    }
    FrameHeader h;
    std::memcpy(&h.frameId, in + 8, sizeof h.frameId);
    std::memcpy(&h.nTiles, in + 12, sizeof h.nTiles);
    std::memcpy(&h.eventTimeMs, in + 16, sizeof h.eventTimeMs);
    std::memcpy(&h.rasterWidth, in + 24, sizeof h.rasterWidth);
    std::memcpy(&h.rasterHeight, in + 28, sizeof h.rasterHeight);
    std::memcpy(&h.tileWidth, in + 32, sizeof h.tileWidth);
    std::memcpy(&h.tileHeight, in + 36, sizeof h.tileHeight);
    h.bands = static_cast<uint8_t>(in[40]);
    std::memcpy(&h.flags, in + 42, sizeof h.flags);
    std::memcpy(&h.noData, in + 48, sizeof h.noData);
    std::memcpy(&h.crsEpsg, in + 56, sizeof h.crsEpsg);
    std::memcpy(h.transform.data(), in + 64, sizeof(double) * h.transform.size());
    if (h.bands == 0 || h.rasterWidth == 0 || h.rasterHeight == 0 || h.tileWidth == 0 || h.tileHeight == 0)
    {
        return CodecStatus::BadDimensions;
    }
    out = h;
    return CodecStatus::Ok;
}

CodecStatus decode(const int8_t* in, uint64_t inSize, const TileSpec& spec, double* scratch, uint64_t scratchCells, GridView& out,
                   MapletHeader* hdrOut) noexcept
{
    if (spec.height == 0 || spec.width == 0 || spec.bands == 0)
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
            return decodeBinary(in, inSize, spec, scratch, scratchCells, out, hdrOut);
    }
    return CodecStatus::UnsupportedVersion;
}

CodecStatus encode(const GridView& grid, const TileSpec& spec, const MapletPlacement* placement, int8_t* out, uint64_t capacity,
                   uint64_t& bytesWritten) noexcept
{
    bytesWritten = 0;
    if (spec.height == 0 || spec.width == 0 || spec.bands == 0)
    {
        return CodecStatus::BadDimensions;
    }
    if (grid.h != spec.height || grid.w != spec.width || grid.bands != spec.bands || grid.cells == nullptr)
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
            return encodeBinary(grid, spec, placement, out, capacity, bytesWritten);
    }
    return CodecStatus::UnsupportedVersion;
}

uint64_t selectedSize(const MapletHeader& hdr, uint32_t k) noexcept
{
    return BINARY_HEADER_SIZE + (static_cast<uint64_t>(k) * hdr.planeCells() * sizeof(double));
}

CodecStatus beginBandSelection(const int8_t* in, uint64_t inSize, uint32_t k, int8_t* out, uint64_t capacity) noexcept
{
    MapletHeader h;
    if (const auto st = readPreamble(in, inSize, h); st != CodecStatus::Ok)
    {
        return st;
    }
    if (k == 0 || k > MAX_BANDS)
    {
        return CodecStatus::BandOutOfRange;
    }
    if (out == nullptr || capacity < selectedSize(h, k))
    {
        return CodecStatus::OutputTooSmall;
    }
    std::memcpy(out, in, BINARY_HEADER_SIZE);  // position, frame id, flags, raster size travel unchanged
    out[2] = static_cast<int8_t>(k);
    return CodecStatus::Ok;
}

CodecStatus copyBand(const int8_t* in, uint64_t inSize, uint32_t band, int8_t* out, uint64_t outSize, uint32_t slot) noexcept
{
    MapletHeader src;
    if (const auto st = readPreamble(in, inSize, src); st != CodecStatus::Ok)
    {
        return st;
    }
    MapletHeader dst;
    if (const auto st = readPreamble(out, outSize, dst); st != CodecStatus::Ok)
    {
        return st;  // not begun with beginBandSelection, or sized wrongly
    }
    if (dst.height != src.height || dst.width != src.width)
    {
        return CodecStatus::SizeMismatch;
    }
    if (band >= src.bands || slot >= dst.bands)
    {
        return CodecStatus::BandOutOfRange;
    }
    const uint64_t planeBytes = src.planeCells() * sizeof(double);
    std::memcpy(out + BINARY_HEADER_SIZE + (slot * planeBytes), in + BINARY_HEADER_SIZE + (band * planeBytes), planeBytes);
    return CodecStatus::Ok;
}

CodecStatus selectBands(const int8_t* in, uint64_t inSize, const uint8_t* idx, uint32_t k, int8_t* out, uint64_t capacity,
                        uint64_t& bytesWritten) noexcept
{
    bytesWritten = 0;
    if (idx == nullptr)
    {
        return CodecStatus::BandOutOfRange;
    }
    MapletHeader h;
    if (const auto st = readPreamble(in, inSize, h); st != CodecStatus::Ok)
    {
        return st;
    }
    for (uint32_t i = 0; i < k; ++i)  // check every index before writing anything
    {
        if (idx[i] >= h.bands)
        {
            return CodecStatus::BandOutOfRange;
        }
    }
    if (const auto st = beginBandSelection(in, inSize, k, out, capacity); st != CodecStatus::Ok)
    {
        return st;
    }
    const uint64_t need = selectedSize(h, k);
    for (uint32_t i = 0; i < k; ++i)
    {
        if (const auto st = copyBand(in, inSize, idx[i], out, need, i); st != CodecStatus::Ok)
        {
            return st;
        }
    }
    bytesWritten = need;
    return CodecStatus::Ok;
}

CodecStatus setFrameId(int8_t* maplet, uint64_t size, uint32_t frameId) noexcept
{
    if (maplet == nullptr || size < BINARY_HEADER_SIZE)
    {
        return CodecStatus::TruncatedInput;
    }
    if (static_cast<uint8_t>(maplet[0]) != static_cast<uint8_t>(WireFormat::Binary))
    {
        return CodecStatus::UnsupportedVersion;
    }
    std::memcpy(maplet + OFFSET_FRAME_ID, &frameId, sizeof frameId);
    return CodecStatus::Ok;
}

CodecStatus setFlags(int8_t* maplet, uint64_t size, uint8_t flags) noexcept
{
    if (maplet == nullptr || size < BINARY_HEADER_SIZE)
    {
        return CodecStatus::TruncatedInput;
    }
    if (static_cast<uint8_t>(maplet[0]) != static_cast<uint8_t>(WireFormat::Binary))
    {
        return CodecStatus::UnsupportedVersion;
    }
    maplet[OFFSET_FLAGS] = static_cast<int8_t>(flags);
    return CodecStatus::Ok;
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