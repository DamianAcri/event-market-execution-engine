// Offline diagnostic: reuse native session validation, books and fill charges.
// Build/link against the same objects as eme-passive-probe; no transport linked.
#include "eme/session/basket_observation.hpp"
#include "eme/core/execution_cost.hpp"
#include "session/study_json.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
using J = nlohmann::json;
using I = std::int64_t;
namespace es = eme::session;
namespace em = eme::market;

J cost(const J& request, const em::MarketState& state) {
    J row={{"id",request.at("id")},{"time_ns",request.at("time_ns")},{"valid",false},{"fills",J::array()}};
    const auto* book=state.find_book(request.at("market_id").get<em::MarketId>());
    if (!state.connected() || !book || book->state()!=eme::book::BookState::valid ||
        (book->best_bid() && book->best_ask() && book->best_bid()->raw()>book->best_ask()->raw())) { return row; }
    const bool yes=request.at("outcome")=="yes";
    const I desired=request.at("quantity_centicontracts").get<I>();
    const I limit=request.at("limit_price_1e4").get<I>();
    if (desired<=0 || desired>10000 || limit<0 || limit>10000) { throw std::runtime_error("query limits"); }
    const eme::core::FeePolicy fee{request.at("coefficient_ppm").get<std::uint32_t>(),request.at("balance_quantum_micro").get<std::uint32_t>()};
    eme::core::FeeAccumulator accumulator; I filled=0, debit=0;
    book->visit_levels(yes?eme::book::Side::ask:eme::book::Side::bid,[&](eme::book::Level level) {
        const I price=yes?level.price.raw():10000-level.price.raw();
        if (price>limit) { return false; }
        const I q=std::min(desired-filled,level.quantity.raw());
        if (!q) { return filled<desired; }
        const auto charged=eme::core::charge_buy_fill(*eme::core::Quantity::from_raw(q),*eme::core::Price::from_raw(price),fee,accumulator);
        if (!charged) { throw std::runtime_error("charge failed"); }
        filled+=q; debit+=charged->debit.raw();
        row["fills"].push_back({{"price_1e4",price},{"quantity_centicontracts",q},{"debit_micro",charged->debit.raw()}});
        return filled<desired;
    });
    row["valid"]=true;row["quantity_centicontracts"]=filled;row["debit_micro"]=debit;return row;
}

class Probe : public es::ReplayObserver {
public:
    Probe(J requests, I until, std::ostream& out): requests_(std::move(requests)),until_(until),out_(out) {
        if (!requests_.is_array() || requests_.size()>2000) { throw std::runtime_error("query budget"); }
        std::stable_sort(requests_.begin(),requests_.end(),[](const J& a,const J& b){return a.at("time_ns").get<I>()<b.at("time_ns").get<I>();});
    }
    void before(I time,const em::MarketState& state) override {
        pending_.clear();
        if (last_ && (time<*last_ || time-*last_>15'000'000'000LL)) { broken_=true; }
        while (cursor_<requests_.size() && requests_[cursor_].at("time_ns").get<I>()<time) {
            auto r=cost(requests_[cursor_++],state);
            if (!last_ || broken_) { r["valid"]=false; }
            pending_.push_back(std::move(r));
        }
    }
    void after(const es::ReplayFrame& frame,std::span<const eme::opportunity::CandidateEvent>,const em::MarketState& state) override {
        if (!frame.observed_wall_ns) { throw std::runtime_error("missing wall clock"); }
        if (wall_ && (*frame.observed_wall_ns<*wall_ || *frame.observed_wall_ns-*wall_>15'000'000'000LL)) { broken_=true; }
        const bool censored=broken_ || !state.connected() || *frame.observed_wall_ns>=until_ ||
            (generation_ && generation_!=state.connection_generation());
        for (auto& r:pending_) { emit(r,censored); }
        pending_.clear();
        while (cursor_<requests_.size() && requests_[cursor_].at("time_ns").get<I>()==frame.time_ns) {
            auto r=cost(requests_[cursor_++],state);emit(r,censored);
        }
        last_=frame.time_ns;wall_=frame.observed_wall_ns;generation_=state.connection_generation();
    }
    void finish(I,const em::MarketState&) override {
        while(cursor_<requests_.size()) {auto r=requests_[cursor_++];r["valid"]=false;r["fills"]=J::array();emit(r,true);}
    }
private:
    void emit(J& r,bool censored) {
        if(censored || !r.at("valid").get<bool>()) {r["valid"]=false;r["fills"]=J::array();r.erase("debit_micro");r.erase("quantity_centicontracts");}
        out_<<r.dump()<<'\n';if(!out_){throw std::runtime_error("output");}
    }
    J requests_;std::size_t cursor_{};I until_;std::ostream& out_;std::vector<J> pending_;
    std::optional<I> last_,wall_;std::optional<em::ConnectionGeneration> generation_;bool broken_{};
};
#ifndef EME_EPISODE_PROBE_TEST
int main(int argc,char** argv) {
    try {
        if(argc!=3){throw std::runtime_error("Usage: episode-probe SESSION SCHEDULE");}
        const std::filesystem::path root{argv[1]};auto loaded=es::load_replay(root,root/"replay.json");
        if(const auto* error=std::get_if<es::ReplayError>(&loaded)){throw std::runtime_error(error->reason);}
        const auto& input=std::get<es::ReplayInput>(loaded);
        const auto schedule=J::parse(es::detail::read_text(argv[2]));
        if(schedule.at("manifest_sha256")!=input.plan.manifest_sha256){throw std::runtime_error("schedule manifest mismatch");}
        std::ostringstream discarded;auto validated=es::make_basket_observation(input.session.metadata,root/"basket-policy.json",discarded);
        const auto policy=J::parse(es::detail::read_text(root/"basket-policy.json"));
        Probe probe{schedule.at("queries"),policy.at("valid_until_unix_ms").get<I>()*1'000'000,std::cout};
        const auto result=es::replay(input,probe);
        if(const auto* error=std::get_if<es::ReplayError>(&result)){throw std::runtime_error(error->reason);}
        std::cout<<J{{"type","complete"},{"orders_sent",0},{"manifest_sha256",input.plan.manifest_sha256}}.dump()<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
