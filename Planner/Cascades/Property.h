#pragma once

#include <vector>
#include <memory>

namespace Database
{
    // A placeholder for physical properties required by a cost-based optimizer (e.g. Sort Order)
    class PhysicalProperties
    {
    public:
        PhysicalProperties() = default;
        // In the future: std::vector<std::string> sort_columns_; etc.
        bool operator==(const PhysicalProperties &other) const { return true; }
        bool operator!=(const PhysicalProperties &other) const { return false; }
    };

    // Forward declarations
    class AbstractPlanNode;
    class AbstractLogicalPlanNode;

} // namespace Database