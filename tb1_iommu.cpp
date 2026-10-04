// TempoIOMMU-RV: Research-Grade RISC-V IOMMU Simulation Environment
// File: tb1_iommu.cpp
// Experiment 1: IOTLB Capacity Thrashing
//
// Setup:
//   - Single device (device_id = 10)
//   - Working set: 17 unique 4KB pages (N+1 where N = IOTLB_ENTRIES = 16)
//   - 10000 total requests: multiple passes over the 17-page working set
//   - NO random stalls injected into memory model (clean bus)
//
// Expected:
//   - Pass 1 (REQ 0-16):  All cold misses -> full PTW (~15 cycles each)
//   - Pass 2 (REQ 17-33): All capacity misses -> full PTW again
//   - Hit Rate: 0%

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
#include <queue>

template <typename T>
static void clear_dev_tr_req(T& req) {
    for (int i = 0; i < 12; i++) req[i] = 0;
}

template <typename T>
static void set_dev_tr_req_ar(T& req, uint32_t dev_id, uint32_t proc_id, bool proc_valid,
                              uint64_t iova, uint8_t ttype, uint8_t id = 1) {
    clear_dev_tr_req(req);
    req[0] |= 0x3;
    auto set_bits = [&](uint32_t start_bit, uint32_t num_bits, uint64_t value) {
        for (uint32_t i = 0; i < num_bits; i++) {
            uint32_t bit_idx = start_bit + i;
            uint32_t word_idx = bit_idx / 32;
            uint32_t offset = bit_idx % 32;
            if ((value >> i) & 1ULL) req[word_idx] |= (1U << offset);
            else req[word_idx] &= ~(1U << offset);
        }
    };
    set_bits(2, 20, proc_id & 0xFFFFF); set_bits(22, 1, proc_valid ? 1 : 0);
    set_bits(23, 24, dev_id & 0xFFFFFF); set_bits(47, 1, 0); set_bits(48, 4, 0);
    set_bits(52, 4, 0); set_bits(56, 3, 1); set_bits(59, 4, 0); set_bits(63, 1, 0);
    set_bits(64, 2, 1); set_bits(66, 3, 3); set_bits(69, 8, 0);
    set_bits(77, 64, iova); set_bits(141, 4, id & 0xF);
}

template <typename T>
static void clear_prog_req(T& req) { for (int i = 0; i < 9; i++) req[i] = 0; }

template <typename T>
static void set_prog_req_write(T& req, uint64_t addr, uint64_t data, uint8_t strb = 0xFF) {
    clear_prog_req(req);
    auto set_bits = [&](uint32_t start_bit, uint32_t num_bits, uint64_t value) {
        for (uint32_t i = 0; i < num_bits; i++) {
            uint32_t bit_idx = start_bit + i;
            uint32_t word_idx = bit_idx / 32;
            uint32_t offset = bit_idx % 32;
            if ((value >> i) & 1ULL) req[word_idx] |= (1U << offset);
            else req[word_idx] &= ~(1U << offset);
        }
    };
    set_bits(0, 1, 1); set_bits(102, 1, 1); set_bits(103, 1, 1); set_bits(105, 1, 1);
    set_bits(106, 8, strb); set_bits(114, 64, data); set_bits(178, 1, 1);
    set_bits(204, 3, 2); set_bits(215, 64, addr);
}

