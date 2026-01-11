#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>

// Registers
enum Reg
{
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    RNONE = 0xF
};

// Condition Codes
struct CC_t
{
    bool zf = true;
    bool sf = false;
    bool of = false;
};

// Program Status
enum Stat_t
{
    STAT_AOK = 1,
    STAT_HLT = 2,
    STAT_ADR = 3,
    STAT_INS = 4
};

// ==========================================
// Branch Predictor Class (Modified)
// Strategy: BTFNT
// ==========================================
class BranchPredictor
{
public:
    BranchPredictor();

    // Updates predictors: needs PC, Target Address, and Actual Outcome
    void update(uint64_t pc, uint64_t target, bool actual_taken);

    struct Stats
    {
        uint64_t total_branches;
        uint64_t mispredictions;
    };

    Stats get_stats() const;

private:
    uint64_t total_branches;
    uint64_t mispredictions;
};

// ==========================================
// Thread-Safe Shared Memory Class (Level 0)
// ==========================================
class SharedMemory
{
public:
    SharedMemory();
    const uint64_t MEM_SIZE = 0x10000;

    uint8_t get_byte(uint64_t addr, bool &error);
    void set_byte(uint64_t addr, uint8_t val, bool &error);
    uint64_t get_word(uint64_t addr, bool &error);
    void set_word(uint64_t addr, int64_t val, bool &error);

    void print_json_content(std::ostream &os);

    // Time tracking
    std::atomic<uint64_t> memory_access_time{0}; // Total time spent on memory access (cycles)

private:
    std::map<uint64_t, uint8_t> mem;
    mutable std::recursive_mutex mtx;
};

// ==========================================
// L1 Cache Structures
// ==========================================
struct CacheLine
{
    bool valid = false;
    bool dirty = false; // 脏位
    uint64_t tag = 0;
    uint64_t lru_time = 0; // LRU timestamp
    uint8_t data[8] = {0};
};

// ==========================================
// Thread-Safe L1 Cache Class (Level 1)
// 4-Way Set Associative with LRU Replacement
// ==========================================
class L1Cache
{
public:
    L1Cache(SharedMemory &mem_ref);
    SharedMemory &memory;

    // CPU accessors
    uint8_t get_byte(uint64_t addr, bool &error);
    void set_byte(uint64_t addr, uint8_t val, bool &error);
    uint64_t get_word(uint64_t addr, bool &error);
    void set_word(uint64_t addr, int64_t val, bool &error);

    // Write-back all dirty lines to memory
    void write_back_all();

    // Reporting
    void print_report();
    void print_memory_json(std::ostream &os);

    // Cache timing constants
    static const uint64_t CACHE_ACCESS_TIME = 1;   // 1 cycle for cache access
    static const uint64_t MEMORY_ACCESS_TIME = 70; // 70 cycles for memory access

    // Cache performance metrics
    std::atomic<uint64_t> total_cache_time{0};
    std::atomic<uint64_t> total_memory_time{0};
    std::atomic<uint64_t> memory_reads{0};
    std::atomic<uint64_t> memory_writes{0};

private:
    std::mutex mtx;

    // Configuration: 4-Way Set Associative, 8 Sets
    static const int NUM_SETS = 8;
    static const int WAYS = 4;
    static const int OFFSET_BITS = 3;
    static const int SET_BITS = 3;
    static const int NUM_LINES = NUM_SETS * WAYS;

    CacheLine lines[NUM_LINES];
    uint64_t lru_clock = 0;

    std::atomic<uint64_t> hits;
    std::atomic<uint64_t> misses;

    int load_line(uint64_t addr, uint64_t set_index, uint64_t tag);
    void write_back_line(int index);
    uint64_t get_block_addr(uint64_t tag, uint64_t set_index);
};

// ==========================================
// CPU Class (Core)
// ==========================================
class CPU
{
public:
    CPU(L1Cache &cache_ref);

    void load_io();
    void run();
    void print_ilp_report();

private:
    uint64_t pc;
    int64_t reg[15];
    CC_t cc;
    Stat_t stat;

    L1Cache &cache;

    BranchPredictor branch_predictor;

    uint64_t reg_last_write_id[15];
    std::map<uint64_t, uint64_t> mem_last_write_id;
    uint64_t instr_id_counter;
    uint64_t total_instr;
    uint64_t total_logic_cycles;

    void parse_line(const std::string &line);
    void step();
    void print_state(bool first);
    void thread_entry();

    uint64_t get_mem_dependency_id(uint64_t addr);
    void update_mem_dependency_id(uint64_t addr, uint64_t instr_id);
};

#endif