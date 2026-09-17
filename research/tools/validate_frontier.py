#!/usr/bin/env python3
"""Offline rational witness checks and public-trade context; never infer fills."""
import argparse
from collections import Counter
from copy import deepcopy
from fractions import Fraction
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct


REPO = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


oracle = module("basket_oracle", REPO / "tests/basket_screen_oracle_tests.py")
audit = module("capture_audit", REPO / "research/results/20260917-basket-live/audit.py")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def quote(witness, mode, quantity):
    notional = debit = reserve = 0
    for index, leg in enumerate(witness["legs"]):
        passive = index == witness["point"]["passive_leg_index"]
        coefficient = 0 if passive and mode == "one_passive_zero_fee_hypothesis" else leg["fee_coefficient_ppm"]
        quantum = leg["balance_quantum_micro"]
        depth = [[leg["join_price_1e4"], quantity]] if passive else leg["buy_depth"]
        remaining, accumulated, limit = quantity, 0, None
        for price, size in depth:
            used = min(size, remaining)
            if not used:
                break
            cost, charged, accumulated = oracle.fee_terms(used, price, coefficient, quantum, accumulated)
            notional += cost
            debit += charged
            remaining -= used
            limit = price
        if remaining:
            return None
        reserve += quantity * limit + math.ceil(Fraction(quantity * coefficient, 400)) + quantity * (quantum + 1)
    floor = quantity * 20000
    return {"gross_margin_micro": floor - notional, "net_margin_micro": floor - debit,
            "fees_and_rounding_micro": debit - notional, "reservation_micro": reserve}


def validate_witness(witness, mode, cash):
    point = witness["point"]
    expected = quote(witness, mode, point["quantity_centicontracts"])
    require(expected is not None, "insufficient witness depth")
    for field, value in expected.items():
        require(point[field] == value, "rational witness mismatch: " + field)
    require(expected["reservation_micro"] <= cash, "unfunded witness")
    q = point["quantity_centicontracts"]
    for candidate in range(100, 10001, 100):
        other = quote(witness, mode, candidate)
        if other and other["reservation_micro"] <= cash:
            require(other["net_margin_micro"] * q <= point["net_margin_micro"] * candidate,
                    "better unit margin at the same witness")


def trades(root):
    # audit.audit verifies framing/CRC/hash bindings first. Re-read only public trades.
    with (root / "session/market.journal").open("rb") as stream:
        stream.read(12)
        while prefix := stream.read(4):
            record = stream.read(struct.unpack("<I", prefix)[0])
            stream.read(4)
            _, _, generation, time_ns, _, _, flags = struct.unpack_from("<IQQqqQB", record)
            offset = 45 + flags * 8
            channel_size, _ = struct.unpack_from("<II", record, offset)
            offset += 8
            if record[offset:offset + channel_size] != b"ws.receive.v1":
                continue
            message = json.loads(record[offset + channel_size:])
            if message.get("type") == "trade":
                yield generation, time_ns, message["msg"]


def validate(root, path):
    audit.audit(root)
    data = json.loads(path.read_text())
    summary = json.loads((root / "session/basket-summary.json").read_text())
    policy = json.loads((root / "session/basket-policy.json").read_text())
    for field in ("records", "book_updates", "public_trades", "basket_evaluations", "manifest_sha256", "policy_sha256"):
        require(data[field] == summary[field], "original observer mismatch: " + field)
    require(data["native_evaluated_quantities"] == summary["evaluated_quantities"], "native quantity count mismatch")
    modes = {"all_taker", "one_passive_zero_fee_hypothesis", "one_passive_taker_fee_stress"}
    ids = {b["id"] for b in policy["screen"]["baskets"]}
    require(len(data["rows"]) == len(ids) * len(modes), "row count mismatch")
    require({(r["basket_id"], r["mode"]) for r in data["rows"]} == {(i, m) for i in ids for m in modes}, "row identity mismatch")
    cash = policy["screen"]["sizing"]["available_cash_micro"]
    checked = 0
    for row in data["rows"]:
        witness = row["best_per_contract_witness"]
        if witness is None:
            require(row["funded_quantities"] == 0, "missing witness")
            continue
        require(witness["point"] == row["best_net_per_contract"], "point binding mismatch")
        validate_witness(witness, row["mode"], cash)
        changed = deepcopy(witness)
        changed["point"]["net_margin_micro"] += 1
        try:
            validate_witness(changed, row["mode"], cash)
        except ValueError:
            pass
        else:
            raise ValueError("one-microdollar negative control was not rejected")
        checked += 1
    require(sum(r["funded_quantities"] for r in data["rows"] if r["mode"] == "all_taker") == data["native_evaluated_quantities"], "all-size count mismatch")
    require(sum(r["net_positive_quantities"] for r in data["rows"] if r["mode"] == "all_taker") == 0, "baseline changed")
    public = list(trades(root))
    require(len(public) == data["public_trades"], "trade count mismatch")
    tickers = {m["id"]: m["ticker"] for m in policy["screen"]["markets"]}
    context = []
    for row in data["rows"]:
        if row["mode"] == "all_taker" or not row["net_positive_quantities"]:
            continue
        witness = row["best_per_contract_witness"]
        point = witness["point"]
        leg = witness["legs"][point["passive_leg_index"]]
        outcome = "yes" if point["passive_leg_index"] == 0 else "no"
        market_trades = [(g, t, msg) for g, t, msg in public if msg["market_ticker"] == tickers[leg["market_id"]]]
        later = [msg for g, t, msg in market_trades if g == point["generation"] and t > point["time_ns"]]
        context.append({"basket_id": row["basket_id"], "mode": row["mode"], "market_id": leg["market_id"],
            "passive_outcome": outcome, "join_price_1e4": leg["join_price_1e4"],
            "visible_queue_centicontracts": leg["visible_queue_centicontracts"],
            "market_public_trades_entire_capture": len(market_trades),
            "market_public_trades_later_same_connection": len(later),
            "later_trade_prices_1e4": sorted({int(Fraction(msg[outcome + "_price_dollars"]) * 10000) for msg in later}),
            "later_taker_outcome_counts": dict(Counter(msg["taker_outcome_side"] for msg in later)),
            "trade_through_or_own_fill_inferred": False})
    paths = [path, Path(__file__), REPO / "research/tools/basket_frontier.cpp", REPO / "tests/basket_screen_oracle_tests.py"]
    return {"schema_version": 1, "capture": root.name, "original_capture_audit_passed": True,
            "rational_witnesses_checked": checked, "witness_grid_sizes_checked": 100,
            "one_microdollar_corruption_controls_rejected": checked,
            "original_observer_counts_match": True, "public_trade_context": context,
            "artifact_sha256": {(p.resolve().relative_to(REPO).as_posix() if p.resolve().is_relative_to(REPO) else p.name):
                                hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
            "orders_sent": 0, "simulated_fills": False, "realized_pnl_micro": None}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("frontier", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.capture, args.frontier), indent=2, sort_keys=True))
