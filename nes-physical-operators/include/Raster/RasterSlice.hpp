#pragma once

#include <atomic>
#include <memory>
#include <utility>

#include <Runtime/AbstractBufferProvider.hpp>
#include <SliceStore/Slice.hpp>

#include <Raster/FrameTable.hpp>
#include <Raster/FrameTableBuffers.hpp>

/// Slice of a raster operator: frame table plus the NES buffers holding it
namespace NES
{

template <typename State>
class RasterSlice final : public Slice
{
public:
    using Frames = FrameTable<State, FrameTableBuffers>;

    RasterSlice(SliceStart start, SliceEnd end, std::shared_ptr<AbstractBufferProvider> provider)
        : Slice(start, end), buffers(std::make_unique<FrameTableBuffers>(std::move(provider))), frames(std::make_unique<Frames>(*buffers))
    {
    }

    RasterSlice(const RasterSlice&) = delete;
    RasterSlice& operator=(const RasterSlice&) = delete;

    /// Destroys every frame state (releasing tiles still stored), then the buffers that held them.
    ~RasterSlice() override
    {
        frames->destroyAll();
        frames.reset();
        buffers.reset();
    }

    [[nodiscard]] Frames& frameTable() { return *frames; }
    [[nodiscard]] const Frames& frameTable() const { return *frames; }

    /// Set by the first trigger covering the slice: deadline of all its frames
    void markDeadlinePassed() { deadline.store(true, std::memory_order_release); }
    [[nodiscard]] bool deadlinePassed() const { return deadline.load(std::memory_order_acquire); }

private:
    std::atomic<bool> deadline{false};
    std::unique_ptr<FrameTableBuffers> buffers;   // declared before `frames`: the table lives in these buffers
    std::unique_ptr<Frames> frames;
};

}