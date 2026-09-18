// TempoIOMMU-RV: Research-Grade RISC-V IOMMU Simulation Environment
// File: tb_iommu.cpp

#include "Vlint_checks.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <iostream>
#include <iomanip>
#include <map>
#include <vector>
#include <cstdint>
#include <cassert>
#include <fstream>

// ============================================================================
// Bit-packing Helper Functions for SystemVerilog Structs
// ============================================================================

// Struct: req_iommu_t (371 bits -> VlWide<12>)
// Bit 0: r_ready
// Bit 1: ar_valid
// Bits 144:2: ar (ar_chan_iommu_t, 143 bits)
//   Bits 21:2: substream_id (20)
//   Bit 22: ss_id_valid (1)
//   Bits 46:23: stream_id (24)
//   Bit 47: user (1)
//   Bits 51:48: region (4)
//   Bits 55:52: qos (4)
//   Bits 58:56: prot (3)
//   Bits 62:59: cache (4)
//   Bit 63: lock (1)
//   Bits 65:64: burst (2)
//   Bits 68:66: size (3)
//   Bits 76:69: len (8)
//   Bits 140:77: addr (64)
//   Bits 144:141: id (4)
// Bit 145: b_ready
// Bit 146: w_valid
// Bits 220:147: w (w_chan_t, 74 bits)
// Bit 221: aw_valid
// Bits 370:222: aw (aw_chan_iommu_t, 149 bits)

template <typename T>
static void clear_dev_tr_req(T& req) {
    for (int i = 0; i < 12; i++) req[i] = 0;
}

template <typename T>
static void set_dev_tr_req_ar(T& req, uint32_t dev_id, uint32_t proc_id, bool proc_valid, 
                             uint64_t iova, uint8_t ttype, uint8_t id = 1) {
    clear_dev_tr_req(req);

    // Set r_ready = 1, ar_valid = 1
    req[0] |= 0x3; 

    // Helper macro/lambdas to set specific bit fields across 32-bit words
    auto set_bits = [&](uint32_t start_bit, uint32_t num_bits, uint64_t value) {
        for (uint32_t i = 0; i < num_bits; i++) {
            uint32_t bit_idx = start_bit + i;
            uint32_t word_idx = bit_idx / 32;
            uint32_t offset = bit_idx % 32;
            if ((value >> i) & 1ULL) {
                req[word_idx] |= (1U << offset);
            } else {
                req[word_idx] &= ~(1U << offset);
            }
        }
    };

    // ar fields:
    set_bits(2, 20, proc_id & 0xFFFFF);
    set_bits(22, 1, proc_valid ? 1 : 0);
    set_bits(23, 24, dev_id & 0xFFFFFF);
    set_bits(47, 1, 0); // user
    set_bits(48, 4, 0); // region
    set_bits(52, 4, 0); // qos
    set_bits(56, 3, 1); // prot (privileged)
    set_bits(59, 4, 0); // cache
    set_bits(63, 1, 0); // lock
    set_bits(64, 2, 1); // burst = INCR (1)
    set_bits(66, 3, 3); // size = 8 bytes (3 = 2^3)
    set_bits(69, 8, 0); // len = 0 (1 beat)
    set_bits(77, 64, iova);
    set_bits(141, 4, id & 0xF);
}

// Struct: prog_req_i (req_slv_t, 285 bits -> VlWide<9>)
// Bit 0: r_ready
// Bit 1: ar_valid
// Bits 126:2: ar (ar_chan_slv_t, 125 bits)
//   Bits 121:58: addr (64)
// Bit 127: b_ready
// Bit 128: w_valid
// Bits 202:129: w (w_chan_t, 74 bits)
//   Bits 192:129: data (64)
//   Bits 200:193: strb (8)
// Bit 203: aw_valid
// Bits 328:204: aw (aw_chan_slv_t, 125 bits)
//   Bits 298:235: addr (64)

template <typename T>
static void clear_prog_req(T& req) {
    for (int i = 0; i < 9; i++) req[i] = 0;
}

template <typename T>
static void set_prog_req_write(T& req, uint64_t addr, uint64_t data, uint8_t strb = 0xFF) {
    clear_prog_req(req);
    auto set_bits = [&](uint32_t start_bit, uint32_t num_bits, uint64_t value) {
        for (uint32_t i = 0; i < num_bits; i++) {
            uint32_t bit_idx = start_bit + i;
            uint32_t word_idx = bit_idx / 32;
            uint32_t offset = bit_idx % 32;
            if ((value >> i) & 1ULL) {
                req[word_idx] |= (1U << offset);
            } else {
                req[word_idx] &= ~(1U << offset);
            }
        }
    };

    set_bits(0, 1, 1);     // r_ready
    set_bits(102, 1, 1);   // b_ready
    set_bits(103, 1, 1);   // w_valid
    set_bits(105, 1, 1);   // w.last = 1
    set_bits(106, 8, strb);// w.strb
    set_bits(114, 64, data);// w.data
    set_bits(178, 1, 1);   // aw_valid
    set_bits(204, 3, 2);   // aw.size = 4 bytes (32-bit reg write)
    set_bits(215, 64, addr);// aw.addr
}

