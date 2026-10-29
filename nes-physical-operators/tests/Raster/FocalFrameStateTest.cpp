#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <random>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <FocalSpec.hpp>
#include <Raster/Focal/FocalFrameState.hpp>

namespace NES::Focal
{
namespace
{

using Raster::RasterGeometry;
using Raster::TilePosition;
using Raster::TileReach;
using Tile = std::shared_ptr<int>;
using State = FocalFrameState<Tile>;

/// One FocalFrameState in its own aligned heap block; destroys the state and frees the block when it goes out of scope.
class Frame
{
public:
    Frame(const RasterGeometry& geo, TileReach reach, FrameKey key = {0, 1}, uint64_t ts = 1000, uint32_t outFrame = 1)
        : bytes(State::bytesFor(geo)), memory(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{64})))
    {
        state = State::create(std::span<std::byte>(memory, bytes), key, ts, outFrame, geo, reach);
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame()
    {
        state->destroy();
        ::operator delete(memory, std::align_val_t{64});
    }

    State* operator->() const { return state; }
    State& operator*() const { return *state; }

private:
    size_t bytes;
    std::byte* memory;
    State* state = nullptr;
};

/// Raster of tilesX x tilesY tiles of 4 x 4 cells (edge tiles full, which does not matter for the bookkeeping).
RasterGeometry grid(int32_t tilesX, int32_t tilesY)
{
    return RasterGeometry{static_cast<int64_t>(tilesX) * 4, static_cast<int64_t>(tilesY) * 4, 4, 4};
}

std::vector<TilePosition> positions(const RasterGeometry& geo)
{
    std::vector<TilePosition> all;
    for (int32_t y = 0; y < geo.tilesY(); ++y)
    {
        for (int32_t x = 0; x < geo.tilesX(); ++x)
        {
            all.push_back({x, y});
        }
    }
    return all;
}

std::vector<TilePosition> neighbours(const RasterGeometry& geo, TilePosition p, TileReach reach)
{
    std::vector<TilePosition> n;
    Raster::forEachNeighbourTile(geo, p.x, p.y, reach, [&](int32_t x, int32_t y) { n.push_back({x, y}); });
    return n;
}

size_t idx(const RasterGeometry& geo, TilePosition p)
{
    return Raster::tileIndex(geo, p);
}

TEST(FocalFrameStateTest, IdentityAndLayout)
{
    const auto geo = grid(4, 3);
    Frame fs(geo, TileReach{1, 1}, FrameKey{2, 9}, 4242, 17);
    EXPECT_EQ(fs->key(), (FrameKey{2, 9}));
    EXPECT_EQ(fs->ts(), 4242U);
    EXPECT_EQ(fs->outFrame(), 17U);
    EXPECT_EQ(fs->tileCount(), 12U);
    EXPECT_EQ(fs->remaining(), 12U);
    for (const auto p : positions(geo))
    {
        EXPECT_FALSE(fs->isPresent(p));
        EXPECT_FALSE(fs->isEmitted(p));
        EXPECT_EQ(fs->tile(p), nullptr);
    }
}

TEST(FocalFrameStateTest, CreateRejectsSmallOrMisalignedMemory)
{
    const auto geo = grid(2, 2);
    const size_t bytes = State::bytesFor(geo);
    auto* memory = static_cast<std::byte*>(::operator new(bytes + 64, std::align_val_t{64}));
    EXPECT_THROW((void)State::create(std::span<std::byte>(memory, bytes - 1), {0, 1}, 0, 1, geo, {1, 1}), std::invalid_argument);
    EXPECT_THROW((void)State::create(std::span<std::byte>(memory + 1, bytes), {0, 1}, 0, 1, geo, {1, 1}), std::invalid_argument);
    ::operator delete(memory, std::align_val_t{64});
}

/// missing[o] and readers[i] start at |N(.)|, clipped at the grid edge: corner 4, edge 6, centre 9 for k = 1.
TEST(FocalFrameStateTest, InitialCountersReachOne)
{
    const auto geo = grid(4, 3);
    Frame fs(geo, TileReach{1, 1});
    EXPECT_EQ(fs->missingInputs({0, 0}), 4U);
    EXPECT_EQ(fs->missingInputs({3, 2}), 4U);
    EXPECT_EQ(fs->missingInputs({1, 0}), 6U);
    EXPECT_EQ(fs->missingInputs({0, 1}), 6U);
    EXPECT_EQ(fs->missingInputs({1, 1}), 9U);
    EXPECT_EQ(fs->missingInputs({2, 1}), 9U);
    for (const auto p : positions(geo))
    {
        EXPECT_EQ(fs->pendingReaders(p), fs->missingInputs(p));
    }
}

/// r > T: radius 5 with 4-cell tiles gives k = 2, so N(o) spans 5 x 5 tiles, not only the 8 direct neighbours.
TEST(FocalFrameStateTest, InitialCountersReachTwo)
{
    const auto geo = grid(5, 5);
    const TileReach reach = Raster::computeTileReach(geo, 5);
    ASSERT_EQ(reach.horizontal, 2);
    ASSERT_EQ(reach.vertical, 2);
    Frame fs(geo, reach);
    EXPECT_EQ(fs->missingInputs({2, 2}), 25U);
    EXPECT_EQ(fs->missingInputs({0, 0}), 9U);
    EXPECT_EQ(fs->missingInputs({1, 1}), 16U);
    EXPECT_EQ(fs->missingInputs({2, 0}), 15U);
}

/// Asymmetric reach (non-square tiles): k = (2, 0) means each output reads only its row.
TEST(FocalFrameStateTest, InitialCountersAsymmetricReach)
{
    const auto geo = grid(5, 3);
    Frame fs(geo, TileReach{2, 0});
    EXPECT_EQ(fs->missingInputs({2, 1}), 5U);
    EXPECT_EQ(fs->missingInputs({0, 1}), 3U);
    EXPECT_EQ(fs->missingInputs({1, 0}), 4U);
}

/// Row-major arrival: each output is reported by exactly the arrival that completes its neighbourhood, never earlier.
TEST(FocalFrameStateTest, ReportsOutputExactlyWhenNeighbourhoodCompletes)
{
    const auto geo = grid(4, 3);
    const TileReach reach{1, 1};
    Frame fs(geo, reach);
    std::set<size_t> arrived;
    std::set<size_t> reported;
    for (const auto p : positions(geo))
    {
        ASSERT_EQ(fs->admit(p), Admission::New);
        fs->storeTile(p, std::make_shared<int>(static_cast<int>(idx(geo, p))));
        arrived.insert(idx(geo, p));

        std::set<size_t> now;
        fs->forEachCompletedBy(p, [&](TilePosition o) { now.insert(idx(geo, o)); });

        for (const auto o : positions(geo))
        {
            const auto n = neighbours(geo, o, reach);
            const bool complete = std::ranges::all_of(n, [&](TilePosition i) { return arrived.contains(idx(geo, i)); });
            const bool wasReported = reported.contains(idx(geo, o));
            /// reported now  <=>  complete now and not reported before
            EXPECT_EQ(now.contains(idx(geo, o)), complete && !wasReported) << "output (" << o.x << "," << o.y << ")";
        }
        reported.insert(now.begin(), now.end());
    }
    EXPECT_EQ(reported.size(), fs->tileCount());
}

/// Duplicates are detected before any counter changes
TEST(FocalFrameStateTest, DuplicateChangesNothing)
{
    const auto geo = grid(3, 3);
    Frame fs(geo, TileReach{1, 1});
    const TilePosition p{1, 1};
    ASSERT_EQ(fs->admit(p), Admission::New);
    fs->storeTile(p, std::make_shared<int>(1));
    fs->forEachCompletedBy(p, [](TilePosition) { FAIL() << "nothing can complete with one tile"; });

    std::vector<uint32_t> before;
    for (const auto o : positions(geo))
    {
        before.push_back(fs->missingInputs(o));
    }
    EXPECT_EQ(fs->admit(p), Admission::Duplicate);
    EXPECT_EQ(fs->admit(p), Admission::Duplicate);
    size_t i = 0;
    for (const auto o : positions(geo))
    {
        EXPECT_EQ(fs->missingInputs(o), before[i++]);
    }
    EXPECT_EQ(*fs->tile(p), 1);
}

TEST(FocalFrameStateTest, ClaimEmissionSucceedsOnce)
{
    const auto geo = grid(2, 2);
    Frame fs(geo, TileReach{1, 1});
    EXPECT_TRUE(fs->claimEmission({1, 0}));
    EXPECT_FALSE(fs->claimEmission({1, 0}));
    EXPECT_TRUE(fs->isEmitted({1, 0}));
    EXPECT_FALSE(fs->isEmitted({0, 0}));
}

/// Full protocol in random order: exactly-once emission and release
void runRandomOrder(const RasterGeometry& geo, TileReach reach, uint32_t seed)
{
    Frame fs(geo, reach);
    auto order = positions(geo);
    std::mt19937 rng(seed);
    std::ranges::shuffle(order, rng);

    std::vector<int> emittedCount(fs->tileCount(), 0);
    std::vector<int> releasedCount(fs->tileCount(), 0);
    std::vector<std::weak_ptr<int>> stored(fs->tileCount());
    int lastOutputs = 0;

    auto emit = [&](TilePosition o)
    {
        if (!fs->claimEmission(o))
        {
            return;
        }
        ++emittedCount[idx(geo, o)];
        for (const auto i : neighbours(geo, o, reach))
        {
            ASSERT_TRUE(fs->isPresent(i)) << "output emitted before its neighbourhood was complete";
            ASSERT_NE(fs->tile(i), nullptr) << "a tile the output reads was already released";
        }
        fs->forEachReleasableAfter(
            o,
            [&](TilePosition i)
            {
                ++releasedCount[idx(geo, i)];
                for (const auto reader : neighbours(geo, i, reach))
                {
                    EXPECT_TRUE(fs->isEmitted(reader)) << "tile released while an output still needs it";
                }
                fs->releaseTile(i);
            });
        if (fs->finishOutput())
        {
            ++lastOutputs;
        }
    };

    for (const auto p : order)
    {
        ASSERT_EQ(fs->admit(p), Admission::New);
        auto tile = std::make_shared<int>(static_cast<int>(idx(geo, p)));
        stored[idx(geo, p)] = tile;
        fs->storeTile(p, std::move(tile));
        fs->forEachCompletedBy(p, emit);
    }

    for (size_t i = 0; i < fs->tileCount(); ++i)
    {
        EXPECT_EQ(emittedCount[i], 1) << "output " << i;
        EXPECT_EQ(releasedCount[i], 1) << "tile " << i;
        EXPECT_TRUE(stored[i].expired()) << "tile " << i << " still stored";
    }
    EXPECT_EQ(lastOutputs, 1);
    EXPECT_EQ(fs->remaining(), 0U);
}

TEST(FocalFrameStateTest, RandomArrivalOrdersReachOne)
{
    for (uint32_t seed = 1; seed <= 100; ++seed)
    {
        runRandomOrder(grid(6, 5), TileReach{1, 1}, seed);
    }
}

TEST(FocalFrameStateTest, RandomArrivalOrdersReachTwo)
{
    for (uint32_t seed = 1; seed <= 100; ++seed)
    {
        runRandomOrder(grid(6, 5), TileReach{2, 2}, seed);
    }
}

TEST(FocalFrameStateTest, SingleTileRaster)
{
    runRandomOrder(grid(1, 1), TileReach{3, 3}, 1);
}

/// Deadline: exactly the unemitted outputs are reported and claimed once
TEST(FocalFrameStateTest, DeadlineEmitsEveryRemainingOutputOnce)
{
    const auto geo = grid(4, 4);
    const TileReach reach{1, 1};
    Frame fs(geo, reach);
    std::vector<int> emittedCount(fs->tileCount(), 0);
    std::vector<std::weak_ptr<int>> stored(fs->tileCount());
    auto emit = [&](TilePosition o)
    {
        if (fs->claimEmission(o))
        {
            ++emittedCount[idx(geo, o)];
            fs->forEachReleasableAfter(o, [&](TilePosition i) { fs->releaseTile(i); });
            (void)fs->finishOutput();
        }
    };

    /// all tiles except the last column arrive
    for (const auto p : positions(geo))
    {
        if (p.x == 3)
        {
            continue;
        }
        ASSERT_EQ(fs->admit(p), Admission::New);
        auto tile = std::make_shared<int>(0);
        stored[idx(geo, p)] = tile;
        fs->storeTile(p, std::move(tile));
        fs->forEachCompletedBy(p, emit);
    }
    /// outputs in columns 0 and 1 are complete; columns 2 and 3 read the missing column
    size_t emittedBefore = 0;
    for (const auto p : positions(geo))
    {
        EXPECT_EQ(fs->isEmitted(p), p.x <= 1);
        emittedBefore += fs->isEmitted(p) ? 1 : 0;
    }

    size_t reportedAtDeadline = 0;
    fs->forEachUnemittedOutput(
        [&](TilePosition o)
        {
            ++reportedAtDeadline;
            emit(o);
        });
    EXPECT_EQ(reportedAtDeadline, fs->tileCount() - emittedBefore);
    for (size_t i = 0; i < fs->tileCount(); ++i)
    {
        EXPECT_EQ(emittedCount[i], 1);
    }
    EXPECT_EQ(fs->remaining(), 0U);

    /// a second deadline pass finds nothing
    fs->forEachUnemittedOutput([](TilePosition) { FAIL() << "output reported twice"; });

    /// every output has been emitted, so every present tile has lost its last reader and was released
    for (size_t i = 0; i < fs->tileCount(); ++i)
    {
        EXPECT_TRUE(stored[i].expired()) << "tile " << i;
    }
}

/// Destroying the state releases tiles that are still stored (lazy path / deadline / slice teardown).
TEST(FocalFrameStateTest, DestroyReleasesStoredTiles)
{
    const auto geo = grid(3, 3);
    std::weak_ptr<int> watch;
    {
        Frame fs(geo, TileReach{1, 1});
        auto tile = std::make_shared<int>(5);
        watch = tile;
        ASSERT_EQ(fs->admit({0, 0}), Admission::New);
        fs->storeTile({0, 0}, std::move(tile));
        EXPECT_FALSE(watch.expired());
    }
    EXPECT_TRUE(watch.expired());
}

/// Eight threads with duplicates: exactly-once invariants under concurrency (also run under TSan)
TEST(FocalFrameStateTest, ConcurrentArrivalsWithDuplicates)
{
    constexpr size_t Threads = 8;
    constexpr int Rounds = 20;
    const auto geo = grid(12, 10);
    const TileReach reach{1, 1};

    for (int round = 0; round < Rounds; ++round)
    {
        Frame fs(geo, reach);
        const size_t n = fs->tileCount();
        std::vector<std::atomic<int>> emittedCount(n);
        std::vector<std::atomic<int>> releasedCount(n);
        std::vector<std::atomic<int>> admitted(n);
        std::vector<std::weak_ptr<int>> stored(n);
        std::atomic<int> lastOutputs{0};
        std::atomic<bool> violation{false};

        /// every position twice; each thread takes a contiguous share of a shuffled list
        std::vector<TilePosition> work;
        for (const auto p : positions(geo))
        {
            work.push_back(p);
            work.push_back(p);
        }
        std::mt19937 rng(static_cast<uint32_t>(round) + 7);
        std::ranges::shuffle(work, rng);

        /// weak_ptrs are written by the admitting thread only and read after join
        std::vector<std::shared_ptr<int>> tiles(n);
        for (size_t i = 0; i < n; ++i)
        {
            tiles[i] = std::make_shared<int>(static_cast<int>(i));
            stored[i] = tiles[i];
        }

        auto emit = [&](TilePosition o)
        {
            if (!fs->claimEmission(o))
            {
                return;
            }
            emittedCount[idx(geo, o)].fetch_add(1);
            for (const auto i : neighbours(geo, o, reach))
            {
                /// reading the stored tile is the real data dependency; TSan checks its publication
                const auto& t = fs->tile(i);
                if (!fs->isPresent(i) || t == nullptr || *t != static_cast<int>(idx(geo, i)))
                {
                    violation.store(true);
                }
            }
            fs->forEachReleasableAfter(
                o,
                [&](TilePosition i)
                {
                    releasedCount[idx(geo, i)].fetch_add(1);
                    for (const auto reader : neighbours(geo, i, reach))
                    {
                        if (!fs->isEmitted(reader))
                        {
                            violation.store(true);
                        }
                    }
                    fs->releaseTile(i);
                });
            if (fs->finishOutput())
            {
                lastOutputs.fetch_add(1);
            }
        };

        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        const size_t share = (work.size() + Threads - 1) / Threads;
        for (size_t t = 0; t < Threads; ++t)
        {
            threads.emplace_back(
                [&, t]
                {
                    while (!go.load(std::memory_order_acquire))
                    {
                    }
                    const size_t begin = t * share;
                    const size_t end = std::min(work.size(), begin + share);
                    for (size_t w = begin; w < end; ++w)
                    {
                        const auto p = work[w];
                        if (fs->admit(p) == Admission::Duplicate)
                        {
                            continue;
                        }
                        admitted[idx(geo, p)].fetch_add(1);
                        fs->storeTile(p, std::move(tiles[idx(geo, p)]));
                        fs->forEachCompletedBy(p, emit);
                    }
                });
        }
        go.store(true, std::memory_order_release);
        for (auto& thread : threads)
        {
            thread.join();
        }

        EXPECT_FALSE(violation.load()) << "round " << round;
        EXPECT_EQ(lastOutputs.load(), 1);
        EXPECT_EQ(fs->remaining(), 0U);
        for (size_t i = 0; i < n; ++i)
        {
            EXPECT_EQ(admitted[i].load(), 1) << "tile " << i;
            EXPECT_EQ(emittedCount[i].load(), 1) << "output " << i;
            EXPECT_EQ(releasedCount[i].load(), 1) << "tile " << i;
            EXPECT_TRUE(stored[i].expired()) << "tile " << i;
        }
    }
}

}
}
