#!/usr/bin/env python3
"""Audit a saved basket capture without loading settings or contacting a venue.

Usage: python3 audit.py /path/to/basket-observe-... > evidence.json
Only sanitized aggregate evidence is written to stdout. Capture files stay intact.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zlib


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def audit(root):
    def load(name):
        return json.loads((root / name).read_text())

    summary = load("session/basket-summary.json")
    result = load("result.json")
    plan = load("observation-plan.json")
    policy = load("observation-policy.json")
    preparation = load("preparation/status.json")
    manifest = load("session/manifest.json")
    witnesses = {}
    for basket in summary["baskets"]:
        quote = basket["best_one_contract_diagnostic"]
        if quote is not None:
            witnesses.setdefault(quote["time_ns"], []).append((basket["basket_id"], quote))

    channels = Counter()
    generations = {}
    gaps = []
    mapped_witnesses = []
    record_count = 0
    previous = None
    first_ns = None
    with (root / "session/market.journal").open("rb") as stream:
        require(stream.read(12) == b"EMEJNL02\x02\x00\x00\x00", "unexpected journal header")
        while True:
            prefix = stream.read(4)
            if not prefix:
                break
            require(len(prefix) == 4, "truncated record length")
            size, = struct.unpack("<I", prefix)
            require(53 <= size <= 16 * 1024 * 1024 + 256 + 80, "invalid record size")
            record = stream.read(size)
            checksum = stream.read(4)
            require(len(record) == size and len(checksum) == 4, "truncated record")
            require(zlib.crc32(record) == struct.unpack("<I", checksum)[0], "record CRC mismatch")
            schema, metadata, generation, mono_ns, wall_ns, sequence, flags = struct.unpack_from("<IQQqqQB", record)
            require(schema == 2 and metadata > 0 and generation > 0 and sequence == 0 and flags == 0,
                    "unexpected read-only journal envelope")
            offset = 45 + 8 * flags
            channel_size, payload_size = struct.unpack_from("<II", record, offset)
            offset += 8
            require(offset + channel_size + payload_size == size, "invalid record framing")
            channel = record[offset:offset + channel_size].decode("ascii")
            require(channel in {"ws.attempt.v1", "ws.open.v1", "ws.send.v1", "ws.receive.v1",
                                "ws.close.v1", "ws.ping.v1", "ws.pong.v1", "basket.clock.v1"},
                    "unexpected journal channel")
            record_count += 1
            channels[channel] += 1
            if first_ns is None:
                first_ns = mono_ns
            if previous is not None:
                require(mono_ns >= previous[0], "monotonic clock regression")
                if mono_ns - previous[0] > 30_000_000_000:
                    gaps.append({"before_record": record_count,
                                 "previous_channel": previous[2], "channel": channel,
                                 "previous_generation": previous[3], "generation": generation,
                                 "monotonic_gap_ns": mono_ns - previous[0],
                                 "wall_gap_ns": wall_ns - previous[1]})
            entry = generations.setdefault(generation, {
                "generation": generation, "channel_counts": Counter(),
                "receive_count": 0, "receive_first_offset_ns": None,
                "receive_last_offset_ns": None, "receive_envelope_ns": 0,
                "maximum_inter_receive_gap_ns": 0, "last_receive_to_close_ns": None})
            entry["channel_counts"][channel] += 1
            if channel == "ws.receive.v1":
                relative = mono_ns - first_ns
                last_receive = entry["receive_last_offset_ns"]
                if last_receive is not None:
                    entry["maximum_inter_receive_gap_ns"] = max(
                        entry["maximum_inter_receive_gap_ns"], relative - last_receive)
                else:
                    entry["receive_first_offset_ns"] = relative
                entry["receive_last_offset_ns"] = relative
                entry["receive_count"] += 1
                entry["receive_envelope_ns"] = relative - entry["receive_first_offset_ns"]
            if channel == "ws.close.v1" and entry["receive_last_offset_ns"] is not None:
                entry["last_receive_to_close_ns"] = mono_ns - first_ns - entry["receive_last_offset_ns"]
            for basket_id, quote in witnesses.get(mono_ns, []):
                require(channel == "ws.receive.v1", "witness is not a received message")
                mapped_witnesses.append({"basket_id": basket_id, "record": record_count,
                    "generation": generation, "offset_ns": mono_ns - first_ns,
                    "previous_record_gap_ns": None if previous is None else mono_ns - previous[0],
                    "quantity_centicontracts": quote["quantity_centicontracts"],
                    "gross_margin_micro": quote["gross_margin_micro"],
                    "modeled_fees_micro": quote["gross_margin_micro"] - quote["net_margin_micro"],
                    "net_margin_micro": quote["net_margin_micro"],
                    "payout_floor_micro": quote["payout_floor_micro"],
                    "debit_micro": quote["debit_micro"],
                    "maximum_book_update_age_ns": quote["maximum_book_update_age_ns"],
                    "book_update_skew_ns": quote["book_update_skew_ns"],
                    "leg_prices_1e4": [leg["limit_price_1e4"] for leg in quote["legs"]]})
            previous = mono_ns, wall_ns, channel, generation

    require(record_count == summary["records"], "journal/summary record count mismatch")
    require(len(mapped_witnesses) == sum(len(items) for items in witnesses.values()), "unmapped witnesses")
    files = ["result.json", "observation-plan.json", "observation-policy.json", "collector-output.json",
             "preparation/status.json", "session/basket-summary.json", "session/market.journal",
             "session/manifest.json", "session/metadata.json"]
    artifact_sha256 = {name: sha256(root / name) for name in files}
    require(manifest["state"] == "complete" and manifest["records"] == record_count,
            "manifest state/record count mismatch")
    for name in ("journal", "metadata"):
        relative = "session/market.journal" if name == "journal" else "session/metadata.json"
        require(manifest[name]["bytes"] == (root / relative).stat().st_size,
                "manifest " + name + " size mismatch")
        require(manifest[name]["sha256"] == artifact_sha256[relative],
                "manifest " + name + " SHA256 mismatch")
    require(summary["manifest_sha256"] == artifact_sha256["session/manifest.json"],
            "summary/manifest SHA256 mismatch")
    require(summary["metadata_sha256"] == artifact_sha256["session/metadata.json"],
            "summary/metadata SHA256 mismatch")
    for name in ("collector-output.json", "observation-plan.json", "observation-policy.json",
                 "session/basket-summary.json"):
        require(result["artifact_hashes"][name] == artifact_sha256[name],
                "result artifact SHA256 mismatch: " + name)
    return {
        "schema_version": 1,
        "capture_basename": root.name,
        "audit_source_sha256": sha256(Path(__file__)),
        "artifact_sha256": artifact_sha256,
        "manifest_and_result_hash_bindings_verified": True,
        "acceptance": {key: result[key] for key in (
            "capture_started_at", "capture_completed_at", "finalized", "usable", "reason",
            "planned_window_complete", "policy_expired", "live_replay_equal", "source_unchanged")},
        "planned_duration_seconds": plan["planned_duration_seconds"],
        "policy_validity_unix_ms": [policy["valid_from_unix_ms"], policy["valid_until_unix_ms"]],
        "selection_counts": preparation["counts"],
        "summary_counts": {key: summary[key] for key in (
            "records", "book_updates", "public_trades", "basket_evaluations", "evaluated_quantities",
            "episode_events", "maximum_simultaneous_positive_baskets", "orders_sent", "simulated_fills",
            "realized_pnl_micro", "model_classification", "production_certificate")},
        "journal_audit": {
            "all_record_crc32_verified": True, "record_count": record_count,
            "channel_counts": dict(sorted(channels.items())),
            "elapsed_monotonic_ns": previous[0] - first_ns,
            "record_gap_threshold_ns": 30_000_000_000,
            "record_gaps_above_threshold": gaps,
            "generations": list(generations.values()),
            "sum_receive_envelopes_ns": sum(row["receive_envelope_ns"] for row in generations.values()),
            "receive_envelope_is_continuous_synchronized_basket_eligibility": False,
            "gap_cause_deducible_from_journal_alone": False},
        "reported_durations_are_not_verified_continuous_coverage": True,
        "reported_basket_diagnostics": [{
            "basket_id": row["basket_id"], "reported_eligible_ns": row["eligible_ns"],
            "reported_status_duration_ns": row["status_duration_ns"],
            "status_counts": row["status_counts"], "evaluated_quantities": row["evaluated_quantities"]
        } for row in summary["baskets"]],
        "best_one_contract_witnesses": sorted(mapped_witnesses, key=lambda item: item["basket_id"]),
        "economic_interpretation_limits": [
            "Point-in-time conditional quotes, not fills or realized profit.",
            "Best one-contract witnesses are not maxima of gross margin across all sizes.",
            "Reevaluations and sizes are correlated calculations, not independent opportunities.",
            "Receive envelopes include startup snapshots and do not certify synchronized basket coverage.",
            "Old duration counters carry states until late closure; do not use them as valid exposure.",
            "Zero positive quotes applies only to the selected model, observed messages and tested sizes."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_directory", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.capture_directory), indent=2, sort_keys=True))
