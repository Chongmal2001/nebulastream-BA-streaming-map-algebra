#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

#include <FocalSpec.hpp>
#include <MapletCodec.hpp>
#include <Raster/FrameTable.hpp>

/// Per-frame focal state: presence, missing / reader counters, emitted flags, stored tiles; all atomic
namespace NES::Focal
{

/// How the frame's tiles are encoded (wire format, band count); the probe encodes the frame's outputs the same way.
struct FrameFormat
{
    Raster::WireFormat format = Raster::WireFormat::Text;
    uint8_t bands = 1;

    [[nodiscard]] bool operator==(const FrameFormat&) const = default;
};

enum class Admission : uint8_t
{
    New,        // first arrival of this position: store it and update the counters
    Duplicate,  // the position already arrived: drop the tile, change nothing
};

template <typename TileRef>
class FocalFrameState
{
public:
    using TilePosition = Raster::TilePosition;

    /// Bytes of state for one frame of this geometry (header and all per-position arrays).
    [[nodiscard]] static size_t bytesFor(const Raster::RasterGeometry& geo) { return layout(tileCountOf(geo)).total; }

    /// Builds the state in memory (bytesFor(geo), max-aligned), fully initialised
    static FocalFrameState* create(std::span<std::byte> memory, FrameKey key, uint64_t ts, uint32_t outFrame, const Raster::RasterGeometry& geo,
                              Raster::TileReach reach, FrameLineage lineage = {}, FrameFormat format = {})
    {
        const size_t n = tileCountOf(geo);
        const Layout l = layout(n);
        if (memory.size() < l.total || reinterpret_cast<uintptr_t>(memory.data()) % alignof(std::max_align_t) != 0)
        {
            throw std::invalid_argument("FocalFrameState::create: memory too small or misaligned");
        }
        std::byte* base = memory.data();
        auto* fs = new (base) FocalFrameState(key, ts, outFrame, geo, reach, n, lineage, format);
        fs->present = construct<std::atomic<uint8_t>>(base + l.present, n, 0);
        fs->emitted = construct<std::atomic<uint8_t>>(base + l.emitted, n, 0);
        fs->missing = construct<std::atomic<uint32_t>>(base + l.missing, n, 0);
        fs->readers = construct<std::atomic<uint32_t>>(base + l.readers, n, 0);
        fs->tiles = reinterpret_cast<TileRef*>(base + l.tiles);
        for (size_t i = 0; i < n; ++i)
        {
            new (&fs->tiles[i]) TileRef();
        }
        fs->initializeCounters();
        return fs;
    }

    /// Releases every tile still stored and ends the lifetime of the state. The memory itself belongs to the caller.
    void destroy()
    {
        for (size_t i = 0; i < count; ++i)
        {
            tiles[i].~TileRef();
        }
        this->~FocalFrameState();
    }

    FocalFrameState(const FocalFrameState&) = delete;
    FocalFrameState& operator=(const FocalFrameState&) = delete;

    [[nodiscard]] FrameKey key() const { return frameKey; }
    [[nodiscard]] FrameLineage lineage() const { return frameLineage; }
    [[nodiscard]] FrameFormat format() const { return frameFormat; }
    [[nodiscard]] uint64_t ts() const { return eventTime; }
    [[nodiscard]] uint32_t outFrame() const { return outputFrame; }
    [[nodiscard]] const Raster::RasterGeometry& geometry() const { return geo; }
    [[nodiscard]] Raster::TileReach reach() const { return tileReach; }
    [[nodiscard]] size_t tileCount() const { return count; }

    /// Records the arrival of input tile p. Returns Duplicate (and changes nothing) if p already arrived.
    [[nodiscard]] Admission admit(TilePosition p)
    {
        uint8_t expected = 0;
        return present[index(p)].compare_exchange_strong(expected, 1, std::memory_order_acq_rel) ? Admission::New : Admission::Duplicate;
    }

    [[nodiscard]] bool isPresent(TilePosition p) const { return present[index(p)].load(std::memory_order_acquire) != 0; }

    /// Stores the tile admitted at p; call before forEachCompletedBy(p)
    void storeTile(TilePosition p, TileRef ref) { tiles[index(p)] = std::move(ref); }

    [[nodiscard]] const TileRef& tile(TilePosition p) const { return tiles[index(p)]; }

    /// Drops the stored tile at p (its readers have all been emitted).
    void releaseTile(TilePosition p) { tiles[index(p)] = TileRef(); }

    /// After storing p: calls fn(o) for every output o now complete
    template <typename Fn>
    void forEachCompletedBy(TilePosition p, Fn&& fn)
    {
        Raster::forEachNeighbourTile(geo, p.x, p.y, tileReach,
                                     [&](int32_t x, int32_t y)
                                     {
                                         const TilePosition o{x, y};
                                         if (missing[index(o)].fetch_sub(1, std::memory_order_acq_rel) == 1)
                                         {
                                             fn(o);
                                         }
                                     });
    }

    [[nodiscard]] uint32_t missingInputs(TilePosition o) const { return missing[index(o)].load(std::memory_order_acquire); }

