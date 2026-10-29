#pragma once

#include <cstdint>
#include <vector>

#include <DataTypes/VarVal.hpp>
#include <Functions/PhysicalFunction.hpp>
#include <Interface/Record.hpp>
#include <Arena.hpp>

namespace NES
{

/// BANDS on one binary maplet: preamble kept (bands = k), planes copied, nothing decoded
class BandsPhysicalFunction final
{
public:
    BandsPhysicalFunction(PhysicalFunction raster, std::vector<PhysicalFunction> bandIndices);
    [[nodiscard]] VarVal execute(const Record& record, ArenaRef& arena) const;

private:
    PhysicalFunction raster;
    std::vector<PhysicalFunction> bandIndices;
};

static_assert(PhysicalFunctionConcept<BandsPhysicalFunction>);

/// Native routines behind nautilus::invoke, public for unit tests
namespace BandsKernels
{
/// Validates the input, returns the output size
uint64_t outputSize(int8_t* input, uint64_t inputSize, uint32_t selectedCount);
/// Writes the output preamble: the input's, with the band count replaced by `selectedCount`.
void writePreamble(int8_t* input, uint64_t inputSize, int8_t* output, uint64_t outputSize, uint32_t selectedCount);
/// Copies band `band` of the input into band slot `slot` of the output.
void copyPlane(int8_t* input, uint64_t inputSize, int8_t* output, uint64_t outputSize, uint32_t slot, uint32_t band);
}

}