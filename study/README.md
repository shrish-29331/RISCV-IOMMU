# TempoIOMMU-RV Study & Research Notes

This directory contains documentation, theoretical background, experimental setups, and empirical findings for micro-architectural contention modeling performed on the RISC-V IOMMU implementation (`TempoIOMMU-RV`).

These experiments are modeled after real-world IOMMU performance bottlenecks and non-deterministic latency phenomena analyzed in high-performance system research (such as the ARM SMMU architecture study).

---

## Documentation

* [Simulation Workflow & Methodology](Simulation_Workflow.md): A high-level guide on how hardware/testbench parameters are configured, and the execution flow from Verilator compilation to trace parsing and plotting.

---

## Table of Contents & Experiment Overview

### 1. [Experiment 1: IOTLB Capacity Thrashing](IOTLB_Capacity_Thrashing.md)
* **Focus**: Page Table Translation Cache Thrashing (Micro-benchmark).
* **Mechanism**: Evaluating IOTLB behavior when an I/O device working set ($N + 1$ pages) exceeds the physical capacity of the IOTLB ($N = 16$).
* **Volume**: **5,000 requests** across a 17-page working set (~294 full passes).
* **Key Finding**: Working set overflow drops the IOTLB hit rate to 0%, causing 100% cyclic capacity misses and increasing request latency from 2 cycles (hit path) to 15 cycles (PTW penalty). This is perfectly sustained across all 5,000 requests with zero variance.

---

### 2. [Experiment 2: Multi-Device Context Contention](Multi_Device_Contention.md)
* **Focus**: Device Directory Table Cache (DDTC) thrashing under multi-tenant I/O workloads (`interf_tbu` analog).
* **Mechanism**: Interleaving DMA translation requests across 5 active virtual devices (`device_id` = 10, 20, 30, 40, 50) when the DDTC capacity is constrained ($N = 4$).
* **Volume**: **5,000 requests** in round-robin across 5 devices (1,000 full rounds).
* **Key Finding**: Even with a **99.98% IOTLB hit rate** (4,999/5,000 requests), concurrent device context evictions force a 14-cycle Device Directory Walk on **100% of transactions**. Proves that DDTC sizing is a critical bottleneck in multi-tenant SoCs.

---

### 3. [Experiment 3: Interconnect Memory Contention](Interconnect_Memory_Contention.md)
* **Focus**: Impact of System Interconnect Latency & Congestion on IOMMU walks.
* **Mechanism**: Injecting randomized AXI interconnect stalls (5–50 cycles) on the memory slave interface during Device Directory and Page Table walks.
* **Volume**: **5,000 requests** across 5 devices with randomized memory backpressure.
* **Key Finding**: Interconnect congestion causes non-deterministic execution spikes — DDT walk latencies stretch from 14 cycles (Exp 2 baseline) to a **P50 of 42 cycles** and **P99 of 64 cycles**, with worst-case tail latencies of **113 cycles** (8× the uncontended baseline).

---

## Summary Matrix

| Experiment | Requests | IOTLB Hit Rate | DDT Walk Rate | Mean Latency | P95 | Max | Root Bottleneck |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **Exp 1: IOTLB Thrashing** | 5,000 | 0% | ~0% | 15.0 cy | 15.0 cy | 27 cy | Page Table Walk (every tx) |
| **Exp 2: DDTC Contention** | 5,000 | 99.98% | 100% | 14.0 cy | 14.0 cy | 27 cy | Device Context Walk (every tx) |
| **Exp 3: Interconnect Stalls** | 5,000 | 99.98% | 100% | 41.4 cy | 62.0 cy | 113 cy | AXI Bus Backpressure (non-deterministic) |

---

## How to Run Each Experiment

> **Prerequisites**: Ensure the project is compiled. Run `make build` once before running experiments.

### Experiment 1 — IOTLB Capacity Thrashing
```bash
make exp1
```
This runs the simulation with `tb1_iommu.cpp` (5,000 requests, Device 10, 17-page working set), then automatically parses the VCD, computes stats, and generates plots.

### Experiment 2 — Multi-Device Context Contention
```bash
make exp2
```
This runs the simulation with `tb2_iommu.cpp` (5,000 requests, 5 rotating devices, fixed-latency memory model), then automatically parses, computes stats, and generates plots.

### Experiment 3 — Interconnect Memory Contention
```bash
make exp3
```
This runs the simulation with `tb3_iommu.cpp` (5,000 requests, 5 rotating devices, randomized stall memory model), then automatically parses, computes stats, and generates plots.

### Run Steps Individually
Each experiment target is a shortcut for these four steps:
```bash
make build   # Compile RTL + testbench (once, or after any code change)
make sim1    # (or sim2, sim3) — run the simulation binary
make parse   # Parse sim_trace.vcd → results/translation_trace.json + .csv
make stats   # Compute mean/P50/P95/P99/max latency statistics
make plot    # Generate PNG plots in results/
```

### Output Files
All results are written to the `results/` directory:
| File | Description |
|------|-------------|
| `results/translation_trace.json` | Per-request trace with latency, IOTLB hit/miss, DDT walk, result |
| `results/translation_trace.csv`  | Same data in CSV format for plotting |
| `results/latency_by_request.png` | Timeline plot of latency per request number |
| `results/latency_distribution.png` | Histogram of latency values |
| `results/latency_hit_vs_miss.png` | Boxplot comparing hit vs. miss latency |
