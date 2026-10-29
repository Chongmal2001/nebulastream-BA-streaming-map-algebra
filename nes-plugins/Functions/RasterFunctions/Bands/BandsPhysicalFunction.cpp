/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        https://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <BandsPhysicalFunction.hpp>

#include <cstdint>
#include <utility>
#include <vector>

#include <DataTypes/VarVal.hpp>
#include <DataTypes/VariableSizedData.hpp>
#include <Functions/PhysicalFunction.hpp>
#include <Interface/Record.hpp>
#include <nautilus/function.hpp>
#include <Arena.hpp>
#include <ErrorHandling.hpp>
#include <MapletCodec.hpp>
#include <PhysicalFunctionRegistry.hpp>

namespace NES
{
namespace
{
void throwIfFailed(Raster::CodecStatus status, const char* step, uint64_t inputSize)
{
    if (status != Raster::CodecStatus::Ok)
    {
        throw FormattingError("BANDS: {} failed on a {}-byte maplet: {}", step, inputSize, Raster::toString(status));
    }
}
}

/// Thin wrappers: codec status -> query error
namespace BandsKernels
{
uint64_t outputSize(int8_t* input, uint64_t inputSize, uint32_t selectedCount)
{
    Raster::MapletHeader header;
    throwIfFailed(Raster::peekHeader(input, inputSize, header), "reading the input preamble", inputSize);
    return Raster::selectedSize(header, selectedCount);
}

void writePreamble(int8_t* input, uint64_t inputSize, int8_t* output, uint64_t outputSize, uint32_t selectedCount)
{
    throwIfFailed(Raster::beginBandSelection(input, inputSize, selectedCount, output, outputSize), "writing the preamble", inputSize);
}

void copyPlane(int8_t* input, uint64_t inputSize, int8_t* output, uint64_t outputSize, uint32_t slot, uint32_t band)
{
    const auto status = Raster::copyBand(input, inputSize, band, output, outputSize, slot);
    if (status == Raster::CodecStatus::BandOutOfRange)
    {
        throw FormattingError("BANDS: band {} does not exist in this maplet", band);
    }
    throwIfFailed(status, "copying a band", inputSize);
}
}

BandsPhysicalFunction::BandsPhysicalFunction(PhysicalFunction raster, std::vector<PhysicalFunction> bandIndices)
    : raster(std::move(raster)), bandIndices(std::move(bandIndices))
{
}

VarVal BandsPhysicalFunction::execute(const Record& record, ArenaRef& arena) const
{
    const auto input = raster.execute(record, arena).getRawValueAs<VariableSizedData>();
    const nautilus::val<uint32_t> selectedCount(static_cast<uint32_t>(bandIndices.size()));

    const auto size = nautilus::invoke(BandsKernels::outputSize, input.getContent(), input.getSize(), selectedCount);
    auto output = arena.allocateVariableSizedData(size);
    nautilus::invoke(BandsKernels::writePreamble, input.getContent(), input.getSize(), output.getContent(), size, selectedCount);

    /// Unrolled at trace time, one copy per index
    for (uint32_t slot = 0; slot < bandIndices.size(); ++slot)
    {
        const auto band = bandIndices[slot].execute(record, arena).getRawValueAs<nautilus::val<uint32_t>>();
        nautilus::invoke(
            BandsKernels::copyPlane, input.getContent(), input.getSize(), output.getContent(), size, nautilus::val<uint32_t>(slot), band);
    }
    return output;
}

PhysicalFunctionRegistryReturnType
PhysicalFunctionGeneratedRegistrar::RegisterBandsPhysicalFunction(PhysicalFunctionRegistryArguments physicalFunctionRegistryArguments)
{
    auto& children = physicalFunctionRegistryArguments.childFunctions;
    PRECONDITION(children.size() >= 2, "BANDS needs a raster and at least one band index, got {} child function(s)", children.size());
    std::vector<PhysicalFunction> indices(children.begin() + 1, children.end());
    return BandsPhysicalFunction(children.front(), std::move(indices));
}

}