// Memory Model: CLEAN (No Random Stalls) - isolates IOTLB thrashing effect
class CleanMemoryModel {
private:
    std::map<uint64_t, uint64_t> mem;
    int base_latency_cycles;
    struct PendingRead { uint64_t base_addr; uint32_t len, current_beat, id; uint64_t ready_cycle; };
    std::queue<PendingRead> read_queue;
    PendingRead active_read;
    bool has_active_read;
    uint64_t next_ar_ready_cycle;
public:
    CleanMemoryModel(int lat = 3) : base_latency_cycles(lat), has_active_read(false), next_ar_ready_cycle(0) {}
    void write64(uint64_t addr, uint64_t val) { mem[addr] = val; }
    uint64_t read64(uint64_t addr) const {
        auto it = mem.find(addr); return (it != mem.end()) ? it->second : 0ULL;
    }
    void eval(Vlint_checks* top, uint64_t current_cycle) {
        for (int i = 0; i < 3; i++) top->ds_resp_i[i] = 0;
        bool ar_valid = (top->ds_req_o[0] >> 1) & 1;
        uint32_t ar_len  = (top->ds_req_o[0] >> 24) & 0xFF;
        uint64_t ar_addr = (((uint64_t)top->ds_req_o[2] & 0xFFFFFFFFULL) << 32) | ((uint64_t)top->ds_req_o[1]);
        uint32_t ar_id   = (top->ds_req_o[3] >> 0) & 0xF;
        bool ar_ready = (current_cycle >= next_ar_ready_cycle);
        if (ar_ready) top->ds_resp_i[2] |= (1U << 18);
        if (ar_valid && ar_ready) {
            PendingRead req;
            req.base_addr = ar_addr; req.len = ar_len; req.current_beat = 0; req.id = ar_id;
            req.ready_cycle = current_cycle + base_latency_cycles; // Fixed latency, no random stall
            std::cout << "[MemModel Cycle " << current_cycle << "] Read QUEUED: addr=0x"
                      << std::hex << ar_addr << std::dec
                      << " (fixed " << base_latency_cycles << " cycle latency)" << std::endl;
            read_queue.push(req);
            next_ar_ready_cycle = current_cycle + 1;
        }
        if (!has_active_read && !read_queue.empty()) {
            if (current_cycle >= read_queue.front().ready_cycle) {
                active_read = read_queue.front(); has_active_read = true; read_queue.pop();
            }
        }
        if (has_active_read) {
            uint64_t beat_addr = active_read.base_addr + (active_read.current_beat * 8);
            uint64_t val = read64(beat_addr);
            bool is_last = (active_read.current_beat == active_read.len);
            
            top->ds_resp_i[2] |= (1U << 8); // r_valid
            if (is_last) top->ds_resp_i[0] |= (1U << 1); // r_last
            top->ds_resp_i[0] |= (uint32_t)((val & 0x0FFFFFFFULL) << 4);
            top->ds_resp_i[1] |= (uint32_t)((val >> 28) & 0xFFFFFFFFULL);
            top->ds_resp_i[2] |= (uint32_t)((val >> 60) & 0xF);
            top->ds_resp_i[2] |= ((active_read.id & 0xF) << 4);
            
            if (is_last) has_active_read = false; else active_read.current_beat++;
        }
    }
};