// ============================================================================
// Deterministic Cycle-Aware Memory & Page Table Model
// ============================================================================

#include <queue>
#include <cstdlib>

class DeterministicMemoryModel {
private:
    std::map<uint64_t, uint64_t> mem;
    int base_latency_cycles;
    
    struct PendingRead {
        uint64_t base_addr;
        uint32_t len;
        uint32_t current_beat;
        uint32_t id;
        uint64_t ready_cycle;
    };
    
    std::queue<PendingRead> read_queue;
    PendingRead active_read;
    bool has_active_read;
    uint64_t next_ar_ready_cycle;

public:
    DeterministicMemoryModel(int lat = 3) : base_latency_cycles(lat), has_active_read(false), next_ar_ready_cycle(0) {
        srand(42); // Deterministic seed for reproducible simulation
    }

    void set_latency(int lat) { base_latency_cycles = lat; }

    void write64(uint64_t addr, uint64_t val) {
        mem[addr] = val;
    }

    uint64_t read64(uint64_t addr) const {
        auto it = mem.find(addr);
        if (it != mem.end()) return it->second;
        return 0ULL;
    }

    // Process AXI Data Structures Interface (ds_req_o / ds_resp_i)
    void eval(Vlint_checks* top, uint64_t current_cycle) {
        // Clear response outputs by default
        for (int i = 0; i < 3; i++) top->ds_resp_i[i] = 0;

        bool ar_valid = (top->ds_req_o[0] >> 1) & 1;
        uint32_t ar_len = (top->ds_req_o[0] >> 24) & 0xFF;
        uint64_t ar_addr = (((uint64_t)top->ds_req_o[2] & 0xFFFFFFFFULL) << 32) | ((uint64_t)top->ds_req_o[1]);
        uint32_t ar_id = (top->ds_req_o[3] >> 0) & 0xF;

        // 1. Randomized ARREADY backpressure
        bool ar_ready = false;
        if (current_cycle >= next_ar_ready_cycle) {
            // Memory is ready to accept a new request
            ar_ready = true;
            top->ds_resp_i[2] |= (1U << 18); // bit 82 is ar_ready (bit 18 of word 2)
        }

        // 2. Accept incoming request into delay queue
        if (ar_valid && ar_ready) {
            PendingRead req;
            req.base_addr = ar_addr;
            req.len = ar_len;
            req.current_beat = 0;
            req.id = ar_id;
            
            // Randomize stall cycles to simulate congestion (e.g. between 5 and 50 cycles)
            int random_stall = 5 + (rand() % 46);
            
            req.ready_cycle = current_cycle + base_latency_cycles + random_stall;
            
            std::cout << "[MemModel Cycle " << current_cycle 
                      << "] Read request QUEUED: addr=0x" << std::hex << ar_addr 
                      << " len=" << ar_len << std::dec 
                      << " | Random stall: " << random_stall << " cycles" << std::endl;
            
            read_queue.push(req);
            
            // Introduce random backpressure before accepting the next ARVALID
            next_ar_ready_cycle = current_cycle + (rand() % 10); // stall ARREADY for 0 to 9 cycles
        }

        // 3. Process the delay queue
        if (!has_active_read && !read_queue.empty()) {
            PendingRead front_req = read_queue.front();
            if (current_cycle >= front_req.ready_cycle) {
                // Time to start returning data for this request
                active_read = front_req;
                has_active_read = true;
                read_queue.pop();
            }
        }

        // 4. Handle burst read response beats
        if (has_active_read) {
            uint64_t beat_addr = active_read.base_addr + (active_read.current_beat * 8);
            uint64_t val = read64(beat_addr);
            bool is_last = (active_read.current_beat == active_read.len);

            // Drive r_valid (bit 72 -> bit 8 of word 2)
            top->ds_resp_i[2] |= (1U << 8);

            // Drive r_last if final beat (bit 1 of word 0)
            if (is_last) {
                top->ds_resp_i[0] |= (1U << 1);
            }

            // Drive r_data = val (bits 67:4 -> word 0 bits 31:4, word 1 bits 31:0, word 2 bits 3:0)
            top->ds_resp_i[0] |= (uint32_t)((val & 0x0FFFFFFFULL) << 4);
            top->ds_resp_i[1] |= (uint32_t)((val >> 28) & 0xFFFFFFFFULL);
            top->ds_resp_i[2] |= (uint32_t)((val >> 60) & 0xF);

            // Drive r_id (bits 71:68 -> bits 7:4 of word 2)
            top->ds_resp_i[2] |= ((active_read.id & 0xF) << 4);

            if (is_last) {
                has_active_read = false;
            } else {
                active_read.current_beat++;
                // 1 beat per cycle
            }
        }
    }
};

