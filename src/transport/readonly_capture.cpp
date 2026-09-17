#include "eme/transport/readonly_capture.hpp"
#include "eme/session/readonly_feed.hpp"
#include "session/study_json.hpp"
#include "session/async_jsonl.hpp"
#include "eme/session/execution_study.hpp"
#include "paper_metrics.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <csignal>
#include <iostream>

namespace eme::transport {
namespace {
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using Tcp = net::ip::tcp;
using Error = boost::system::error_code;
using Json = session::detail::Json;
using Clock = std::chrono::steady_clock;
constexpr auto path = "/trade-api/ws/v2";
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

int no_password(char*, int, int, void*) { return 0; }
Key load_key(const std::filesystem::path& filename) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new_file(filename.string().c_str(), "rb"), BIO_free};
    Key key{bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, no_password, nullptr) : nullptr, EVP_PKEY_free};
    if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA || EVP_PKEY_bits(key.get()) < 2048) {
        session::detail::invalid("RSA private key unavailable/unsupported");
    }
    return key;
}
std::string sign(EVP_PKEY* key, const std::string& message) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    EVP_PKEY_CTX* pkey_context{};
    if (!context || EVP_DigestSignInit(context.get(), &pkey_context, EVP_sha256(), nullptr, key) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(pkey_context, RSA_PKCS1_PSS_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_context, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pkey_context, EVP_sha256()) <= 0 ||
        EVP_DigestSignUpdate(context.get(), message.data(), message.size()) <= 0) {
        session::detail::invalid("authentication signing failed");
    }
    std::size_t length{};
    if (EVP_DigestSignFinal(context.get(), nullptr, &length) <= 0 || length > 4096U) {
        session::detail::invalid("authentication signature size");
    }
    std::vector<unsigned char> bytes(length);
    if (EVP_DigestSignFinal(context.get(), bytes.data(), &length) <= 0) { session::detail::invalid("authentication signature failed"); }
    std::string encoded(4U * ((length + 2U) / 3U), '\0');
    const auto written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), bytes.data(), static_cast<int>(length));
    if (written < 0 || static_cast<std::size_t>(written) != encoded.size()) { session::detail::invalid("authentication encoding failed"); }
    return encoded;
}

struct Connection final {
    Connection(net::io_context& io, ssl::context& context) : resolver{io}, socket{io, context}, deadline{io} {}
    Tcp::resolver resolver;
    ws::stream<beast::ssl_stream<beast::tcp_stream>> socket;
    net::steady_timer deadline;
    beast::flat_buffer buffer{1024U * 1024U};
    ws::response_type response;
    Clock::time_point last_receive{Clock::now()};
    Clock::time_point opened_at{};
    std::size_t next_command{};
    bool opened{};
};

