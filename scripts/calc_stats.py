import sys
import json
import statistics
import os

def calculate_stats(data, name):
    if not data:
        return f"{name:<25} 0     -       -       -       -       -"
    
    n = len(data)
    mean = statistics.mean(data)
    median = statistics.median(data)
    p90 = sorted(data)[int(n * 0.90)] if n > 0 else 0
    p95 = sorted(data)[int(n * 0.95)] if n > 0 else 0
    p99 = sorted(data)[int(n * 0.99)] if n > 0 else 0
    max_val = max(data)
    
    return f"{name:<25} {n:<5} {mean:<7.1f} {median:<7.1f} {p95:<7.1f} {p99:<7.1f} {max_val:<7.1f}"

def generate_stats(json_file):
    try:
        with open(json_file, 'r') as f:
            reqs = json.load(f)
    except Exception as e:
        print(f"Failed to load {json_file}: {e}")
        return

    # Filter out requests with no latency (e.g. unfinished)
    valid_reqs = [r for r in reqs if r.get('latency_cycles') is not None]

    is_true = lambda val: val is True or str(val).upper() in ('YES', 'TRUE', '1')
    is_false = lambda val: val is False or str(val).upper() in ('NO', 'FALSE', '0')

    all_lats = [r['latency_cycles'] for r in valid_reqs]
    hit_lats = [r['latency_cycles'] for r in valid_reqs if is_true(r.get('iotlb_hit')) or is_false(r.get('iotlb_miss'))]
    miss_lats = [r['latency_cycles'] for r in valid_reqs if is_true(r.get('iotlb_miss'))]
    ddt_lats = [r['latency_cycles'] for r in valid_reqs if is_true(r.get('ddt_walk'))]
    ptw_s1_lats = [r['latency_cycles'] for r in valid_reqs if is_true(r.get('s1_ptw'))]
    ptw_s2_lats = [r['latency_cycles'] for r in valid_reqs if is_true(r.get('s2_ptw'))]
    succ_lats = [r['latency_cycles'] for r in valid_reqs if is_true(r.get('success'))]
    fail_lats = [r['latency_cycles'] for r in valid_reqs if is_false(r.get('success'))]

    print("TempoIOMMU-RV Latency Statistics")
    print()
    print(f"{'Category':<25} {'N':<5} {'Mean':<7} {'P50':<7} {'P95':<7} {'P99':<7} {'Max':<7}")
    print("-" * 65)
    print(calculate_stats(hit_lats, "IOTLB hit"))
    print(calculate_stats(miss_lats, "IOTLB miss"))
    print(calculate_stats(ddt_lats, "DDT walk"))
    print(calculate_stats(ptw_s1_lats, "Stage-1 PTW"))
    print(calculate_stats(ptw_s2_lats, "Stage-2 PTW"))
    print(calculate_stats(succ_lats, "Successful"))
    print(calculate_stats(fail_lats, "Failed"))
    print(calculate_stats(all_lats, "All translations"))

if __name__ == '__main__':
    json_path = sys.argv[1] if len(sys.argv) > 1 else 'results/translation_trace.json'
    generate_stats(json_path)
