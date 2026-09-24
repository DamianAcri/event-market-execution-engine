"""Exploratory episode audit; no capture, credentials, transport or real orders."""
import argparse,datetime,hashlib,json,subprocess,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[4]/'tests'))
from basket_screen_oracle_tests import fee_terms

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def write(p,x):p.write_text(json.dumps(x,indent=2)+'\n')
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path);parser.add_argument('probe',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();out=args.output;out.mkdir(parents=True,exist_ok=False)
    root=args.capture;session=root/'session';policy=json.loads((session/'basket-policy.json').read_text())
    rows=[json.loads(l) for l in (session/'basket.jsonl').open()]
    openings=[r for r in rows if r['type']=='basket_episode_open']
    summary=json.loads((session/'basket-summary.json').read_text())
    fees={f['market_id']:f for f in policy['screen']['fees']}
    queries=[];cases=[]
    # Frozen experiment, no post-result search for quantities, prices or delays.
    for event in openings:
        for delay in (0,1,10,50,100,250):
            for mode in ('initial_limit','cross_available_depth'):
                case={'episode':event['episode'],'basket_id':event['basket_id'],'delay_ms_per_leg':delay,'mode':mode,
                      'quantity_centicontracts':event['quote']['quantity_centicontracts'],
                      'opening_margin_micro':event['quote']['net_margin_micro'],'query_ids':[]}
                for leg,price in enumerate(event['quote']['legs']):
                    row={'id':len(queries),'time_ns':event['time_ns']+(leg+1)*delay*1_000_000,
                         'market_id':price['market_id'],'outcome':price['outcome'],
                         'quantity_centicontracts':case['quantity_centicontracts'],
                         'limit_price_1e4':price['limit_price_1e4'] if mode=='initial_limit' else 10000,
                         'coefficient_ppm':fees[price['market_id']]['coefficient_ppm'],
                         'balance_quantum_micro':fees[price['market_id']]['balance_quantum_micro']}
                    queries.append(row);case['query_ids'].append(row['id'])
                cases.append(case)
    plan={'schema_version':1,'created_before_probe_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'manifest_sha256':summary['manifest_sha256'],'basket_policy_sha256':digest(session/'basket-policy.json'),
          'episode_trace_sha256':digest(session/'basket.jsonl'),'probe_sha256':digest(args.probe),
          'probe_source_sha256':digest(Path(__file__).with_name('probe.cpp')),'audit_source_sha256':digest(Path(__file__)),
          'exploratory_after_observing_capture':True,'orders_sent':0,'delay_is_measured_network_latency':False,
          'timing':'First leg arrives after d; next legs at 2d and 3d. Equal time samples first processed frame at that timestamp.',
          'execution':'Native observed depth; stop submitting further legs on invalid book or first partial fill; no unwind.',
          'limitations':['Selected observed episodes, not an independent forward strategy trial.',
                         'No venue-atomic snapshot, hidden liquidity, race or market impact model.',
                         'One assumed fill per price level; fee fragmentation uncalibrated.',
                         'Episodes share liquidity; independent counterfactuals cannot be summed as earnings.',
                         'Payoff floor conditional on existing settlement rules; not realized PnL.'],
          'cases':cases,'queries':queries}
    write(out/'plan.json',plan)
    started=time.monotonic()
    with (out/'depth.jsonl').open('wb') as f:subprocess.run([str(args.probe),str(session),str(out/'plan.json')],stdout=f,check=True,timeout=180)
    answers=[json.loads(l) for l in (out/'depth.jsonl').open()]
    assert answers[-1]['type']=='complete' and answers[-1]['orders_sent']==0
    by_id={r['id']:r for r in answers[:-1]};assert len(by_id)==len(queries)
    rational_checks=0
    for request in queries:
        answer=by_id[request['id']]
        if not answer['valid']:continue
        total=accumulated=quantity=0
        for fill in answer['fills']:
            _,charge,accumulated=fee_terms(fill['quantity_centicontracts'],fill['price_1e4'],request['coefficient_ppm'],request['balance_quantum_micro'],accumulated)
            assert charge==fill['debit_micro'];total+=charge;quantity+=fill['quantity_centicontracts'];rational_checks+=1
        assert total==answer['debit_micro'] and quantity==answer['quantity_centicontracts']
    results=[]
    for case in cases:
        holdings=[0,0,0];debit=0;valid=True;legs=[]
        for leg,idx in enumerate(case['query_ids']):
            a=by_id[idx];legs.append(a)
            if not a['valid']:valid=False;break
            holdings[leg]=a['quantity_centicontracts'];debit+=a['debit_micro']
            if holdings[leg]!=case['quantity_centicontracts']:break
        completed=all(q==case['quantity_centicontracts'] for q in holdings)
        floor=min(holdings)*20000
        r={**case,'holdings_centicontracts':holdings,'debit_micro':debit,'complete':completed,'valid_until_stop':valid,
           'unmatched_holdings_centicontracts':[q-min(holdings) for q in holdings],
           'conditional_margin_micro':floor-debit if completed else None,'realized_pnl_micro':None,'legs':legs}
        if case['delay_ms_per_leg']==0:assert completed and r['conditional_margin_micro']==case['opening_margin_micro'], 'Zero-delay native quote parity failed'
        results.append(r)
    report={'orders_sent':0,'exploratory':True,'results_are_additive':False,'processing_seconds':time.monotonic()-started,
            'native_depth_queries':len(queries),'rational_fill_checks':rational_checks,'zero_delay_opening_quote_parity':True,
            'plan_sha256':digest(out/'plan.json'),'depth_sha256':digest(out/'depth.jsonl'),'results':results}
    write(out/'report.json',report)
    for mode in ('initial_limit','cross_available_depth'):
        for delay in (0,1,10,50,100,250):
            rs=[r for r in results if r['mode']==mode and r['delay_ms_per_leg']==delay]
            margins=[r['conditional_margin_micro']/1e6 for r in rs if r['complete']]
            print(mode,delay,'ms:',sum(r['complete'] for r in rs),'complete;',sum(x>0 for x in margins),'positive; margins',margins,'residual',sum(any(r['unmatched_holdings_centicontracts']) for r in rs))
    print('Report:',out/'report.json')
if __name__=='__main__':main()
