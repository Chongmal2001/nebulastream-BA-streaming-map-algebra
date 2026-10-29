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

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <DataTypes/DataType.hpp>
#include <Functions/LogicalFunction.hpp>
#include <Schema/Field.hpp>
#include <Schema/Binder.hpp>
#include <Util/Logger/Formatter.hpp>
#include <Util/PlanRenderer.hpp>
#include <Util/ReflectionFwd.hpp>

namespace NES
{

/// BANDS(raster, i1..ik): bands i1..ik of raster, in that order; indices checked at planning time
class BandsLogicalFunction final
{
public:
    static constexpr std::string_view NAME = "Bands";

    explicit BandsLogicalFunction(std::vector<LogicalFunction> children);

    [[nodiscard]] bool operator==(const BandsLogicalFunction& rhs) const;

    [[nodiscard]] DataType getDataType() const;
    [[nodiscard]] LogicalFunction withInferredDataType(const Schema<Field, Unordered>& schema) const;

    [[nodiscard]] std::vector<LogicalFunction> getChildren() const;
    [[nodiscard]] BandsLogicalFunction withChildren(const std::vector<LogicalFunction>& children) const;

    [[nodiscard]] std::string_view getType() const;
    [[nodiscard]] std::string explain(ExplainVerbosity verbosity) const;

    /// The selected source bands, in output order.
    [[nodiscard]] const std::vector<uint8_t>& getSelectedBands() const;
    /// Number of bands of the output maplet (= number of indices).
    [[nodiscard]] uint32_t getBandCount() const;

    /// Band count of raster if known statically (nested BANDS)
    [[nodiscard]] static std::optional<uint32_t> staticBandCount(const LogicalFunction& raster);

private:
    DataType dataType;
    LogicalFunction raster;
    std::vector<LogicalFunction> indexArguments;
    std::vector<uint8_t> selectedBands;

    friend Reflector<BandsLogicalFunction>;
};

namespace detail
{
struct ReflectedBandsLogicalFunction
{
    std::vector<LogicalFunction> children;
};
}

template <>
struct Unreflector<BandsLogicalFunction>
{
    BandsLogicalFunction operator()(const Reflected& reflected, const ReflectionContext& context) const;
};

template <>
struct Reflector<BandsLogicalFunction>
{
    Reflected operator()(const BandsLogicalFunction& function) const;
};

static_assert(LogicalFunctionConcept<BandsLogicalFunction>);

}

FMT_OSTREAM(NES::BandsLogicalFunction);