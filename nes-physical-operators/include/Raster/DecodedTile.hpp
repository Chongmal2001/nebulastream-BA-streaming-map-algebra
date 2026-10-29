#pragma once

#include <utility>

#include <Runtime/TupleBuffer.hpp>
#include <MapletCodec.hpp>

namespace NES
{

/// Decoded tile in NES memory: cell buffer plus a view into it; empty when default-constructed
struct DecodedTile
{
    TupleBuffer cells;
    Raster::GridView view;

    [[nodiscard]] bool empty() const { return view.cells == nullptr; }
};

}