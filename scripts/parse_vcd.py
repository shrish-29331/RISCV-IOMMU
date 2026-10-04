#!/usr/bin/env python3
# TempoIOMMU-RV: Structured VCD Parser & Latency Analysis Engine
# File: scripts/parse_vcd.py

import os
import sys
import json
import csv
from vcdvcd import VCDVCD

def parse_vcd_trace(vcd_path, json_out="results/translation_trace.json", csv_out="results/translation_trace.csv"):
    if not os.path.exists(vcd_path):
        print(f"Error: VCD trace file '{vcd_path}' not found.")
        sys.exit(1)

    print(f"[+] Loading VCD trace '{vcd_path}'...")
    vcd = VCDVCD(vcd_path)

    # 1. Discover signals in VCD
    clk_sig = next((s for s in vcd.signals if s.endswith('.clk_i')), None)
    if not clk_sig:
        print("Error: clk_i signal not found in VCD.")
        sys.exit(1)

    # Find clock posedge timestamps to map time (ps) -> cycles
    clk_tv = vcd[clk_sig].tv
    clock_posedges = [t for t, val in clk_tv if val == '1']
    if len(clock_posedges) < 2:
        print("Error: Insufficient clock edges found.")
        sys.exit(1)

    clock_period_ps = clock_posedges[1] - clock_posedges[0]
    first_edge = clock_posedges[0]

    def time_to_cycle(t_ps):
        return int((t_ps - first_edge + clock_period_ps / 2) // clock_period_ps)

    def parse_bin(val):
        if not val or not isinstance(val, str):
            return 0
        v = val.lstrip('b').lstrip('B')
        if any(c in v for c in ('x', 'z', 'X', 'Z')):
            return 0
        try:
            return int(v, 2)
        except ValueError:
            return 0

    def get_signal(sig_name):
        matches = [s for s in vcd.signals if sig_name in s]
        if not matches:
            return None
        return vcd[matches[0]]

    # Target key event signals
    trans_valid_vcd = get_signal("trans_valid")
    trans_error_vcd = get_signal("trans_error")
    spaddr_vcd      = get_signal("spaddr")
    cause_code_vcd  = get_signal("cause_code")
    iotlb_miss_vcd  = get_signal("iotlb_miss")
    ddt_walk_vcd    = get_signal("ddt_walk")
    pdt_walk_vcd    = get_signal("pdt_walk")
    s1_ptw_vcd      = get_signal("s1_ptw")
    s2_ptw_vcd      = get_signal("s2_ptw")
    ds_req_vcd      = get_signal("ds_req_o")
    tr_req_vcd      = get_signal("dev_tr_req_i")

    # 2. Extract DMA Translation Request Intervals
    requests = []

    # If dev_tr_req_i transitions are available:
    if tr_req_vcd:
        active_req = None
        for t, val in tr_req_vcd.tv:
            val_int = parse_bin(val)
            # Check ar_valid (bit 1)
            ar_valid = (val_int >> 1) & 1
            if ar_valid and not active_req:
                start_cycle = time_to_cycle(t)
                device_id = (val_int >> 23) & 0xFFFFFF
                process_id = (val_int >> 2) & 0xFFFFF
                iova = (val_int >> 77) & 0xFFFFFFFFFFFFFFFF
                
                active_req = {
                    "start_time": t,
                    "start_cycle": start_cycle,
                    "device_id": device_id,
                    "process_id": process_id,
                    "iova": iova,
                    "val_int": val_int
                }
            elif not ar_valid and active_req:
                active_req["end_time"] = t
                requests.append(active_req)
                active_req = None

    # 3. Match Requests to Completions / Events
    results = []

    # Collect completion events from dev_tr_resp_o (r_valid = bit 72)
    comp_events = []
    tr_resp_vcd = get_signal("dev_tr_resp_o")
    if tr_resp_vcd:
        prev_r_valid = 0
        for t, val in tr_resp_vcd.tv:
            val_int = parse_bin(val)
            r_valid = (val_int >> 72) & 1
            if r_valid and not prev_r_valid:
                r_resp = (val_int >> 6) & 3
                r_data = (val_int >> 8) & 0xFFFFFFFFFFFFFFFF
                success = (r_resp == 0)
                comp_events.append({
                    "time": t,
                    "cycle": time_to_cycle(t),
                    "success": success,
                    "physical_address": f"0x{r_data:016x}",
                    "resp_code": r_resp
                })
            prev_r_valid = r_valid

    comp_events.sort(key=lambda x: x["time"])

    # Collect Memory Access events (ds_req_o ar_valid = bit 1)
    ds_access_times = []
    if ds_req_vcd:
        for t, val in ds_req_vcd.tv:
            val_int = parse_bin(val)
            if (val_int >> 1) & 1:
                ds_access_times.append(t)

    # Collect event flag pulse times
    def get_pulse_times(sig_obj):
        if not sig_obj:
            return []
        return [t for t, val in sig_obj.tv if val == '1']

    iotlb_miss_times = get_pulse_times(iotlb_miss_vcd)
    ddt_walk_times   = get_pulse_times(ddt_walk_vcd)
    pdt_walk_times   = get_pulse_times(pdt_walk_vcd)
    s1_ptw_times     = get_pulse_times(s1_ptw_vcd)
    s2_ptw_times     = get_pulse_times(s2_ptw_vcd)

    # Helper to check signal hex/bin value at timestamp
    def value_at_time(sig_obj, target_time):
        if not sig_obj:
            return "0"
        val = "0"
        for t, v in sig_obj.tv:
            if t <= target_time:
                val = v
            else:
                break
        return val

    for req_idx, req in enumerate(requests, 1):
        t_start = req["start_time"]
        c_start = req["start_cycle"]

        # Find matching completion event after t_start
        matching_comp = next((c for c in comp_events if c["time"] >= t_start), None)
        
        if matching_comp:
            t_comp = matching_comp["time"]
            c_comp = matching_comp["cycle"]
            lat_cycles = c_comp - c_start
            success = matching_comp["success"]
            physical_address = matching_comp.get("physical_address", "0x0000000000000000")
        else:
            t_comp = t_start + 1000
            c_comp = c_start + 100
            lat_cycles = 100
            success = False
            physical_address = "0x0000000000000000"

        # Extract Cause Code if error
        cause_code = 0
        if cause_code_vcd:
            for t, v in cause_code_vcd.tv:
                if t_start <= t <= t_comp + 50:
                    v_int = parse_bin(v)
                    if v_int != 0:
                        cause_code = v_int
                        break

        # Check events in window [t_start, t_comp]
        in_window = lambda times: any(t_start <= t <= t_comp + 20 for t in times)
        
        iotlb_miss = in_window(iotlb_miss_times)
        iotlb_hit  = not iotlb_miss
        ddt_walk   = in_window(ddt_walk_times)
        pdt_walk   = in_window(pdt_walk_times)
        s1_ptw     = in_window(s1_ptw_times)
        s2_ptw     = in_window(s2_ptw_times)

        ds_count = sum(1 for t in ds_access_times if t_start <= t <= t_comp)

        record = {
            "request_id": req_idx,
            "start_cycle": c_start,
            "completion_cycle": c_comp,
            "latency_cycles": lat_cycles,
            "device_id": req["device_id"],
            "process_id": req["process_id"],
            "iova": f"0x{req['iova']:016x}",
            "transaction_type": "READ",
            "iotlb_hit": iotlb_hit,
            "iotlb_miss": iotlb_miss,
            "ddt_walk": ddt_walk,
            "pdt_walk": pdt_walk,
            "s1_ptw": s1_ptw,
            "s2_ptw": s2_ptw,
            "ds_access_count": ds_count,
            "success": success,
            "error": not success,
            "cause_code": cause_code,
            "physical_address": physical_address
        }
        results.append(record)

    # 4. Print Human-Readable Analysis Report
    print("\n" + "=" * 60)
    print("TempoIOMMU-RV Translation Analysis Report")
    print("=" * 60)

    for i, rec in enumerate(results):
        if len(results) > 100 and i >= 10:
            if i == 10:
                print(f"\n... skipping detailed print for remaining {len(results)-10} requests ...")
            continue
        print(f"\nRequest #{rec['request_id']}")
        print("-" * 60)
        print(f"Device ID             : {rec['device_id']}")
        print(f"Process ID            : {rec['process_id']}")
        print(f"IOVA                  : {rec['iova']}")
        print(f"Transaction           : {rec['transaction_type']}")
        print("")
        print(f"IOTLB                 : {'HIT' if rec['iotlb_hit'] else 'MISS'}")
        print(f"DDT walk              : {'YES' if rec['ddt_walk'] else 'NO'}")
        print(f"PDT walk              : {'YES' if rec['pdt_walk'] else 'NO'}")
        print(f"Stage-1 PTW           : {'YES' if rec['s1_ptw'] else 'NO'}")
        print(f"Stage-2 PTW           : {'YES' if rec['s2_ptw'] else 'NO'}")
        print("")
        print(f"Data-structure reads  : {rec['ds_access_count']}")
        print(f"Start cycle           : {rec['start_cycle']}")
        print(f"Completion cycle      : {rec['completion_cycle']}")
        print(f"Latency               : {rec['latency_cycles']} cycles")
        print("")
        print(f"Result                : {'SUCCESS' if rec['success'] else 'FAILED'}")
        print(f"Physical address      : {rec['physical_address']}")
    print("=" * 60 + "\n")

    # 5. Export JSON and CSV
    os.makedirs(os.path.dirname(json_out), exist_ok=True)
    with open(json_out, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"[+] Saved JSON trace to '{json_out}'")

    with open(csv_out, 'w', newline='') as f:
        if results:
            writer = csv.DictWriter(f, fieldnames=results[0].keys())
            writer.writeheader()
            writer.writerows(results)
    print(f"[+] Saved CSV trace to '{csv_out}'")

if __name__ == '__main__':
    vcd_path = sys.argv[1] if len(sys.argv) > 1 else 'sim_trace.vcd'
    parse_vcd_trace(vcd_path)
