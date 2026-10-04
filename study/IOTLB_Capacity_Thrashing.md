# Experiment 1: IOTLB Capacity Thrashing

## Theoretical Background
In IOMMUs (such as the ARM SMMU or the RISC-V IOMMU), the I/O Translation Lookaside Buffer (IOTLB) caches recently used page translations to avoid expensive multi-cycle Page Table Walks (PTWs) to main memory.

When a device streams data sequentially or accesses a working set of memory that fits entirely within the IOTLB (the "fast path"), latency is typically very low (e.g., 2 cycles). 

However, if a device generates interleaved requests across a working set of $N+1$ unique pages (where $N$ is the maximum number of entries in the IOTLB), the IOMMU's cache replacement policy (typically LRU or FIFO) will begin to thrash. Every new request will cause a cache miss and evict the oldest entry. By the time the device loops back to the first page, its translation will have been evicted. This drops the hit rate to 0%, penalizing every transaction with the full PTW latency.

## Setup
- **Target IP**: `riscv_iommu` (TempoIOMMU-RV)
- **IOTLB Capacity** (`IOTLB_ENTRIES`): Set to `16` in `riscv_iommu.sv`.
- **Working Set**: $N+1 = 17$ unique 4KB pages allocated to Device 10.
- **Test Pattern**: The testbench issues **5,000 requests** with a rotating 17-page working set (cycling through pages `0x4000`–`0x14000`), completing approximately **294 full passes** through the working set.
- **Testbench**: [`tb1_iommu.cpp`](../tb1_iommu.cpp)
- **AXI ID Scheme**: Dynamic rotating IDs (`exp_stage % 8`) to prevent AXI demux counter saturation across thousands of back-to-back transactions.

## Expected Behavior
- **Pass 1 (Requests 0–16)**: All 17 requests encounter a cold IOTLB miss and trigger a PTW. (Hit rate: 0%).
- **Pass 2+ (Requests 17–4999)**: Because the working set (17) > cache capacity (16), the translation needed for each new pass is evicted before the loop repeats. Thus, all requests on every subsequent pass *also* miss. (Sustained hit rate: 0%).

## Results (5,000 Requests)

| Metric | Value |
|--------|-------|
| Total Requests | **5,000** |
| IOTLB Hits | 0 |
| IOTLB Misses | 5,000 |
| DDT Walks | 1 (cold start only) |
| Stage-1 PTW | 5,000 |
| Successful Translations | **5,000 (100%)** |
| Failed Translations | 0 |
| Mean Latency | **15.0 cycles** |
| P50 Latency | 15.0 cycles |
| P95 Latency | 15.0 cycles |
| P99 Latency | 15.0 cycles |
| Max Latency | **27 cycles** (first request: DDT cold miss) |

### Analysis
At high volume, the 17-page working set against a 16-entry IOTLB produces a perfectly **deterministic, sustained 0% IOTLB hit rate** across all 5,000 requests. With no variance in the memory model (fixed 3-cycle latency), every PTW completes in exactly 15 cycles, producing an ultra-tight, zero-variance latency distribution.

The single 27-cycle maximum latency spike corresponds to the very first request (Request #1), which incurred both a DDT walk (fetching Device Context) and a Stage-1 PTW, totalling 4 sequential memory reads. All subsequent 4,999 requests avoided the DDT walk since the Device Context remained cached in the DDTC.

This experiment conclusively demonstrates that **a working set exceeding cache capacity by even one page is sufficient to destroy the IOTLB hit rate entirely**, regardless of traffic volume or access pattern. At scale, this results in every single peripheral transaction paying the full PTW penalty.

### Latency Distribution
```
                            IOTLB Miss (PTW) = 15 cycles (constant)
                            ↓
     ████████████████████████████████████████  ← 4,999 requests
     │
     │ 1 outlier at 27 cycles (DDT + PTW cold start)
```

### Key Takeaway
> IOTLB capacity must be sized to comfortably **exceed the working set** of all active DMA agents. A single-entry shortfall (capacity = 16, working set = 17) eliminates caching entirely, forcing 100% PTW rate at any scale.
