#include <Raster/FrameTableBuffers.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <ErrorHandling.hpp>

namespace NES
{
namespace
{
TupleBuffer unpooled(AbstractBufferProvider& provider, size_t bytes)
{
    auto buffer = provider.getUnpooledBuffer(bytes);
    if (!buffer)
    {
        throw CannotAllocateBuffer("frame table: no unpooled buffer of {} bytes", bytes);
    }
    return std::move(*buffer);
}
}

std::span<std::byte> FrameTableBuffers::Pending::bytes()
{
    return buffer.getAvailableMemoryArea<std::byte>();
}

FrameTableBuffers::FrameTableBuffers(std::shared_ptr<AbstractBufferProvider> provider)
    : provider(std::move(provider)), root(unpooled(*this->provider, 1))
{
}

FrameTableBuffers::Pending FrameTableBuffers::allocate(size_t bytes)
{
    return Pending{unpooled(*provider, bytes)};
}

void FrameTableBuffers::adopt(Pending&& pending)
{
    const std::lock_guard lock(mutex);
    (void)root.storeChildBuffer(pending.buffer);
}

}