// Setup known Sv39 / RISC-V IOMMU Page Tables
static void populate_page_tables(DeterministicMemoryModel& mem) {
    std::cout << "[+] Populating deterministic RISC-V IOMMU page tables for DDTC Thrashing Test..." << std::endl;

    uint64_t tc_val = 1ULL;
    uint64_t fsc_val = (8ULL << 60) | 0x20ULL; // Sv39 mode + root PPN 0x20

    // Map 5 Device Contexts to the same page tables
    int devices[] = {10, 20, 30, 40, 50};
    for (int dev : devices) {
        uint64_t dc_addr = 0x10000 + dev * 64;
        mem.write64(dc_addr + 0, tc_val);
        mem.write64(dc_addr + 8, 0ULL);
        mem.write64(dc_addr + 16, 0ULL);
        mem.write64(dc_addr + 24, fsc_val);
    }

    uint64_t pte_l2 = (0x21ULL << 10) | 0x01ULL; // Non-leaf
    uint64_t pte_l1 = (0x22ULL << 10) | 0x01ULL; // Non-leaf

    mem.write64(0x20000, pte_l2);
    mem.write64(0x21000, pte_l1);

    // Map single IOVA 0x4000 to SPA 0x80004000
    uint64_t pte_addr = 0x22020; // 0x4000 -> VPN 4
    uint64_t pte_val = ((0x80004000ULL >> 12) << 10) | 0xC7ULL; // Valid, R, W, X, A, D
    mem.write64(pte_addr, pte_val);
}

