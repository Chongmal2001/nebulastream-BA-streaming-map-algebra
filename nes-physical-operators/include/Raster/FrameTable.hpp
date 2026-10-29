#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>

/// Lock-free paged index of the frames in flight in one slice; memory from a Memory policy (allocate / adopt)
namespace NES
{

/// Frame identity: dense origin index and source frame id (0 = unassigned)
struct FrameKey
{
    uint32_t originIdx = 0;
    uint32_t frameId = 0;

    [[nodiscard]] uint64_t packed() const { return (static_cast<uint64_t>(originIdx) << 32) | frameId; }
    [[nodiscard]] bool operator==(const FrameKey&) const = default;
};

/// Origin of a frame (store, store frame number); 0 = unknown, never a key
struct FrameLineage
{
    uint32_t srcId = 0;
    uint32_t srcFrameId = 0;

    [[nodiscard]] bool operator==(const FrameLineage&) const = default;
};

template <typename State, typename Memory, size_t S = 8>
class FrameTable
{
public:
    explicit FrameTable(Memory& memory) : memory(memory)
    {
        auto block = memory.allocate(sizeof(Page));
        head = new (block.bytes().data()) Page();
        memory.adopt(std::move(block));
    }

    FrameTable(const FrameTable&) = delete;
    FrameTable& operator=(const FrameTable&) = delete;

    /// State of frame key, nullptr if none; never locks
    template <typename Key>
    [[nodiscard]] State* find(const Key& key) const
    {
        const size_t home = homeSlot(key);
        for (const Page* page = head; page != nullptr; page = page->next.load(std::memory_order_acquire))
        {
            for (size_t j = 0; j < S; ++j)
            {
                State* s = page->slots[(home + j) % S].load(std::memory_order_acquire);
                if (s == nullptr)
                {
                    return nullptr;   // slots fill in probe order and are never cleared: the key is not further back
                }
                if (sameKey(s->key(), key))
                {
                    return s;
                }
            }
        }
        return nullptr;
    }

    /// State of frame key, created if absent; a lost race destroys its fresh state. Returns {state, created}
    template <typename Key, typename Create>
    std::pair<State*, bool> getOrCreate(const Key& key, size_t stateBytes, Create&& create)
    {
        if (State* existing = find(key))
        {
            return {existing, false};
        }
        auto block = memory.allocate(stateBytes);
        State* fresh = create(block.bytes());
        const size_t home = homeSlot(key);
        Page* page = head;
        while (true)
        {
            for (size_t j = 0; j < S; ++j)
            {
                auto& slot = page->slots[(home + j) % S];
                State* expected = nullptr;
                if (slot.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    memory.adopt(std::move(block));
                    return {fresh, true};
                }
                if (sameKey(expected->key(), key))
                {
                    fresh->destroy();   // lost the race for this frame; the unadopted block frees itself
                    return {expected, false};
                }
            }
            page = nextPage(page);
        }
    }

    /// Calls fn for every frame; concurrent creations may be missed
    template <typename Fn>
    void forEach(Fn&& fn) const
    {
        for (const Page* page = head; page != nullptr; page = page->next.load(std::memory_order_acquire))
        {
            for (const auto& slot : page->slots)
            {
                if (State* s = slot.load(std::memory_order_acquire))
                {
                    fn(*s);
                }
            }
        }
    }

    [[nodiscard]] size_t size() const
    {
        size_t n = 0;
        forEach([&](State&) { ++n; });
        return n;
    }

    [[nodiscard]] size_t pageCount() const
    {
        size_t n = 0;
        for (const Page* page = head; page != nullptr; page = page->next.load(std::memory_order_acquire))
        {
            ++n;
        }
        return n;
    }

    /// Destroys every frame state; the memory stays with the Memory owner
    void destroyAll()
    {
        forEach([](State& s) { s.destroy(); });
    }

private:
    struct Page
    {
        std::atomic<Page*> next{nullptr};
        std::array<std::atomic<State*>, S> slots{};
    };

    template <typename Key>
    static size_t homeSlot(const Key& key)
    {
        /// Consecutive frames of one origin land in consecutive slots; origins are spread by a Fibonacci multiplier.
        return (static_cast<uint64_t>(key.frameId) + (static_cast<uint64_t>(key.originIdx) * 0x9E3779B1ULL)) % S;
    }

    template <typename A, typename B>
    static bool sameKey(const A& a, const B& b)
    {
        return a.originIdx == b.originIdx && a.frameId == b.frameId;
    }

    Page* nextPage(Page* page)
    {
        Page* next = page->next.load(std::memory_order_acquire);
        if (next != nullptr)
        {
            return next;
        }
        auto block = memory.allocate(sizeof(Page));
        Page* candidate = new (block.bytes().data()) Page();
        if (page->next.compare_exchange_strong(next, candidate, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            memory.adopt(std::move(block));
            return candidate;
        }
        candidate->~Page();   // another thread appended first; drop ours
        return next;
    }

    Memory& memory;
    Page* head = nullptr;
};

}