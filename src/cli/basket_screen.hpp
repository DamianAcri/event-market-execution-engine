#pragma once

#include <string>
#include <string_view>
#include <variant>

namespace eme::cli {

struct BasketScreenError final { std::string field; };
using BasketScreenResult = std::variant<std::string, BasketScreenError>;

// Offline conditional arithmetic for previously qualified threshold/range
// relationships. This does not certify settlement rules or simulate execution.
[[nodiscard]] BasketScreenResult screen_baskets(std::string_view input);
[[nodiscard]] int run_basket_screen_command(int argc, const char* const argv[]);

}  // namespace eme::cli