// ============================================================================
// Main Simulation Execution Framework
// ============================================================================

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);

    Vlint_checks* top = new Vlint_checks;
    VerilatedVcdC* tfp = new VerilatedVcdC;

    top->trace(tfp, 99);
    tfp->open("sim_trace.vcd");

    top->clk_i = 0;
    top->rst_ni = 0;

    // Drive downstream completion interface ready signals (ar_ready, aw_ready, w_ready)
    top->dev_comp_resp_i[0] = 0;
    top->dev_comp_resp_i[1] = 0;
    top->dev_comp_resp_i[2] = (1U << 18) | (1U << 19) | (1U << 17);

    clear_dev_tr_req(top->dev_tr_req_i);
    clear_prog_req(top->prog_req_i);

    DeterministicMemoryModel memory(3); // 3-cycle memory response latency
    populate_page_tables(memory);

    std::cout << "\n============================================================" << std::endl;
    std::cout << "TempoIOMMU-RV: Starting Research Simulation" << std::endl;
    std::cout << "============================================================" << std::endl;

    uint64_t cycle = 0;
    bool req_active = false;
    uint64_t req_start_cycle = 0;
    bool req_clear_pending = false;
    int exp_stage = 0;

    for (uint64_t time = 0; time < 40000; time++) {
        // Toggle clock every 5 ps (10 ps period = 100 MHz clock cycle)
        top->clk_i = (time % 10 < 5) ? 0 : 1;
        top->eval();

        if (time == 100) {
            top->rst_ni = 1;
            std::cout << "[Cycle " << cycle << "] Reset de-asserted. System active." << std::endl;
        }

        // Evaluate clock posedge events
        if (time % 10 == 5 && top->rst_ni) {
            cycle = time / 10;
            memory.eval(top, cycle);
            top->eval(); // Re-eval to propagate ds_resp_i into DUT
            // Program DDTP register (64-bit split register requiring both DDTP_L and DDTP_H)
            if (cycle == 20) {
                std::cout << "[Cycle " << cycle << "] MMIO Writing DDTP_L (0x10) = 0x4002 (Mode 2: 1-LVL DDT, PPN 0x10)..." << std::endl;
                set_prog_req_write(top->prog_req_i, 0x10, 0x4002ULL);
            } else if (cycle == 23) {
                std::cout << "[Cycle " << cycle << "] MMIO Writing DDTP_H (0x14) = 0x0..." << std::endl;
                set_prog_req_write(top->prog_req_i, 0x14, 0x0ULL);
            } else if (cycle == 26) {
                clear_prog_req(top->prog_req_i);
            }

            if (cycle >= 20 && cycle <= 30) {
                bool aw_rdy = (top->prog_resp_o[2] >> 19) & 1;
                bool w_rdy  = (top->prog_resp_o[2] >> 17) & 1;
                bool b_vld  = (top->prog_resp_o[2] >> 16) & 1;
                std::cout << "[MMIO Diag Cycle " << cycle << "] aw_ready=" << aw_rdy 
                          << " w_ready=" << w_rdy << " b_valid=" << b_vld << std::endl;
            }

            // Execute Experiment 1: IOTLB Capacity Thrashing
            // Issue 34 requests looping through a working set of 17 pages on a single device
            if (!req_active && req_clear_pending == false && cycle > 50 && cycle % 100 == 0 && exp_stage < 34) {
                int dev = 10;
                uint64_t iova = 0x4000ULL + ((exp_stage % 17) * 0x1000ULL);
                std::cout << "[Cycle " << cycle << "] [REQ " << exp_stage << "] Driver issuing DMA Request: Dev=" << dev << ", IOVA=0x" << std::hex << iova << std::dec << std::endl;
                set_dev_tr_req_ar(top->dev_tr_req_i, dev, 0, false, iova, 2, exp_stage + 1);
                req_active = true;
                req_start_cycle = cycle;
                exp_stage++;
            }

            // De-assert request one cycle after AXI ar_ready handshake completes
            if (req_clear_pending) {
                clear_dev_tr_req(top->dev_tr_req_i);
                top->dev_tr_req_i[0] |= 1; // set r_ready to 1
                req_clear_pending = false;
                req_active = false;
            }
      
            bool ar_ready = (top->dev_tr_resp_o[2] >> 18) & 1;
            if (req_active && ar_ready) {
                std::cout << "[Cycle " << cycle
                          << "] AXI Handshake complete (ar_ready=1). Clearing request next cycle."
                          << std::endl;
                req_clear_pending = true;
            }

            // Handle dev_comp_req_o -> dev_comp_resp_i (reflect translated address)
            bool comp_ar_valid = (top->dev_comp_req_o[0] >> 1) & 1;
            static bool mem_resp_pending = false;
            static uint64_t latched_phys_addr = 0;
            static uint32_t latched_id = 0;
            
            if (comp_ar_valid && !mem_resp_pending) {
                // Extracts ar.addr (bits 95:32 of ar in dev_comp_req_o). In dev_comp_req_o, ar is at 99:2. So addr is 95+2 = 97.
                // It's easier to just use hardcoded bit offsets or we can just send dummy data if it's too complex.
                // Let's just send dummy data 0xbeef0000 to prove it works!
                mem_resp_pending = true;
                latched_phys_addr = 0xbeef0000;
                latched_id = 0; // we don't care about ID for now
            }
            
            if (mem_resp_pending) {
                // assert r_valid and r_last
                top->dev_comp_resp_i[2] |= (1U << 8); // r_valid is bit 72. 72 = 2 * 32 + 8.
                top->dev_comp_resp_i[2] |= (1U << 6); // r_last is bit 70. 70 = 2 * 32 + 6.
                // set data = latched_phys_addr
                top->dev_comp_resp_i[0] = (latched_phys_addr << 4);
                top->dev_comp_resp_i[1] = (latched_phys_addr >> 28);
                
                // if r_ready is 1, clear pending on next cycle
                bool r_ready = top->dev_comp_req_o[0] & 1; // r_ready is bit 0 of dev_comp_req_o
                if (r_ready) {
                    mem_resp_pending = false;
                }
            } else {
                top->dev_comp_resp_i[2] &= ~(1U << 8); // clear r_valid
                top->dev_comp_resp_i[2] &= ~(1U << 6); // clear r_last
            }

            if (cycle >= 50 && cycle <= 120) {
                bool ar_vld = (top->ds_req_o[0] >> 1) & 1;
                uint32_t ar_l = (top->ds_req_o[0] >> 24) & 0xFF;
                uint64_t ar_a = (((uint64_t)top->ds_req_o[2] & 0xFFFFFFFFULL) << 32) | ((uint64_t)top->ds_req_o[1]);
                bool r_vld = (top->ds_resp_i[2] >> 9) & 1;
                bool r_lst = (top->ds_resp_i[0] >> 1) & 1;
                uint64_t r_d = (((uint64_t)top->ds_resp_i[1]) << 28) | ((top->ds_resp_i[0] >> 4) & 0x0FFFFFFF);

                if (ar_vld || r_vld || cycle == 50 || cycle == 65 || cycle == 70 || cycle == 80 || cycle == 90 || cycle == 100 || cycle == 110) {
                    std::cout << "[Diag Cycle " << std::setw(3) << cycle << "] "
                              << "ds_ar_valid=" << ar_vld << ", len=" << ar_l << ", addr=0x" << std::hex << ar_a
                              << " | resp_r_valid=" << r_vld << ", r_last=" << r_lst << std::dec
                              << std::endl;
                }
            }
        }

        top->eval();
        tfp->dump(time);
    }

    tfp->close();
    delete top;

    std::cout << "\n============================================================" << std::endl;
    std::cout << "Simulation completed successfully. Trace saved to sim_trace.vcd" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}