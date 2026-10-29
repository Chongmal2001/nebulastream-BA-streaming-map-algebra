#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

/// Heap Memory policy for FrameTable tests; counts blocks to detect leaks
namespace NES::Testing
{

class HeapMemory
{
public:
    static constexpr std::align_val_t Alignment{64};

    class Block
    {
    public:
        Block(std::byte* data, size_t size, std::atomic<size_t>& live) : data(data), size(size), live(&live) { }
        Block(Block&& other) noexcept
            : data(std::exchange(other.data, nullptr)), size(std::exchange(other.size, 0)), live(std::exchange(other.live, nullptr))
        {
        }
        Block& operator=(Block&&) = delete;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block()
        {
            if (data != nullptr)
            {
                ::operator delete(data, Alignment);
                live->fetch_sub(1, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] std::span<std::byte> bytes() { return {data, size}; }

    private:
        std::byte* data;
        size_t size;
        std::atomic<size_t>* live;
    };

    HeapMemory() = default;
    HeapMemory(const HeapMemory&) = delete;
    HeapMemory& operator=(const HeapMemory&) = delete;

    [[nodiscard]] Block allocate(size_t bytes)
    {
        auto* data = static_cast<std::byte*>(::operator new(bytes, Alignment));
        liveBlocks.fetch_add(1, std::memory_order_relaxed);
        allocations.fetch_add(1, std::memory_order_relaxed);
        return Block{data, bytes, liveBlocks};
    }

    void adopt(Block&& block)
    {
        const std::lock_guard lock(mutex);
        owned.push_back(std::make_unique<Block>(std::move(block)));
    }

    /// Blocks currently allocated (adopted or still pending).
    [[nodiscard]] size_t live() const { return liveBlocks.load(std::memory_order_relaxed); }
    /// Blocks ever allocated (including the ones dropped after a lost race).
    [[nodiscard]] size_t allocated() const { return allocations.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t adopted()
    {
        const std::lock_guard lock(mutex);
        return owned.size();
    }

private:
    std::atomic<size_t> liveBlocks{0};
    std::atomic<size_t> allocations{0};
    std::mutex mutex;
    std::vector<std::unique_ptr<Block>> owned;
};

}
