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

#include <BandsLogicalFunction.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DataTypes/DataType.hpp>
#include <DataTypes/DataTypeProvider.hpp>
#include <Functions/ConstantValueLogicalFunction.hpp>
#include <Functions/FieldAccessLogicalFunction.hpp>
#include <Functions/LogicalFunction.hpp>
#include <Serialization/LogicalFunctionReflection.hpp>
#include <Serialization/ReflectedOperator.hpp>
#include <Util/PlanRenderer.hpp>
#include <Util/Reflection.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <ErrorHandling.hpp>
#include <LogicalFunctionRegistry.hpp>

namespace NES
{
namespace
{
/// A maplet has at most 255 bands, so a valid index is 0..254
constexpr int64_t MAX_BAND_INDEX = 254;

uint8_t parseBandIndex(const LogicalFunction& argument, size_t position)
{
    const auto constant = argument.tryGetAs<ConstantValueLogicalFunction>();
    if (not constant.has_value())
    {
        throw InvalidQuerySyntax("BANDS: argument {} must be an integer constant band index, got {}", position, argument);
    }
    if (not constant.value()->getDataType().isInteger())
    {
        throw InvalidQuerySyntax(
            "BANDS: argument {} must be an integer band index, got {} of type {}", position, argument, constant.value()->getDataType());
    }
    const std::string text = constant.value()->getConstantValue();
    int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} or end != text.data() + text.size() or value < 0 or value > MAX_BAND_INDEX)
    {
        throw InvalidQuerySyntax("BANDS: band index {} (argument {}) must be in 0..{}", text, position, MAX_BAND_INDEX);
    }
    return static_cast<uint8_t>(value);
}

std::vector<uint8_t> parseBandIndices(const std::vector<LogicalFunction>& indexArguments)
{
    if (indexArguments.empty())
    {
        throw InvalidQuerySyntax("BANDS needs at least one band index: BANDS(raster, i1, ..., ik)");
    }
    std::vector<uint8_t> bands;
    bands.reserve(indexArguments.size());
    for (size_t i = 0; i < indexArguments.size(); ++i)
    {
        const uint8_t band = parseBandIndex(indexArguments[i], i + 1);
        if (std::ranges::find(bands, band) != bands.end())
        {
            throw InvalidQuerySyntax("BANDS: band index {} is selected twice", band);
        }
        bands.push_back(band);
    }
    return bands;
}

void checkAgainstNestedBands(const LogicalFunction& raster, const std::vector<uint8_t>& selectedBands)
{
    if (const auto known = BandsLogicalFunction::staticBandCount(raster))
    {
        for (const uint8_t band : selectedBands)
        {
            if (band >= *known)
            {
                throw InvalidQuerySyntax("BANDS: band index {} does not exist in {}, which has {} band(s)", band, raster, *known);
            }
        }
    }
}
}

BandsLogicalFunction::BandsLogicalFunction(std::vector<LogicalFunction> children)
    : dataType(DataTypeProvider::provideDataType(DataType::Type::UNDEFINED))
{
    if (children.size() < 2)
    {
        throw InvalidQuerySyntax("BANDS(raster, i1, ..., ik) needs a raster and at least one band index, got {} argument(s)", children.size());
    }
    raster = children.front();
    indexArguments.assign(children.begin() + 1, children.end());
    selectedBands = parseBandIndices(indexArguments);
    checkAgainstNestedBands(raster, selectedBands);
}

bool BandsLogicalFunction::operator==(const BandsLogicalFunction& rhs) const
{
    return raster == rhs.raster and selectedBands == rhs.selectedBands;
}

DataType BandsLogicalFunction::getDataType() const
{
    return dataType;
}

LogicalFunction BandsLogicalFunction::withInferredDataType(const Schema<Field, Unordered>& schema) const
{
    auto copy = *this;
    copy.raster = raster.withInferredDataType(schema);
    copy.indexArguments.clear();
    for (const auto& argument : indexArguments)
    {
        copy.indexArguments.push_back(argument.withInferredDataType(schema));
    }

    const bool isField = copy.raster.tryGetAs<FieldAccessLogicalFunction>().has_value();
    const bool isBands = copy.raster.tryGetAs<BandsLogicalFunction>().has_value();
    if (not isField and not isBands)
    {
        throw InvalidQuerySyntax("BANDS: the first argument must be a raster field or BANDS(...), got {}", copy.raster);
    }
    if (not copy.raster.getDataType().isType(DataType::Type::VARSIZED))
    {
        throw DifferentFieldTypeExpected("BANDS: the first argument must be VARSIZED, got {} of type {}", copy.raster, copy.raster.getDataType());
    }

    copy.dataType = DataTypeProvider::provideDataType(DataType::Type::VARSIZED);
    copy.dataType.nullable = copy.raster.getDataType().nullable;
    return copy;
}

std::vector<LogicalFunction> BandsLogicalFunction::getChildren() const
{
    std::vector<LogicalFunction> children{raster};
    children.insert(children.end(), indexArguments.begin(), indexArguments.end());
    return children;
}

BandsLogicalFunction BandsLogicalFunction::withChildren(const std::vector<LogicalFunction>& children) const
{
    auto copy = BandsLogicalFunction(children);
    copy.dataType = dataType;
    return copy;
}

std::string_view BandsLogicalFunction::getType() const
{
    return NAME;
}

std::string BandsLogicalFunction::explain(ExplainVerbosity verbosity) const
{
    return fmt::format("BANDS({}, {})", raster.explain(verbosity), fmt::join(selectedBands, ", "));
}

const std::vector<uint8_t>& BandsLogicalFunction::getSelectedBands() const
{
    return selectedBands;
}

uint32_t BandsLogicalFunction::getBandCount() const
{
    return static_cast<uint32_t>(selectedBands.size());
}

std::optional<uint32_t> BandsLogicalFunction::staticBandCount(const LogicalFunction& raster)
{
    if (const auto bands = raster.tryGetAs<BandsLogicalFunction>())
    {
        return bands.value()->getBandCount();
    }
    return std::nullopt;
}

Reflected Reflector<BandsLogicalFunction>::operator()(const BandsLogicalFunction& function) const
{
    return reflect(detail::ReflectedBandsLogicalFunction{.children = function.getChildren()});
}

BandsLogicalFunction Unreflector<BandsLogicalFunction>::operator()(const Reflected& reflected, const ReflectionContext& context) const
{
    auto [children] = context.unreflect<detail::ReflectedBandsLogicalFunction>(reflected);
    return BandsLogicalFunction(std::move(children));
}

LogicalFunctionRegistryReturnType LogicalFunctionGeneratedRegistrar::RegisterBandsLogicalFunction(LogicalFunctionRegistryArguments arguments)
{
    return BandsLogicalFunction(std::move(arguments.children));
}

}