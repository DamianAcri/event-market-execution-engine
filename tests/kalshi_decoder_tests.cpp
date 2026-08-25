#include "eme/book/order_book.hpp"
#include "eme/gateway/kalshi/market_registry.hpp"
#include "eme/gateway/kalshi/orderbook_decoder.hpp"
#include "eme/gateway/kalshi/orderbook_normalizer.hpp"
#include "test_support.hpp"

#include <string_view>
#include <variant>

namespace {

namespace kalshi = eme::gateway::kalshi;

void test_market_registry(eme::test::Context& test) {
    kalshi::MarketRegistry unversioned{0U};
    test.expect(unversioned.register_market(1U, "X") ==
                    kalshi::MarketRegistrationResult::invalid_metadata_version,
                "market mappings require a nonzero metadata version");
    kalshi::MarketRegistry markets{11U};
    test.expect(markets.register_market(1U, "") ==
                    kalshi::MarketRegistrationResult::invalid_ticker &&
                    markets.register_market(0U, "X") ==
                        kalshi::MarketRegistrationResult::invalid_market_id,
                "empty ticker cannot be registered");
    const auto first = markets.register_market(42U, "FED-23DEC-T3.00");
    const auto duplicate = markets.register_market(42U, "FED-23DEC-T3.00");
    const auto second = markets.register_market(7U, "X");
    test.expect(first == kalshi::MarketRegistrationResult::registered &&
                    duplicate == kalshi::MarketRegistrationResult::already_registered,
                "registering the same ticker is idempotent");
    test.expect(second == kalshi::MarketRegistrationResult::registered &&
                    markets.size() == 2U,
                "explicit IDs are independent of registration order");
    test.expect(markets.find("FED-23DEC-T3.00") == 42U &&
                    markets.find(42U) == "FED-23DEC-T3.00" &&
                    markets.metadata_version() == 11U,
                "registry resolves both directions under a versioned mapping");
    test.expect(markets.register_market(42U, "OTHER") ==
                        kalshi::MarketRegistrationResult::market_id_conflict &&
                    markets.register_market(99U, "X") ==
                        kalshi::MarketRegistrationResult::ticker_conflict,
                "registry rejects ID and ticker remapping conflicts");
    test.expect(!markets.find("UNKNOWN").has_value(),
                "unknown ticker does not resolve");

    kalshi::MarketRegistry reverse_order{11U};
    static_cast<void>(reverse_order.register_market(7U, "X"));
    static_cast<void>(reverse_order.register_market(42U, "FED-23DEC-T3.00"));
    test.expect(reverse_order.find("X") == 7U &&
                    reverse_order.find("FED-23DEC-T3.00") == 42U,
                "registration order cannot change persistent market identity");
}

kalshi::MarketRegistry fixture_markets() {
    kalshi::MarketRegistry markets{11U};
    (void)markets.register_market(42U, "FED-23DEC-T3.00");
    (void)markets.register_market(7U, "X");
    return markets;
}

void test_snapshot_pipeline(eme::test::Context& test) {
    constexpr std::string_view fixture = R"json({
      "type": "orderbook_snapshot",
      "sid": 2,
      "seq": 2,
      "msg": {
        "market_ticker": "FED-23DEC-T3.00",
        "yes_dollars_fp": [["0.0800", "300.00"], ["0.2200", "333.00"]],
        "no_dollars_fp": [["0.4600", "20.00"], ["0.4400", "146.00"]]
      }
    })json";

    const auto markets = fixture_markets();
    const auto decoded = kalshi::decode_orderbook_message(
        fixture, 1U, eme::market::ReceiveTime{}, markets);
    const auto* wire = std::get_if<kalshi::WireOrderBookSnapshot>(&decoded);
    test.expect(wire != nullptr, "raw snapshot JSON decodes strictly");
    if (wire == nullptr) {
        return;
    }
    test.expect(markets.find("FED-23DEC-T3.00") == wire->market_id,
                "payload ticker determines the internal market identity");
    test.expect(wire->connection_generation == 1U,
                "decoder preserves the connection generation");

    const auto normalized_result = kalshi::normalize_orderbook_snapshot(*wire);
    const auto* event = std::get_if<eme::market::BookSnapshot>(&normalized_result);
    test.expect(event != nullptr, "decoded snapshot normalizes into a core event");
    if (event == nullptr) {
        return;
    }

    eme::book::OrderBook book;
    test.expect(book.apply_snapshot(
                    event->stream_id, event->sequence, event->bids, event->asks) ==
                    eme::book::BookUpdateResult::applied,
                "raw snapshot reaches the venue-neutral order book");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 2'200 &&
                    book.best_ask().has_value() && book.best_ask()->raw() == 4'400,
                "decoder-normalizer-book pipeline preserves executable prices");
}

