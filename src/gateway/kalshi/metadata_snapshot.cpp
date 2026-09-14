#include "eme/gateway/kalshi/metadata_snapshot.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace eme::gateway::kalshi {
namespace {

using Json = nlohmann::json;

[[noreturn]] void reject(const MetadataErrorCode code, const std::string_view field) {
    throw MetadataError{code, std::string{field}};
}

void shape(const Json& object, const std::initializer_list<std::string_view> fields,
           const std::string_view path) {
    if (!object.is_object() || object.size() != fields.size()) {
        reject(MetadataErrorCode::invalid_shape, path);
    }
    for (const auto field : fields) {
        if (!object.contains(field)) {
            reject(MetadataErrorCode::invalid_shape, path);
        }
    }
}

[[nodiscard]] std::uint64_t positive_integer(
    const Json& value, const std::uint64_t maximum, const std::string_view path) {
    if (!value.is_number_unsigned()) {
        reject(MetadataErrorCode::invalid_value, path);
    }
    const auto number = value.get<std::uint64_t>();
    if (number == 0U || number > maximum) {
        reject(MetadataErrorCode::invalid_value, path);
    }
    return number;
}

[[nodiscard]] std::uint32_t id(const Json& value, const std::string_view path) {
    return static_cast<std::uint32_t>(positive_integer(
        value, std::numeric_limits<std::uint32_t>::max(), path));
}

[[nodiscard]] std::string text(
    const Json& value, const std::size_t maximum, const std::string_view path) {
    if (!value.is_string()) {
        reject(MetadataErrorCode::invalid_value, path);
    }
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > maximum ||
        std::all_of(result.begin(), result.end(), [](const char ch) { return ch == ' '; }) ||
        std::any_of(result.begin(), result.end(), [](const char ch) {
            const auto byte = static_cast<unsigned char>(ch);
            return byte < 0x20U || byte == 0x7fU;
        })) {
        reject(MetadataErrorCode::invalid_value, path);
    }
    return result;
}

void array(const Json& value, const std::size_t maximum, const std::string_view path) {
    if (!value.is_array()) {
        reject(MetadataErrorCode::invalid_shape, path);
    }
    if (value.size() > maximum) {
        reject(MetadataErrorCode::invalid_value, path);
    }
}

}  // namespace

