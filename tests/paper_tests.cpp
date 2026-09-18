#include "study_fixture.hpp"
#include "session/async_jsonl.hpp"
#include "transport/paper_metrics.hpp"

using namespace eme::test::study;
int main() {
    try {
        eme::test::Context test;
        Fixture fixture;
        const auto input = fixture.build();
        auto parameters = policy();
        parameters["schema_version"] = 3U; parameters["strategy"] = "execution_lifecycle_v3";
        parameters["max_sizing_evaluations"] = 100000U;
        parameters["lifecycle"] = {{"execution_policy", "parallel_hold"}, {"first_leg", 0U},
            {"response_latency_ns", {500U, 500U}}, {"completion_timeout_ns", 5000U},
            {"completion_loss_limit_micro_usd", 0U}, {"maximum_completion_orders", 2U}, {"settlements", Json::array()}};
        write(fixture.root / "policy.json", parameters);
        std::ostringstream out;
        auto simulation = session::make_execution_simulation(input.session.metadata, fixture.root / "policy.json", out, true);
        simulation->start(true);
        test.expect(!simulation->next_event_time(), "no clock wakeups without pending orders");
        test.expect(std::holds_alternative<session::ReplaySummary>(session::replay(input, *simulation)), "streaming observer accepts existing lifecycle");
        simulation->checkpoint(10000);
        simulation->report(input.plan);
        test.expect(!simulation->next_event_time(), "finished lifecycle has no pending timer");
        test.expect(out.str().find("live_simulated_ioc") != std::string::npos &&
            out.str().find("quantity_centicontracts") != std::string::npos && out.str().find("paper_status") != std::string::npos,
            "live trace exposes simulation label and holdings");
        parameters["lifecycle"]["settlements"].push_back({{"market_id", 1U}, {"yes_wins", true},
            {"time_ns", 1000000U}, {"provenance", "synthetic future label"}});
        write(fixture.root / "future.json", parameters);
        bool rejected{};
        try { (void)session::make_execution_simulation(input.session.metadata, fixture.root / "future.json", out, true); }
        catch (const session::ReplayError&) { rejected = true; }
        test.expect(rejected, "live simulation rejects future settlement outcomes before starting");

        std::string expected;
        {
            session::detail::AsyncJsonl sink{fixture.root / "async.jsonl"};
            for (int i = 0; i < 2000; ++i) {
                const auto line = Json{{"type", "fixture"}, {"n", i}, {"payload", "spaced text\n"}}.dump() + '\n';
                expected += line; sink.stream() << line;
            }
            test.expect(sink.finish(), "bounded asynchronous sink drains successfully");
            test.expect(sink.finish(), "sink finish is idempotent");
            sink.stream() << "{}\n";
            test.expect(!sink.healthy(), "closed sink rejects further writes");
        }
        std::ifstream saved{fixture.root / "async.jsonl", std::ios::binary};
        const std::string actual{std::istreambuf_iterator<char>{saved}, std::istreambuf_iterator<char>{}};
        test.expect(actual == expected, "async output preserves every byte and order");
        {
            session::detail::AsyncJsonl sink{fixture.root / "overflow.jsonl", 32U};
            sink.stream() << "{\"data\":\"" << std::string(100, 'x') << "\"}\n";
            test.expect(!sink.finish(), "queue overflow fails rather than silently losing economic events");
        }
        {
            session::detail::AsyncJsonl sink{fixture.root / "partial.jsonl"};
            sink.stream() << "unfinished";
            test.expect(!sink.finish(), "unterminated output cannot be certified");
        }
        eme::transport::PaperLatency latency;
        for (int i = 1; i <= 100; ++i) { latency.observe(i); }
        const auto histogram = latency.json();
        test.expect(histogram["samples"] == 100U && histogram["p50_upper_ns"] == 63U &&
            histogram["p99_upper_ns"] == 127U && histogram["max_ns"] == 100U,
            "fixed histogram reports percentile upper bounds honestly");
        return test.result();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    catch (const session::ReplayError& error) { std::cerr << error.reason << '\n'; return 1; }
}