void test_delta_pipeline(eme::test::Context& test) {
    constexpr std::string_view fixture = R"json({
      "type": "orderbook_delta",
      "sid": 2,
      "seq": 3,
      "msg": {
        "market_ticker": "FED-23DEC-T3.00",
        "price_dollars": "0.960",
        "delta_fp": "-54.00",
        "side": "yes",
        "ts_ms": 1669149841000
      }
    })json";

    const auto markets = fixture_markets();
    const auto decoded = kalshi::decode_orderbook_message(
        fixture, 1U, eme::market::ReceiveTime{}, markets);
    const auto* wire = std::get_if<kalshi::WireOrderBookDelta>(&decoded);
    test.expect(wire != nullptr, "raw delta JSON decodes strictly");
    if (wire == nullptr) {
        return;
    }

    const auto normalized_result = kalshi::normalize_orderbook_delta(*wire);
    const auto* event = std::get_if<eme::market::BookDelta>(&normalized_result);
    test.expect(event != nullptr, "decoded delta normalizes into a core event");
    if (event != nullptr) {
        test.expect(event->sequence == 3U && event->side == eme::book::Side::bid &&
                        event->price.raw() == 9'600 &&
                        event->quantity_delta.raw() == -5'400,
                    "raw delta fields survive the decoding boundary");
    }
}

void test_decode_errors(eme::test::Context& test) {
    const auto markets = fixture_markets();
    const auto decode = [&markets](const std::string_view payload) {
        return kalshi::decode_orderbook_message(
            payload, 1U, eme::market::ReceiveTime{}, markets);
    };
    const auto has_error = [](const kalshi::DecodedOrderBookMessage& result,
                              const kalshi::DecodeErrorCode expected) {
        const auto* error = std::get_if<kalshi::DecodeError>(&result);
        return error != nullptr && error->code == expected;
    };

    test.expect(has_error(decode("{"), kalshi::DecodeErrorCode::invalid_json),
                "malformed JSON is rejected without throwing");
    test.expect(has_error(
                    decode(R"json({"type":"orderbook_delta","sid":2,"msg":{}})json"),
                    kalshi::DecodeErrorCode::missing_field),
                "missing sequence is rejected");
    test.expect(has_error(
                    decode(R"json({"type":"orderbook_snapshot","sid":2,"seq":2,"msg":{"market_ticker":"X","yes_dollars_fp":[["0.5","1.00","extra"]],"no_dollars_fp":[]}})json"),
                    kalshi::DecodeErrorCode::invalid_level),
                "malformed price level is rejected");
    test.expect(has_error(
                    decode(R"json({"type":"ticker","sid":2,"seq":2,"msg":{"market_ticker":"X"}})json"),
                    kalshi::DecodeErrorCode::unsupported_message_type),
                "non-orderbook message is classified explicitly");
    test.expect(has_error(
                    decode(R"json({"type":"orderbook_snapshot","sid":2,"seq":2,"msg":{"market_ticker":"UNKNOWN","yes_dollars_fp":[],"no_dollars_fp":[]}})json"),
                    kalshi::DecodeErrorCode::unknown_market),
                "unregistered payload ticker fails closed");
    test.expect(has_error(
                    decode(R"json({"type":"orderbook_snapshot","sid":2,"seq":2,"msg":{"market_ticker":"","yes_dollars_fp":[],"no_dollars_fp":[]}})json"),
                    kalshi::DecodeErrorCode::invalid_field_value),
                "empty payload ticker is rejected");
    test.expect(has_error(
                    kalshi::decode_orderbook_message(
                        R"json({"type":"orderbook_snapshot","sid":2,"seq":2,"msg":{"market_ticker":"X","yes_dollars_fp":[],"no_dollars_fp":[]}})json",
                        0U,
                        eme::market::ReceiveTime{},
                        markets),
                    kalshi::DecodeErrorCode::invalid_field_value),
                "zero connection generation is rejected");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_market_registry(test);
    test_snapshot_pipeline(test);
    test_delta_pipeline(test);
    test_decode_errors(test);
    return test.result();
}
