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

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <DataTypes/DataType.hpp>
#include <DataTypes/DataTypeProvider.hpp>
#include <Functions/ConstantValueLogicalFunction.hpp>
#include <Functions/LogicalFunction.hpp>
#include <BandsLogicalFunction.hpp>

using namespace NES;

namespace
{
/// Raster stand-in; the VARSIZED rule is covered by the systest
LogicalFunction raster()
{
    return ConstantValueLogicalFunction(DataTypeProvider::provideDataType(DataType::Type::VARSIZED), "maplet");
}

LogicalFunction index(const std::string& value, DataType::Type type = DataType::Type::INT32)
{
    return ConstantValueLogicalFunction(DataTypeProvider::provideDataType(type), value);
}

BandsLogicalFunction bands(std::vector<LogicalFunction> indices, LogicalFunction input = raster())
{
    std::vector<LogicalFunction> children{std::move(input)};
    children.insert(children.end(), indices.begin(), indices.end());
    return BandsLogicalFunction(std::move(children));
}
}

TEST(BandsLogicalFunctionTest, ParsesIndicesInOrder)
{
    const auto f = bands({index("3"), index("0"), index("2")});
    EXPECT_EQ(f.getSelectedBands(), (std::vector<uint8_t>{3, 0, 2}));
    EXPECT_EQ(f.getBandCount(), 3U);
    EXPECT_EQ(f.getChildren().size(), 4U);
    EXPECT_EQ(f.getType(), "Bands");
}

TEST(BandsLogicalFunctionTest, AcceptsAnyIntegerConstantType)
{
    EXPECT_EQ(bands({index("1", DataType::Type::UINT8)}).getSelectedBands(), (std::vector<uint8_t>{1}));
    EXPECT_EQ(bands({index("254", DataType::Type::UINT64)}).getSelectedBands(), (std::vector<uint8_t>{254}));
}

TEST(BandsLogicalFunctionTest, RejectsBadArguments)
{
    EXPECT_ANY_THROW(BandsLogicalFunction(std::vector<LogicalFunction>{raster()}));     // no index
    EXPECT_ANY_THROW(bands({index("-1")}));                                               // negative
    EXPECT_ANY_THROW(bands({index("255")}));                                              // a maplet has at most 255 bands
    EXPECT_ANY_THROW(bands({index("1"), index("1")}));                                    // repeated
    EXPECT_ANY_THROW(bands({index("1.0", DataType::Type::FLOAT64)}));                    // not an integer
    EXPECT_ANY_THROW(bands({raster()}));                                                  // not an integer constant
}

TEST(BandsLogicalFunctionTest, NestedCallsAreCheckedStatically)
{
    const LogicalFunction inner = bands({index("1"), index("2"), index("3")});
    EXPECT_EQ(BandsLogicalFunction::staticBandCount(inner), 3U);
    EXPECT_EQ(BandsLogicalFunction::staticBandCount(raster()), std::nullopt);

    const auto outer = bands({index("0")}, inner);   // BANDS(BANDS(x, 1, 2, 3), 0) = source band 1
    EXPECT_EQ(outer.getSelectedBands(), (std::vector<uint8_t>{0}));
    EXPECT_ANY_THROW(bands({index("3")}, inner));     // the inner call has only bands 0..2
}

TEST(BandsLogicalFunctionTest, WithChildrenReparsesAndEqualityUsesSelection)
{
    const auto f = bands({index("1"), index("0")});
    const auto g = f.withChildren(f.getChildren());
    EXPECT_EQ(f, g);
    EXPECT_FALSE(f == bands({index("0"), index("1")}));   // order matters

}