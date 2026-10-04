# Experiment 2: Multi-Device Context Contention (interf_tbu Analog)

## Theoretical Background
In addition to caching page translations in the IOTLB, the RISC-V IOMMU also caches **Device Contexts** in the Device Directory Table Cache (DDTC). Device contexts contain essential attributes including the root Page Table Pointer (FSC) and process/domain identifiers (GSCID/PSCID).

When multiple devices attempt to perform DMA operations simultaneously, their transactions interleave. If the number of concurrently active devices exceeds the capacity of the DDTC, the devices will continuously evict each other's context entries from the cache.

This leads to severe performance degradation where every transaction must suffer a Device Directory Walk (typically 14 cycles) to fetch the device context, *even if the target memory page translation is already cached in the IOTLB*. This is a direct RISC-V IOMMU analog of the `interf_tbu` bottleneck observed in ARM SMMU literature.

## Setup
- **Target IP**: `riscv_iommu` (TempoIOMMU-RV)
- **DDTC Capacity**: `DDTC_ENTRIES = 4` (set in `lint_checks.sv` instantiation parameters).
- **Working Set**: 5 distinct devices (`device_id` = 10, 20, 30, 40, 50).
- **Test Pattern**: The testbench issues **5,000 requests** in a round-robin interleaved fashion across the 5 devices (1,000 full rounds). All devices request translations for the *exact same memory page* (IOVA `0x4000`).
- **Testbench**: [`tb2_iommu.cpp`](../tb2_iommu.cpp)
- **AXI ID Scheme**: Dynamic rotating IDs (`exp_stage % 8`) to sustain thousands of back-to-back transactions without saturating the AXI demux ID counters.

## Expected Behavior
- **Request #1 (Device 10, cold start)**: Full DDT walk + Stage-1 PTW. Translation cached in IOTLB.
- **Requests #2–5 (Devices 20–50)**: IOTLB HIT (same IOVA), but DDT walk required (DDTC cold miss for each new device context).
- **Requests #6+ (round-robin repeating)**: IOTLB remains a hit for all devices. However, because 5 devices > DDTC capacity of 4, the PLRU replacement policy continuously evicts device contexts. Every transaction requires a DDT walk even though the page translation is cached.

## Results (5,000 Requests)

| Metric | Value |
|--------|-------|
| Total Requests | **5,000** |
| IOTLB Hits | **4,999 (99.98%)** |
| IOTLB Misses | 1 (initial cold miss, Request #1) |
| DDT Walks | **5,000 (100%)** |
| Stage-1 PTW | 1 (initial cold miss only) |
| Successful Translations | **5,000 (100%)** |
| Failed Translations | 0 |
| Mean Latency | **14.0 cycles** |
| P50 Latency | 14.0 cycles |
| P95 Latency | 14.0 cycles |
| P99 Latency | 14.0 cycles |
| Max Latency | **27 cycles** (Request #1: DDT + PTW cold start) |

### Analysis
Across all 5,000 requests, the IOTLB successfully cached the single shared page translation (`0x4000`) after the first request. This produced a near-perfect **99.98% IOTLB hit rate** — demonstrating that the IOTLB is working optimally for this workload pattern.

However, the **DDT walk rate was 100%** across every single transaction. Because 5 competing devices continuously evict each other from the 4-entry DDTC, the IOMMU is forced to re-fetch device context for every request. The impact is a flat **14-cycle floor** imposed on every transaction — impossible to eliminate regardless of IOTLB hit rate.

```text
Example from trace (Request #2500):
------------------------------------------------------------
Device ID             : 20
IOVA                  : 0x0000000000004000
IOTLB                 : HIT      ← page translation cached
DDT walk              : YES      ← device context evicted by contention
Stage-1 PTW           : NO
Latency               : 14 cycles
Result                : SUCCESS
```

### Comparison vs. Experiment 1

| Metric | Exp 1 (IOTLB Thrash) | Exp 2 (DDTC Thrash) |
|--------|----------------------|----------------------|
| IOTLB Hit Rate | 0% | **99.98%** |
| DDT Walk Rate | 0.02% (cold only) | **100%** |
| Mean Latency | 15.0 cycles | **14.0 cycles** |
| Root Bottleneck | Page Table Walk | **Device Context Walk** |

### Key Takeaway
> **Optimizing the IOTLB capacity is insufficient if the DDTC size does not comfortably exceed the number of actively interleaving DMA agents.** At 5,000 requests, this experiment proves that DDTC contention from even 5 devices causes a persistent, unavoidable latency floor across the entire workload. The IOMMU's performance is bounded by its weakest cache.
