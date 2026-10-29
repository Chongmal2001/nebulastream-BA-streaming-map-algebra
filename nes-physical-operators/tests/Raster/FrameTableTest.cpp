#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <random>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <Raster/FrameTable.hpp>
#include "HeapMemory.hpp"

namespace NES
{
namespace
{

using Testing::HeapMemory;

/// Minimal per-frame state: a key, and a counter of destroy() calls shared by all states of a test.
struct TestState
{
    FrameKey frameKey;
    std::atomic<size_t>* destroyed;

    [[nodiscard]] FrameKey key() const { return frameKey; }
    void destroy()
    {
        destroyed->fetch_add(1, std::memory_order_relaxed);
        this->~TestState();
    }
};

using Table = FrameTable<TestState, HeapMemory>;

class FrameTableTest : public ::testing::Test
{
protected:
    /// The create callback for getOrCreate: builds a TestState for `key` and counts how often it ran.
    auto creator(FrameKey key)
    {
        return [this, key](std::span<std::byte> memory)
        {
            constructed.fetch_add(1, std::memory_order_relaxed);
            return new (memory.data()) TestState{key, &destroyed};
        };
    }

    HeapMemory memory;
    std::atomic<size_t> constructed{0};
    std::atomic<size_t> destroyed{0};
};

TEST_F(FrameTableTest, EmptyTableHasOnePageAndFindsNothing)
{
    Table table(memory);
    EXPECT_EQ(table.pageCount(), 1U);
    EXPECT_EQ(table.size(), 0U);
    EXPECT_EQ(table.find(FrameKey{0, 1}), nullptr);
    EXPECT_EQ(memory.adopted(), 1U);   // the head page
}

TEST_F(FrameTableTest, GetOrCreateCreatesOnceThenFinds)
{
    Table table(memory);
    const FrameKey key{0, 7};

    auto [first, created] = table.getOrCreate(key, sizeof(TestState), creator(key));
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(created);
    EXPECT_EQ(first->key(), key);

    auto [second, createdAgain] = table.getOrCreate(key, sizeof(TestState), creator(key));
    EXPECT_EQ(second, first);
    EXPECT_FALSE(createdAgain);
    EXPECT_EQ(constructed.load(), 1U);   // the second call found the frame and never ran create
    EXPECT_EQ(table.find(key), first);
    EXPECT_EQ(table.size(), 1U);

    table.destroyAll();
    EXPECT_EQ(destroyed.load(), 1U);
}

TEST_F(FrameTableTest, DistinguishesOriginsWithTheSameFrameId)
{
    Table table(memory);
    auto [a, createdA] = table.getOrCreate(FrameKey{0, 5}, sizeof(TestState), creator(FrameKey{0, 5}));
    auto [b, createdB] = table.getOrCreate(FrameKey{1, 5}, sizeof(TestState), creator(FrameKey{1, 5}));
    EXPECT_TRUE(createdA);
    EXPECT_TRUE(createdB);
    EXPECT_NE(a, b);
    EXPECT_EQ(table.find(FrameKey{0, 5}), a);
    EXPECT_EQ(table.find(FrameKey{1, 5}), b);
    EXPECT_EQ(table.find(FrameKey{2, 5}), nullptr);
    table.destroyAll();
}

/// More frames than one page: pages appended, all findable; S is no limit
TEST_F(FrameTableTest, AppendsPagesBeyondCapacity)
{
    constexpr uint32_t Origins = 3;
    constexpr uint32_t FramesPerOrigin = 40;
    Table table(memory);

    std::vector<TestState*> states;
    for (uint32_t origin = 0; origin < Origins; ++origin)
    {
        for (uint32_t frame = 1; frame <= FramesPerOrigin; ++frame)
        {
            const FrameKey key{origin, frame};
            auto [state, created] = table.getOrCreate(key, sizeof(TestState), creator(key));
            ASSERT_TRUE(created);
            states.push_back(state);
        }
    }

    const size_t total = Origins * FramesPerOrigin;
    EXPECT_EQ(table.size(), total);
    EXPECT_EQ(table.pageCount(), (total + 7) / 8);   // pages fill completely before the next one is appended
    size_t i = 0;
    for (uint32_t origin = 0; origin < Origins; ++origin)
    {
        for (uint32_t frame = 1; frame <= FramesPerOrigin; ++frame)
        {
            EXPECT_EQ(table.find(FrameKey{origin, frame}), states[i++]);
        }
    }
    EXPECT_EQ(table.find(FrameKey{0, FramesPerOrigin + 1}), nullptr);
    EXPECT_EQ(table.find(FrameKey{Origins, 1}), nullptr);

    /// head page + appended pages + one block per frame, all adopted; nothing pending
    EXPECT_EQ(memory.adopted(), table.pageCount() + total);
    EXPECT_EQ(memory.live(), memory.adopted());

    table.destroyAll();
    EXPECT_EQ(destroyed.load(), total);
}

TEST_F(FrameTableTest, ForEachVisitsEveryFrameOnce)
{
    Table table(memory);
    for (uint32_t frame = 1; frame <= 20; ++frame)
    {
        (void)table.getOrCreate(FrameKey{0, frame}, sizeof(TestState), creator(FrameKey{0, frame}));
    }
    std::vector<uint32_t> seen;
    table.forEach([&](TestState& s) { seen.push_back(s.key().frameId); });
    std::ranges::sort(seen);
    ASSERT_EQ(seen.size(), 20U);
    for (uint32_t frame = 1; frame <= 20; ++frame)
    {
        EXPECT_EQ(seen[frame - 1], frame);
    }
    table.destroyAll();
}

/// Threads race to create the same frames: each created once, lost races clean up
TEST_F(FrameTableTest, ConcurrentGetOrCreateCreatesEachFrameOnce)
{
    constexpr size_t Threads = 8;
    constexpr uint32_t Origins = 2;
    constexpr uint32_t FramesPerOrigin = 100;
    constexpr size_t Keys = Origins * FramesPerOrigin;
    Table table(memory);

    std::vector<FrameKey> keys;
    for (uint32_t origin = 0; origin < Origins; ++origin)
    {
        for (uint32_t frame = 1; frame <= FramesPerOrigin; ++frame)
        {
            keys.push_back(FrameKey{origin, frame});
        }
    }

    std::vector<std::vector<TestState*>> results(Threads, std::vector<TestState*>(Keys, nullptr));
    std::vector<std::vector<uint8_t>> createdBy(Threads, std::vector<uint8_t>(Keys, 0));
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < Threads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                std::vector<size_t> order(Keys);
                for (size_t i = 0; i < Keys; ++i)
                {
                    order[i] = i;
                }
                std::mt19937 rng(static_cast<uint32_t>(t) + 1);
                std::ranges::shuffle(order, rng);
                while (!go.load(std::memory_order_acquire))
                {
                }
                for (const size_t i : order)
                {
                    auto [state, created] = table.getOrCreate(keys[i], sizeof(TestState), creator(keys[i]));
                    results[t][i] = state;
                    createdBy[t][i] = created ? 1 : 0;
                }
            });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads)
    {
        thread.join();
    }

    for (size_t i = 0; i < Keys; ++i)
    {
        size_t creators = 0;
        for (size_t t = 0; t < Threads; ++t)
        {
            EXPECT_EQ(results[t][i], results[0][i]) << "threads disagree on frame " << i;
            creators += createdBy[t][i];
        }
        EXPECT_EQ(creators, 1U) << "frame " << i << " created " << creators << " times";
        ASSERT_NE(results[0][i], nullptr);
        EXPECT_EQ(results[0][i]->key(), keys[i]);
    }

    EXPECT_EQ(table.size(), Keys);
    EXPECT_EQ(table.pageCount(), (Keys + 7) / 8);
    /// every lost race destroyed its own state; their blocks (and lost page candidates) were freed
    EXPECT_EQ(destroyed.load(), constructed.load() - Keys);
    EXPECT_EQ(memory.adopted(), table.pageCount() + Keys);
    EXPECT_EQ(memory.live(), memory.adopted());

    table.destroyAll();
    EXPECT_EQ(destroyed.load(), constructed.load());
}

}
}
