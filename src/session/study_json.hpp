#pragma once

#include "eme/session/replay.hpp"
#include "session_files.hpp"

#include <nlohmann/json.hpp>
#include <limits>
#include <unordered_set>

namespace eme::session::detail {
using Json = nlohmann::json;
[[noreturn]] inline void invalid(const std::string& field) { throw ReplayError{field, 0U}; }
inline void shape(const Json& value, const std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) { invalid("object shape"); }
    for (const auto field : fields) { if (!value.contains(field)) { invalid(std::string{field}); } }
}
inline std::uint64_t integer(const Json& value, const std::string_view field,
                             const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    const auto& item = value.at(field);
    if (!item.is_number_unsigned() || item.get<std::uint64_t>() > maximum) { invalid(std::string{field}); }
    return item.get<std::uint64_t>();
}
inline std::string string(const Json& value, const std::string_view field) {
    const auto& item = value.at(field);
    if (!item.is_string() || item.get_ref<const std::string&>().empty() ||
        item.get_ref<const std::string&>().size() > 4096U) { invalid(std::string{field}); }
    return item.get<std::string>();
}
inline std::string read_text(const std::filesystem::path& path, const std::size_t limit = 4U * 1024U * 1024U) {
    auto result = read_bounded(path, limit);
    if (const auto* failure = std::get_if<SessionError>(&result)) { invalid(std::string{to_string(failure->code)} + ": " + failure->field); }
    return std::get<std::string>(std::move(result));
}
inline Json parse_strict(const std::string_view bytes) {
    std::vector<std::unordered_set<std::string>> keys;
    return Json::parse(bytes, [&keys](const int depth, const Json::parse_event_t event, Json& value) {
        if (depth > 12) { invalid("JSON depth"); }
        if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
        else if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) {
            invalid("duplicate field");
        }
        return true;
    });
}
}  // namespace eme::session::detail
