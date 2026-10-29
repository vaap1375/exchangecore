#pragma once

#include "exchangecore/order.hpp"
#include <string>

namespace exchangecore {
// Synthetic ES display convention: one tick = 0.25 points. No floating point.
[[nodiscard]] std::string format_price(Price ticks);
} // namespace exchangecore
