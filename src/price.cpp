#include "exchangecore/price.hpp"
#include <array>
#include <cstdint>

namespace exchangecore {
std::string format_price(Price ticks) {
    const bool negative = ticks < 0;
    const auto magnitude = negative
        ? static_cast<std::uint64_t>(-(ticks + 1)) + 1
        : static_cast<std::uint64_t>(ticks);
    constexpr std::array suffix{".00", ".25", ".50", ".75"};
    return (negative ? "-" : "") + std::to_string(magnitude / 4) + suffix[magnitude % 4];
}
} // namespace exchangecore