    /// Claims output o for emission. Returns true for exactly one caller over the lifetime of the frame.
    [[nodiscard]] bool claimEmission(TilePosition o)
    {
        uint8_t expected = 0;
        return emitted[index(o)].compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool isEmitted(TilePosition o) const { return emitted[index(o)].load(std::memory_order_acquire) != 0; }

    /// After emitting o: calls fn(i) for every input i no longer needed
    template <typename Fn>
    void forEachReleasableAfter(TilePosition o, Fn&& fn)
    {
        Raster::forEachNeighbourTile(geo, o.x, o.y, tileReach,
                                     [&](int32_t x, int32_t y)
                                     {
                                         const TilePosition i{x, y};
                                         if (isPresent(i) && readers[index(i)].fetch_sub(1, std::memory_order_acq_rel) == 1)
                                         {
                                             fn(i);
                                         }
                                     });
    }

    [[nodiscard]] uint32_t pendingReaders(TilePosition i) const { return readers[index(i)].load(std::memory_order_acquire); }

    /// Counts one emitted output. Returns true for the call that emits the frame's last output.
    [[nodiscard]] bool finishOutput() { return remainingOutputs.fetch_sub(1, std::memory_order_acq_rel) == 1; }

    [[nodiscard]] uint32_t remaining() const { return remainingOutputs.load(std::memory_order_acquire); }

    /// Deadline: calls fn(o) for every output not emitted yet
    template <typename Fn>
    void forEachUnemittedOutput(Fn&& fn) const
    {
        for (int32_t y = 0; y < geo.tilesY(); ++y)
        {
            for (int32_t x = 0; x < geo.tilesX(); ++x)
            {
                if (!isEmitted({x, y}))
                {
                    fn(TilePosition{x, y});
                }
            }
        }
    }

private:
    struct Layout
    {
        size_t present, emitted, missing, readers, tiles, total;
    };

    FocalFrameState(
        FrameKey key,
        uint64_t ts,
        uint32_t outFrame,
        const Raster::RasterGeometry& geo,
        Raster::TileReach reach,
        size_t n,
        FrameLineage lineage,
        FrameFormat format)
        : frameKey(key)
        , frameLineage(lineage)
        , frameFormat(format)
        , eventTime(ts)
        , outputFrame(outFrame)
        , geo(geo)
        , tileReach(reach)
        , count(n)
        , remainingOutputs(static_cast<uint32_t>(n))
    {
    }

    ~FocalFrameState() = default;

    static size_t tileCountOf(const Raster::RasterGeometry& geo)
    {
        return static_cast<size_t>(geo.tilesX()) * static_cast<size_t>(geo.tilesY());
    }

    static size_t alignUp(size_t offset, size_t alignment) { return (offset + alignment - 1) / alignment * alignment; }

    static Layout layout(size_t n)
    {
        Layout l{};
        size_t at = sizeof(FocalFrameState);
        l.present = at = alignUp(at, alignof(std::atomic<uint8_t>));
        l.emitted = at = alignUp(at + (n * sizeof(std::atomic<uint8_t>)), alignof(std::atomic<uint8_t>));
        l.missing = at = alignUp(at + (n * sizeof(std::atomic<uint8_t>)), alignof(std::atomic<uint32_t>));
        l.readers = at = alignUp(at + (n * sizeof(std::atomic<uint32_t>)), alignof(std::atomic<uint32_t>));
        l.tiles = at = alignUp(at + (n * sizeof(std::atomic<uint32_t>)), alignof(TileRef));
        l.total = at + (n * sizeof(TileRef));
        return l;
    }

    template <typename A>
    static A* construct(std::byte* at, size_t n, typename A::value_type initial)
    {
        auto* first = reinterpret_cast<A*>(at);
        for (size_t i = 0; i < n; ++i)
        {
            new (&first[i]) A(initial);
        }
        return first;
    }

    /// missing[o] = |N(o)| and readers[i] = |N(i)|: the number of grid tiles within the tile reach (clipped at the edges).
    void initializeCounters()
    {
        for (int32_t y = 0; y < geo.tilesY(); ++y)
        {
            for (int32_t x = 0; x < geo.tilesX(); ++x)
            {
                uint32_t neighbours = 0;
                Raster::forEachNeighbourTile(geo, x, y, tileReach, [&](int32_t, int32_t) { ++neighbours; });
                missing[index({x, y})].store(neighbours, std::memory_order_relaxed);
                readers[index({x, y})].store(neighbours, std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] size_t index(TilePosition p) const { return Raster::tileIndex(geo, p); }

    const FrameKey frameKey;
    const FrameLineage frameLineage;   // input store and store frame of this frame, copied to its outputs
    const FrameFormat frameFormat;     // encoding of the frame's tiles, used for its outputs
    const uint64_t eventTime;
    const uint32_t outputFrame;
    const Raster::RasterGeometry geo;
    const Raster::TileReach tileReach;
    const size_t count;
    std::atomic<uint32_t> remainingOutputs;
    std::atomic<uint8_t>* present = nullptr;
    std::atomic<uint8_t>* emitted = nullptr;
    std::atomic<uint32_t>* missing = nullptr;
    std::atomic<uint32_t>* readers = nullptr;
    TileRef* tiles = nullptr;
};

}