MetadataResult parse_metadata_snapshot(const std::string_view input) {
    if (input.size() > maximum_metadata_bytes) {
        return MetadataError{MetadataErrorCode::input_too_large, "$"};
    }
    try {
        // Reject ambiguous keys before the DOM can overwrite their first value.
        std::vector<std::unordered_set<std::string>> object_keys;
        auto root = Json::parse(input, [&object_keys](
            const int depth, const Json::parse_event_t event, Json& parsed) {
            if (depth > 8) {
                reject(MetadataErrorCode::nesting_too_deep, "$");
            }
            if (event == Json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == Json::parse_event_t::object_end) {
                object_keys.pop_back();
            } else if (event == Json::parse_event_t::key) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!object_keys.back().insert(key).second) {
                    reject(MetadataErrorCode::duplicate_field, "$");
                }
            }
            return true;
        });
        shape(root, {"schema_version", "metadata_version", "venue", "markets", "constraints"}, "$");
        const auto schema = positive_integer(root["schema_version"],
            std::numeric_limits<std::uint64_t>::max(), "schema_version");
        if (schema != 1U) {
            reject(MetadataErrorCode::unsupported_version, "schema_version");
        }
        const auto version = positive_integer(root["metadata_version"],
            std::numeric_limits<std::uint64_t>::max(), "metadata_version");
        if (text(root["venue"], 32U, "venue") != "kalshi") {
            reject(MetadataErrorCode::invalid_value, "venue");
        }

        auto& markets = root["markets"];
        auto& definitions = root["constraints"];
        array(markets, 10'000U, "markets");
        array(definitions, 50'000U, "constraints");
        if (markets.empty()) {
            reject(MetadataErrorCode::invalid_value, "markets");
        }

        MetadataSnapshot snapshot{version};
        for (const auto& entry : markets) {
            shape(entry, {"id", "ticker"}, "markets[]");
            const auto market_id = id(entry["id"], "markets[].id");
            auto ticker = text(entry["ticker"], 128U, "markets[].ticker");
            if (snapshot.markets_.register_market(market_id, std::move(ticker)) !=
                MarketRegistrationResult::registered) {
                reject(MetadataErrorCode::duplicate_market, "markets[]");
            }
        }

        std::vector<constraint::ConstraintDefinition> validated;
        validated.reserve(definitions.size());
        std::unordered_set<constraint::ConstraintId> seen_ids;
        std::unordered_set<std::string> seen_keys;
        for (const auto& entry : definitions) {
            shape(entry, {"id", "semantic_version", "key", "provenance", "relationship"}, "constraints[]");
            constraint::ConstraintDefinition definition;
            definition.id = id(entry["id"], "constraints[].id");
            definition.semantic_version = id(entry["semantic_version"], "constraints[].semantic_version");
            definition.key = text(entry["key"], 256U, "constraints[].key");
            definition.provenance = text(entry["provenance"], 4096U, "constraints[].provenance");
            if (!seen_ids.insert(definition.id).second || !seen_keys.insert(definition.key).second) {
                reject(MetadataErrorCode::duplicate_constraint, "constraints[]");
            }
            const auto& relation = entry["relationship"];
            if (!relation.is_object() || !relation.contains("type")) {
                reject(MetadataErrorCode::invalid_shape, "constraints[].relationship");
            }
            const auto type = text(relation["type"], 32U, "constraints[].relationship.type");
            std::uint32_t first{};
            std::uint32_t second{};
            if (type == "implication") {
                shape(relation, {"type", "antecedent", "consequent"}, "constraints[].relationship");
                first = id(relation["antecedent"], "relationship.antecedent");
                second = id(relation["consequent"], "relationship.consequent");
                definition.relationship = constraint::Implication{first, second};
            } else if (type == "complement") {
                shape(relation, {"type", "left", "right"}, "constraints[].relationship");
                first = id(relation["left"], "relationship.left");
                second = id(relation["right"], "relationship.right");
                definition.relationship = constraint::Complement{first, second};
            } else {
                reject(MetadataErrorCode::invalid_value, "constraints[].relationship.type");
            }
            if (first == second) {
                reject(MetadataErrorCode::invalid_value, "constraints[].relationship");
            }
            if (!snapshot.markets_.find(first) || !snapshot.markets_.find(second)) {
                reject(MetadataErrorCode::unknown_market, "constraints[].relationship");
            }
            validated.push_back(std::move(definition));
        }

        // Stable dependency iteration, independent of input/registration order.
        std::sort(validated.begin(), validated.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        for (auto& definition : validated) {
            if (snapshot.constraints_.add(std::move(definition)) !=
                constraint::ConstraintRegistrationResult::registered) {
                reject(MetadataErrorCode::invalid_value, "constraints[]");
            }
        }
        const auto by_id = [](const Json& left, const Json& right) {
            return left["id"].get<std::uint32_t>() < right["id"].get<std::uint32_t>();
        };
        std::sort(markets.begin(), markets.end(), by_id);
        std::sort(definitions.begin(), definitions.end(), by_id);
        snapshot.canonical_ = root.dump();
        return snapshot;
    } catch (const MetadataError& error) {
        return error;
    } catch (const Json::exception&) {
        return MetadataError{MetadataErrorCode::invalid_json, "$"};
    }
}

std::string_view to_string(const MetadataErrorCode code) noexcept {
    switch (code) {
    case MetadataErrorCode::invalid_json: return "invalid_json";
    case MetadataErrorCode::duplicate_field: return "duplicate_field";
    case MetadataErrorCode::invalid_shape: return "invalid_shape";
    case MetadataErrorCode::invalid_value: return "invalid_value";
    case MetadataErrorCode::unsupported_version: return "unsupported_version";
    case MetadataErrorCode::duplicate_market: return "duplicate_market";
    case MetadataErrorCode::duplicate_constraint: return "duplicate_constraint";
    case MetadataErrorCode::unknown_market: return "unknown_market";
    case MetadataErrorCode::input_too_large: return "input_too_large";
    case MetadataErrorCode::nesting_too_deep: return "nesting_too_deep";
    }
    return "unknown_metadata_error";
}

}  // namespace eme::gateway::kalshi