class Runner final {
public:
    Runner(const CaptureConfig& config, const gateway::kalshi::MetadataSnapshot& metadata)
        : config_{config}, metadata_{metadata}, feed_{metadata.markets(), config.markets, config.public_trades
            ? session::FeedProtocol::book_and_trades_v1 : session::FeedProtocol::shared_subscription_v1},
          context_{ssl::context::tls_client}, key_{load_key(config.private_key)},
          duration_{io_}, health_{io_}, retry_{io_}, paper_timer_{io_}, signals_{io_, SIGINT, SIGTERM} {
        if (config.duration.count() <= 0 || config.duration > std::chrono::hours{24} ||
            config.handshake_timeout.count() <= 0 || config.idle_timeout.count() <= 0 ||
            config.handshake_timeout > std::chrono::minutes{1} || config.idle_timeout > std::chrono::minutes{10} ||
            config.retry_delay.count() <= 0 || config.retry_delay > std::chrono::seconds{30} || config.maximum_connections == 0U || config.maximum_connections > 1000U ||
            config.key_id.empty() || config.key_id.size() > 128U ||
            config.key_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) {
            session::detail::invalid("capture configuration");
        }
        if (SSL_CTX_set_min_proto_version(context_.native_handle(), TLS1_2_VERSION) != 1) { session::detail::invalid("TLS minimum version"); }
        Error trust_error;
        if (config.ca_file.empty()) { context_.set_default_verify_paths(trust_error); }
        else { context_.load_verify_file(config.ca_file.string(), trust_error); }
        if (trust_error) { session::detail::invalid("TLS trust configuration unavailable"); }
        context_.set_verify_mode(ssl::verify_peer);
        auto created = session::create_async_capture(config.directory, metadata, config.queue);
        if (std::holds_alternative<session::SessionError>(created)) { session::detail::invalid("capture directory/writer"); }
        writer_ = std::get<std::unique_ptr<session::AsyncCaptureWriter>>(std::move(created));
        if (!config.paper_policy.empty()) {
            const auto policy = session::detail::read_text(config.paper_policy);
            if (session::detail::write_new_file(config.directory / "paper-policy.json", policy)) { session::detail::invalid("paper policy copy"); }
            paper_output_ = std::make_unique<session::detail::AsyncJsonl>(config.directory / "paper.jsonl");
            simulation_ = session::make_execution_simulation(metadata_, config.directory / "paper-policy.json", paper_output_->stream(), true);
            simulation_->start(true);
        }
    }
    CaptureResult run() {
        duration_.expires_after(config_.duration);
        duration_.async_wait([this](Error ec) { if (!ec) { stop("duration"); } });
        signals_.async_wait([this](Error ec, int) { if (!ec) { stop("operator_stop"); } });
        health();
        connect();
        io_.run();
        if (storage_failed_) { return {false, updates_, generation_, "recorder_failure"}; }
        if (simulation_ && !paper_failed_) {
            simulation_->finish(last_time_, feed_.state());
            simulation_->checkpoint(last_time_);
            paper_timing();
            session::ReplayPlan provisional;
            provisional.source_kind = config_.synthetic ? "synthetic" : "observed_ws";
            simulation_->report(provisional);
        }
        if (paper_output_ && !paper_output_->finish()) { paper_failed_ = true; }
        const auto finalized = writer_->finish();
        if (!std::holds_alternative<session::SessionManifest>(finalized)) { return {false, updates_, generation_, "finalization_failure"}; }
        const auto fingerprint = session::detail::fingerprint_file(config_.directory / session::manifest_filename);
        if (!std::holds_alternative<session::ArtifactFingerprint>(fingerprint)) { return {false, updates_, generation_, "manifest_read_failure"}; }
        const auto plan = Json{{"schema_version", config_.public_trades ? 4U : 3U}, {"source_kind", config_.synthetic ? "synthetic" : "observed_ws"},
            {"provenance", config_.synthetic ? "local TLS/WebSocket fixture" : "Kalshi read-only WS capture; shared subscription sequence"},
            {"manifest_sha256", std::get<session::ArtifactFingerprint>(fingerprint).sha256},
            {"use_yes_price", true}, {"markets", config_.markets}}.dump();
        if (session::detail::write_new_file(config_.directory / "replay.json", plan)) { return {false, updates_, generation_, "plan_write_failure"}; }
        auto input = session::load_replay(config_.directory, config_.directory / "replay.json");
        if (!std::holds_alternative<session::ReplayInput>(input)) { return {false, updates_, generation_, "plan_validation_failure"}; }
        if (!simulation_) {
            session::ReplayObserver observer;
            const auto verified = session::replay(std::get<session::ReplayInput>(input), observer);
            if (!std::holds_alternative<session::ReplaySummary>(verified)) { return {false, updates_, generation_, "controller_replay_failure"}; }
        }
        if (paper_failed_) { return {true, updates_, generation_, "paper_simulation_failure"}; }
        if (simulation_ && !verify_paper(std::get<session::ReplayInput>(input))) {
            return {true, updates_, generation_, "paper_replay_mismatch"};
        }
        return {true, updates_, generation_, reason_, public_trades_};
    }
private:
    bool current(const std::shared_ptr<Connection>& connection) const { return !done_ && connection_ == connection; }
    bool record(const std::string_view channel, std::string payload, const Clock::time_point started = Clock::now()) {
        journal::RawMarketRecord record;
        record.metadata_version = metadata_.markets().metadata_version();
        record.connection_generation = generation_;
        record.received_at = market::ReceiveTime{std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch())};
        record.observed_at = journal::WallTime{std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())};
        record.channel = channel;
        record.payload = std::move(payload);
        last_time_ = record.received_at.time_since_epoch().count();
        try {
            if (simulation_ && !paper_failed_) { simulation_->before(last_time_, feed_.state()); }
            const auto before_book = Clock::now();
            const auto update = feed_.accept(record);
            const auto after_book = Clock::now();
            if (writer_->try_append(std::move(record)) != session::CaptureAppendResult::queued) {
                storage_failed_ = true;
                feed_.abort();
                return false;
            }
            if (update.event == session::FeedEvent::market) { ++updates_; }
            if (update.event == session::FeedEvent::public_trade) { ++public_trades_; }
            if (simulation_ && !paper_failed_) {
                const auto before_decision = Clock::now();
                simulation_->after({records_, last_time_, update.market_id, update.event == session::FeedEvent::market, update.trade}, {}, feed_.state());
                const auto ended = Clock::now();
                if (update.event == session::FeedEvent::market) {
                    book_latency_.observe(std::chrono::duration_cast<std::chrono::nanoseconds>(after_book - before_book).count());
                    decision_latency_.observe(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - before_decision).count());
                    processing_latency_.observe(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count());
                }
                if (!paper_output_->healthy()) { session::detail::invalid("paper writer failed"); }
                schedule_paper();
            }
            ++records_;
            return update.event != session::FeedEvent::invalidated && update.event != session::FeedEvent::invalid_history;
        } catch (...) {
            paper_failed_ = true;
            feed_.abort();
            return false;
        }
    }
    void schedule_paper() {
        const auto next = simulation_->next_event_time();
        if (next == scheduled_paper_) { return; }
        paper_timer_.cancel();
        scheduled_paper_ = next;
        if (!next) { return; }
        if (*next == std::numeric_limits<std::int64_t>::max()) { session::detail::invalid("paper timer overflow"); }
        // before() processes strictly earlier simulated events; +1 preserves
        // the existing rule that equal-timestamp observations precede arrivals.
        paper_timer_.expires_at(Clock::time_point{std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds{*next + 1})});
        paper_timer_.async_wait([this, due = *next](Error ec) {
            if (ec || done_) { return; }
            scheduled_paper_.reset();
            timer_lateness_.observe(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() - due);
            if (!record("paper.clock.v1", "{}")) { stop("paper_simulation_failure"); }
        });
    }
    void paper_timing() {
        paper_output_->stream() << Json{{"type", "paper_timing"}, {"book_update", book_latency_.json()},
            {"decision_callback", decision_latency_.json()}, {"receive_callback_to_decisions", processing_latency_.json()},
            {"timer_lateness", timer_lateness_.json()}, {"network_order_latency_measured", false}}.dump() << '\n';
    }
    bool verify_paper(const session::ReplayInput& input) {
        // This pass happens only AFTER the live simulation and capture end.
        // No per-tick replay transcript: compare the compact economic trace.
        const auto replay_path = config_.directory / "paper-replay.jsonl";
        std::ofstream replay_output{replay_path, std::ios::binary};
        auto reference = session::make_execution_simulation(metadata_, config_.directory / "paper-policy.json", replay_output);
        reference->start();
        if (!std::holds_alternative<session::ReplaySummary>(session::replay(input, *reference))) { return false; }
        reference->report(input.plan);
        replay_output.close();
        if (!replay_output) { return false; }
        std::ifstream live{config_.directory / "paper.jsonl"}, offline{replay_path};
        const auto next = [](std::istream& file) -> std::optional<Json> {
            std::string line;
            while (std::getline(file, line)) {
                auto value = Json::parse(line);
                const auto type = value.at("type");
                if (type == "paper_status" || type == "paper_timing" || type == "study_start") { continue; }
                if (type == "study_complete") { value.erase("manifest_sha256"); value.erase("plan_sha256"); }
                return value;
            }
            return std::nullopt;
        };
        Json summary;
        for (;;) {
            const auto a = next(live), b = next(offline);
            if (a != b) { return false; }
            if (!a) { break; }
            if (a->at("type") == "study_complete") { summary = *a; }
        }
        if (live.bad() || offline.bad() || summary.is_null()) { return false; }
        summary["mode"] = "live_paper_no_orders_sent";
        summary["live_replay_equal"] = true;
        summary["manifest_sha256"] = input.plan.manifest_sha256;
        summary["plan_sha256"] = input.plan.plan_sha256;
        summary["stop_reason"] = reason_;
        summary["book_update"] = book_latency_.json();
        summary["decision_callback"] = decision_latency_.json();
        summary["receive_callback_to_decisions"] = processing_latency_.json();
        summary["timer_lateness"] = timer_lateness_.json();
        const auto fingerprint = session::detail::fingerprint_file(config_.directory / "paper.jsonl");
        if (!std::holds_alternative<session::ArtifactFingerprint>(fingerprint)) { return false; }
        summary["live_trace_sha256"] = std::get<session::ArtifactFingerprint>(fingerprint).sha256;
        return !session::detail::write_new_file(config_.directory / "paper-summary.json", summary.dump(2));
    }
    void release_connection() {
        if (!connection_) { return; }
        connection_->resolver.cancel();
        connection_->deadline.cancel();
        Error ignored;
        beast::get_lowest_layer(connection_->socket).socket().close(ignored);
        connection_.reset();
    }
    void stop(const std::string_view reason) {
        if (done_) { return; }
        if (!feed_.closed() && !storage_failed_) { (void)record("ws.close.v1", Json{{"reason", reason}}.dump()); }
        feed_.abort();
        done_ = true;
        reason_ = reason;
        release_connection();
        duration_.cancel(); health_.cancel(); retry_.cancel(); paper_timer_.cancel(); signals_.cancel();
    }
    void failed(const std::string_view reason, const bool fatal = false) {
        if (done_) { return; }
        if (fatal || storage_failed_ || paper_failed_ || generation_ >= config_.maximum_connections) { stop(reason); return; }
        if (!feed_.closed()) { (void)record("ws.close.v1", Json{{"reason", reason}}.dump()); }
        release_connection();
        if (storage_failed_) { stop("recorder_failure"); return; }
        const auto exponent = static_cast<unsigned>(std::min<std::uint64_t>(generation_ - 1U, 5U));
        retry_.expires_after(std::min(config_.retry_delay * (1U << exponent), std::chrono::milliseconds{30'000}));
        retry_.async_wait([this](Error ec) { if (!ec && !done_) { connect(); } });
    }
    void health() {
        health_.expires_after(std::chrono::milliseconds{100});
        health_.async_wait([this](Error ec) {
            if (ec || done_) { return; }
            if (writer_->status() != session::CaptureAppendResult::queued) { storage_failed_ = true; stop("recorder_failure"); return; }
            if (simulation_ && (!paper_output_->healthy() || paper_failed_)) { paper_failed_ = true; stop("paper_simulation_failure"); return; }
            if (simulation_ && Clock::now() - last_status_ >= std::chrono::seconds{60}) {
                simulation_->checkpoint(last_time_); paper_timing(); last_status_ = Clock::now();
            }
            if (connection_ && connection_->opened) {
                const auto now = Clock::now();
                if (now - connection_->last_receive >= config_.idle_timeout) { failed("idle_timeout"); }
                else if (!feed_.ready() && now - connection_->opened_at >= config_.handshake_timeout) {
                    failed("snapshot_timeout");
                }
            }
            if (!done_) { health(); }
        });
    }
    void connect() {
        if (done_) { return; }
        ++generation_;
        if (!record("ws.attempt.v1", Json{{"host", config_.host}, {"path", path}, {"use_yes_price", true},
                {"idle_timeout_ms", config_.idle_timeout.count()}, {"handshake_timeout_ms", config_.handshake_timeout.count()}}.dump())) { stop("recorder_failure"); return; }
        const auto connection = std::make_shared<Connection>(io_, context_);
        connection_ = connection;
        connection->socket.next_layer().set_verify_callback(ssl::host_name_verification(config_.host));
        if (SSL_set_tlsext_host_name(connection->socket.next_layer().native_handle(), config_.host.c_str()) != 1) { failed("tls_sni", true); return; }
        connection->deadline.expires_after(config_.handshake_timeout);
        connection->deadline.async_wait([this, connection](Error ec) { if (!ec && current(connection)) { failed("connect_timeout"); } });
        connection->resolver.async_resolve(config_.host, config_.port, [this, connection](Error ec, Tcp::resolver::results_type endpoints) {
            if (!current(connection)) { return; }
            if (ec) { failed("dns_failure"); return; }
            beast::get_lowest_layer(connection->socket).async_connect(endpoints, [this, connection](Error error, const Tcp::resolver::results_type::endpoint_type&) {
                if (!current(connection)) { return; }
                if (error) { failed("connect_failure"); return; }
                Error option_error;
                beast::get_lowest_layer(connection->socket).socket().set_option(Tcp::no_delay{true}, option_error);
                if (option_error) { failed("socket_option"); return; }
                connection->socket.next_layer().async_handshake(ssl::stream_base::client, [this, connection](Error tls_error) {
                    if (!current(connection)) { return; }
                    if (tls_error) { failed("tls_verification_or_handshake", true); return; }
                    handshake(connection);
                });
            });
        });
    }
    void handshake(const std::shared_ptr<Connection>& connection) {
        const auto timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const auto signature = sign(key_.get(), timestamp + "GET" + path);
        connection->socket.set_option(ws::stream_base::decorator([id = config_.key_id, signature, timestamp](ws::request_type& request) {
            request.set("KALSHI-ACCESS-KEY", id);
            request.set("KALSHI-ACCESS-SIGNATURE", signature);
            request.set("KALSHI-ACCESS-TIMESTAMP", timestamp);
        }));
        connection->socket.read_message_max(1024U * 1024U);
        connection->socket.text(true);
        connection->socket.async_handshake(connection->response, config_.host + ":" + config_.port, path, [this, connection](Error ec) {
            if (!current(connection)) { return; }
            if (ec) {
                const auto status = connection->response.result_int();
                failed(status == 401U || status == 403U ? "authentication_rejected" : "websocket_handshake", status == 401U || status == 403U);
                return;
            }
            connection->deadline.cancel();
            connection->socket.set_option(ws::stream_base::decorator([](ws::request_type&) {}));
            connection->opened = true;
            connection->opened_at = connection->last_receive = Clock::now();
            if (!record("ws.open.v1", "{}")) { stop("controller_open_failure"); return; }
            connection->socket.control_callback([this, weak = std::weak_ptr<Connection>{connection}](ws::frame_type type, beast::string_view payload) {
                const auto locked = weak.lock();
                if (!locked || !current(locked)) { return; }
                locked->last_receive = Clock::now();
                if (type == ws::frame_type::ping || type == ws::frame_type::pong) {
                    if (!record(type == ws::frame_type::ping ? "ws.ping.v1" : "ws.pong.v1", Json(std::string{payload}).dump())) {
                        // Never destroy the active socket inside Beast's passive callback.
                        net::post(io_, [this] { stop("recorder_failure"); });
                    }
                }
            });
            read(connection);
            send(connection);
        });
    }
    void send(const std::shared_ptr<Connection>& connection) {
        if (!current(connection) || connection->next_command >= feed_.commands().size()) { return; }
        const auto& command = feed_.commands()[connection->next_command];
        if (!record("ws.send.v1", command)) { failed("subscription_record_failure", true); return; }
        connection->socket.async_write(net::buffer(command), [this, connection](Error ec, std::size_t) {
            if (!current(connection)) { return; }
            if (ec) { failed("subscription_write_failure"); return; }
            ++connection->next_command;
            send(connection);
        });
    }
    void read(const std::shared_ptr<Connection>& connection) {
        connection->socket.async_read(connection->buffer, [this, connection](Error ec, std::size_t) {
            if (!current(connection)) { return; }
            if (ec) { failed(ec == ws::error::closed ? "peer_close" : "read_failure"); return; }
            const auto received = Clock::now();
            connection->last_receive = received;
            if (!connection->socket.got_text()) { failed("binary_message"); return; }
            auto bytes = beast::buffers_to_string(connection->buffer.data());
            connection->buffer.consume(connection->buffer.size());
            if (!record("ws.receive.v1", std::move(bytes), received)) { failed("feed_invalidated"); return; }
            read(connection);
        });
    }
    const CaptureConfig& config_;
    const gateway::kalshi::MetadataSnapshot& metadata_;
    session::ReadOnlyFeed feed_;
    net::io_context io_;
    ssl::context context_;
    Key key_;
    net::steady_timer duration_, health_, retry_, paper_timer_;
    net::signal_set signals_;
    std::unique_ptr<session::AsyncCaptureWriter> writer_;
    std::shared_ptr<Connection> connection_;
    std::unique_ptr<session::detail::AsyncJsonl> paper_output_;
    std::unique_ptr<session::ExecutionSimulation> simulation_;
    std::optional<std::int64_t> scheduled_paper_;
    std::int64_t last_time_{};
    std::uint64_t records_{};
    Clock::time_point last_status_{Clock::now()};
    PaperLatency book_latency_, decision_latency_, processing_latency_, timer_lateness_;
    std::uint64_t public_trades_{};
    bool paper_failed_{};
    std::uint64_t generation_{}, updates_{};
    bool done_{}, storage_failed_{};
    std::string reason_;
};
} // namespace
CaptureResult capture_readonly(const CaptureConfig& config, const gateway::kalshi::MetadataSnapshot& metadata) {
    try { Runner runner{config, metadata}; return runner.run(); }
    catch (const session::ReplayError& error) { return {false, 0U, 0U, error.reason}; }
    catch (const std::exception&) { return {false, 0U, 0U, "capture_runtime_failure"}; }
}
} // namespace eme::transport