static void populate_page_tables(CleanMemoryModel& mem) {
    std::cout << "[+] EXP1: Populating 17-page working set for IOTLB Capacity Thrashing..." << std::endl;
    uint64_t tc_val = 1ULL;
    uint64_t fsc_val = (8ULL << 60) | 0x20ULL;
    int devices[] = {10, 20, 30, 40, 50};
    for (int dev : devices) {
        uint64_t dc_addr = 0x10000 + dev * 64;
        mem.write64(dc_addr + 0, tc_val); mem.write64(dc_addr + 8, 0ULL);
        mem.write64(dc_addr + 16, 0ULL); mem.write64(dc_addr + 24, fsc_val);
    }
    mem.write64(0x20000, (0x21ULL << 10) | 0x01ULL);
    mem.write64(0x21000, (0x22ULL << 10) | 0x01ULL);
    for (int i = 0; i < 17; i++) {
        uint64_t iova = 0x4000ULL + (i * 0x1000ULL);
        uint64_t vpn0 = (iova >> 12) & 0x1FF;
        uint64_t spa  = 0x80004000ULL + (i * 0x1000ULL);
        mem.write64(0x22000 + vpn0 * 8, ((spa >> 12) << 10) | 0xC7ULL);
        std::cout << "[+] PTE[" << i << "]: IOVA=0x" << std::hex << iova << " -> SPA=0x" << spa << std::dec << std::endl;
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Verilated::traceEverOn(true);
    Vlint_checks* top = new Vlint_checks;
    VerilatedVcdC* tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("sim_trace.vcd");
    top->clk_i = 0; top->rst_ni = 0;
    top->dev_comp_resp_i[0] = 0; top->dev_comp_resp_i[1] = 0;
    top->dev_comp_resp_i[2] = (1U << 18) | (1U << 19) | (1U << 17);
    clear_dev_tr_req(top->dev_tr_req_i);
    clear_prog_req(top->prog_req_i);
    CleanMemoryModel memory(3);
    populate_page_tables(memory);

    std::cout << "\n============================================================" << std::endl;
    std::cout << "TempoIOMMU-RV: Experiment 1 - IOTLB Capacity Thrashing" << std::endl;
    std::cout << "  Device: 10 | Working Set: 17 pages | IOTLB_ENTRIES: 16" << std::endl;
    std::cout << "  Total Requests: 1000" << std::endl;
    std::cout << "  Expected: 0% hit rate, all requests at ~15 cycle PTW latency" << std::endl;
    std::cout << "============================================================" << std::endl;

    uint64_t cycle = 0;
    bool req_active = false, req_clear_pending = false;
    uint64_t req_start_cycle = 0;
    int exp_stage = 0;

    for (uint64_t time = 0; time < 20000000; time++) {
        top->clk_i = (time % 10 < 5) ? 0 : 1;
        top->eval();
        if (time == 100) { top->rst_ni = 1; std::cout << "[Cycle " << cycle << "] Reset de-asserted." << std::endl; }
        if (time % 10 == 5 && top->rst_ni) {
            cycle = time / 10;
            memory.eval(top, cycle);
            top->eval();
            if (cycle == 20) { set_prog_req_write(top->prog_req_i, 0x10, 0x4002ULL); }
            else if (cycle == 23) { set_prog_req_write(top->prog_req_i, 0x14, 0x0ULL); }
            else if (cycle == 26) { clear_prog_req(top->prog_req_i); }
            if (cycle >= 20 && cycle <= 30) {
                bool aw_rdy = (top->prog_resp_o[2] >> 19) & 1;
                bool w_rdy  = (top->prog_resp_o[2] >> 17) & 1;
                bool b_vld  = (top->prog_resp_o[2] >> 16) & 1;
                std::cout << "[MMIO Diag Cycle " << cycle << "] aw_ready=" << aw_rdy
                          << " w_ready=" << w_rdy << " b_valid=" << b_vld << std::endl;
            }

            // === EXP1: Single device, 17-page working set, 5000 requests ===
            if (!req_active && !req_clear_pending && cycle > 50 && exp_stage < 5000) {
                int dev = 10;
                uint64_t iova = 0x4000ULL + ((exp_stage % 17) * 0x1000ULL);
                int pass = (exp_stage / 17) + 1;
                uint8_t req_id = exp_stage % 8;
                std::cout << "[Cycle " << cycle << "] [REQ " << exp_stage
                          << " | Pass " << pass << "] Dev=" << dev
                          << ", IOVA=0x" << std::hex << iova << std::dec << std::endl;
                set_dev_tr_req_ar(top->dev_tr_req_i, dev, 0, false, iova, req_id, 0);
                req_active = true; req_start_cycle = cycle; exp_stage++;
            }
            if (req_clear_pending) {
                clear_dev_tr_req(top->dev_tr_req_i);
                req_clear_pending = false; req_active = false;
            }
            
            // Always accept read responses to prevent AXI demux FIFO from filling up
            top->dev_tr_req_i[0] |= 1;
            bool ar_ready = (top->dev_tr_resp_o[2] >> 18) & 1;
            if (req_active && ar_ready) { req_clear_pending = true; }

            bool comp_ar_valid = (top->dev_comp_req_o[0] >> 1) & 1;
            static bool mem_resp_pending = false;
            static uint64_t latched_phys_addr = 0;

            top->dev_comp_resp_i[0] = 0;
            top->dev_comp_resp_i[1] = 0;
            top->dev_comp_resp_i[2] = 0;
            
            top->dev_comp_resp_i[2] |= (1U << 18); // ar_ready (bit 82)
            top->dev_comp_resp_i[2] |= (1U << 19); // aw_ready (bit 83)
            top->dev_comp_resp_i[2] |= (1U << 17); // w_ready (bit 81)

            if (comp_ar_valid && !mem_resp_pending) { mem_resp_pending = true; latched_phys_addr = 0xbeef0000; }
            if (mem_resp_pending) {
                top->dev_comp_resp_i[2] |= (1U << 8); // r_valid (bit 72)
                top->dev_comp_resp_i[0] |= (1U << 5); // r_last (bit 5)
                uint8_t req_id = (exp_stage - 1) % 8;
                top->dev_comp_resp_i[0] |= ((uint32_t)req_id << 1); // r_id (bits 4:1)
                top->dev_comp_resp_i[0] |= (uint32_t)((latched_phys_addr & 0x0FFFFFFFULL) << 8); // data starts at bit 8
                top->dev_comp_resp_i[1] |= (uint32_t)((latched_phys_addr >> 24) & 0xFFFFFFFFULL);
                top->dev_comp_resp_i[2] |= (uint32_t)((latched_phys_addr >> 56) & 0xFFULL);
                if (top->dev_comp_req_o[0] & 1) mem_resp_pending = false;
            }
        }
        top->eval();
        tfp->dump(time);
        if (exp_stage >= 5000 && !req_active && !req_clear_pending) break;
    }
    tfp->close(); delete top;
    std::cout << "\n============================================================" << std::endl;
    std::cout << "Experiment 1 complete. Run: make parse && make stats && make plot" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}
