#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>

#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/TupleBuffer.hpp>

namespace NES
{

/// NES buffers backing one slice's frame table (Memory policy); adopt() is the only locked step
class FrameTableBuffers
{
public:
    /// A buffer obtained but not yet owned by the slice.
    struct Pending
    {
        TupleBuffer buffer;
        [[nodiscard]] std::span<std::byte> bytes();
    };

    explicit FrameTableBuffers(std::shared_ptr<AbstractBufferProvider> provider);

    FrameTableBuffers(const FrameTableBuffers&) = delete;
    FrameTableBuffers& operator=(const FrameTableBuffers&) = delete;

    /// An unpooled buffer of at least `bytes` bytes. Throws CannotAllocateBuffer if the provider has none.
    [[nodiscard]] Pending allocate(size_t bytes);

    /// Makes the slice the owner of `pending`: it stays alive until this object is destroyed.
    void adopt(Pending&& pending);

private:
    std::shared_ptr<AbstractBufferProvider> provider;
    TupleBuffer root;   // owner of every adopted buffer, through its child list
    std::mutex mutex;   // guards root's child list (storeChildBuffer is not thread-safe)
};

}