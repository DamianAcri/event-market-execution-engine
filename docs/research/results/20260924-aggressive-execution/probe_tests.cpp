#define EME_EPISODE_PROBE_TEST
#include "probe.cpp"
#include "test_support.hpp"
J request(I time,I limit=10000) {return {{"id",time},{"time_ns",time},{"market_id",1},{"outcome","yes"},{"quantity_centicontracts",100},{"limit_price_1e4",limit},{"coefficient_ppm",0},{"balance_quantum_micro",10000}};}
void book(em::MarketState& state,I time,I ask,std::uint64_t seq,I depth=100){
    const auto r=state.apply(em::BookSnapshot{1,1,1,seq,em::ReceiveTime{std::chrono::nanoseconds{time}},
        {eme::test::level(4000,100)},{eme::test::level(ask,depth)}});
    if(std::get<eme::book::BookUpdateResult>(r)!=eme::book::BookUpdateResult::applied){throw std::runtime_error("fixture");}
}
std::vector<J> lines(const std::ostringstream& out){std::istringstream s{out.str()};std::string l;std::vector<J> v;while(std::getline(s,l)){v.push_back(J::parse(l));}return v;}
int main(){
    eme::test::Context t;em::MarketState state;t.expect(state.open_connection(1),"fixture open");book(state,1,5000,1);
    auto r=cost(request(1,4900),state);t.expect(r["quantity_centicontracts"]==0,"fixed limit excludes worse price");
    book(state,2,5000,2,40);r=cost(request(2),state);t.expect(r["quantity_centicontracts"]==40 && r["debit_micro"]==200000,"partial depth is not invented");
    book(state,3,5000,3);std::ostringstream out;Probe p{J::array({request(5),request(10),request(30)}),1000000000000LL,out};
    p.before(3,state);p.after({0,3,1,true,{},1003},{},state);
    p.before(10,state);book(state,10,7000,4);p.after({1,10,1,true,{},1010},{},state);p.finish(10,state);
    auto rows=lines(out);t.expect(rows[0]["debit_micro"]==500000,"future price cannot leak into earlier arrival");
    t.expect(rows[1]["debit_micro"]==700000,"arrival at update sees current price");t.expect(rows[2]["valid"]==false,"EOF cannot extrapolate unobserved arrivals");
    for(bool wallgap:{false,true}){
        std::ostringstream g;Probe gap{J::array({request(5)}),1000000000000LL,g};gap.before(3,state);gap.after({0,3,1,true,{},1003},{},state);
        const I next=wallgap?10:20'000'000'000LL;gap.before(next,state);gap.after({1,next,1,true,{},20'000'000'000LL},{},state);
        t.expect(lines(g)[0]["valid"]==false,"clock gaps censor pending earlier prices before publication");
    }
    t.expect(state.close_connection(1),"fixture close");t.expect(cost(request(1),state)["valid"]==false,"disconnected book cannot fill");
    return t.result();
}
