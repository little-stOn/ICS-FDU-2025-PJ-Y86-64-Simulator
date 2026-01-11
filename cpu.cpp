#include "cpu.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <set>
#include <algorithm>
#include <cmath>
#include <numeric>

using namespace std;

const string REG_NAMES[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14"};

std::mutex io_mutex;

// ==========================================
// BranchPredictor Implementation
// Strategy: BTFNT (Backward Taken, Forward Not Taken)
// ==========================================

BranchPredictor::BranchPredictor()
    : total_branches(0), mispredictions(0)
{
}

void BranchPredictor::update(uint64_t pc, uint64_t target, bool actual_taken)
{
    total_branches++;

    // --- Strategy: BTFNT ---
    // Rule 1: If Target <= PC, it's a Backward Branch (e.g. Loop) -> Predict TAKEN
    // Rule 2: If Target > PC, it's a Forward Branch (e.g. if/else) -> Predict NOT TAKEN

    // Check direction
    bool is_backward = (target <= pc);

    // Predict
    bool predicted_taken = is_backward;

    // Verify
    if (predicted_taken != actual_taken)
    {
        mispredictions++;
    }
}

BranchPredictor::Stats BranchPredictor::get_stats() const
{
    return {total_branches, mispredictions};
}

// ==========================================
// SharedMemory Implementation
// ==========================================

SharedMemory::SharedMemory() {}

uint8_t SharedMemory::get_byte(uint64_t addr, bool &error)
{
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (addr >= MEM_SIZE)
    {
        error = true;
        return 0;
    }

    memory_access_time += L1Cache::MEMORY_ACCESS_TIME;

    if (mem.find(addr) != mem.end())
    {
        return mem[addr];
    }
    return 0;
}

void SharedMemory::set_byte(uint64_t addr, uint8_t val, bool &error)
{
    std::lock_guard<std::recursive_mutex> lock(mtx);
    if (addr >= MEM_SIZE)
    {
        error = true;
        return;
    }

    memory_access_time += L1Cache::MEMORY_ACCESS_TIME;
    mem[addr] = val;
}

uint64_t SharedMemory::get_word(uint64_t addr, bool &error)
{
    std::lock_guard<std::recursive_mutex> lock(mtx);
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
    {
        uint64_t b = get_byte(addr + i, error);
        val |= (b << (i * 8));
    }
    return val;
}

void SharedMemory::set_word(uint64_t addr, int64_t val, bool &error)
{
    std::lock_guard<std::recursive_mutex> lock(mtx);
    uint64_t uval = (uint64_t)val;
    for (int i = 0; i < 8; ++i)
    {
        set_byte(addr + i, (uval >> (i * 8)) & 0xFF, error);
    }
}

void SharedMemory::print_json_content(std::ostream &os)
{
    std::lock_guard<std::recursive_mutex> lock(mtx);

    std::set<uint64_t> aligned_addrs;
    for (auto const &[addr, val] : mem)
    {
        aligned_addrs.insert(addr & ~0x7ULL);
    }

    bool first_mem = true;
    for (uint64_t addr : aligned_addrs)
    {
        uint64_t val_u = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (mem.count(addr + i))
                val_u |= ((uint64_t)mem.at(addr + i) << (i * 8));
        }
        int64_t val = (int64_t)val_u;

        if (val != 0)
        {
            if (!first_mem)
                os << ",";
            os << endl
               << "      \"" << addr << "\": " << val;
            first_mem = false;
        }
    }
}

// ==========================================
// L1Cache Implementation
// ==========================================

L1Cache::L1Cache(SharedMemory &mem_ref) : memory(mem_ref)
{
    hits = 0;
    misses = 0;
    lru_clock = 0;
    for (int i = 0; i < NUM_LINES; ++i)
    {
        lines[i].valid = false;
        lines[i].dirty = false;
        lines[i].tag = 0;
        lines[i].lru_time = 0;
    }
}

uint64_t L1Cache::get_block_addr(uint64_t tag, uint64_t set_index)
{
    return (tag << (SET_BITS + OFFSET_BITS)) | (set_index << OFFSET_BITS);
}

int L1Cache::load_line(uint64_t addr, uint64_t set_index, uint64_t tag)
{
    bool error = false;
    uint64_t block_addr = addr & ~0x7ULL;

    int victim_way = -1;
    int empty_way = -1;
    uint64_t min_lru = UINT64_MAX;

    int base_index = set_index * WAYS;

    for (int i = 0; i < WAYS; ++i)
    {
        int idx = base_index + i;
        if (!lines[idx].valid)
        {
            empty_way = i;
            break;
        }
        if (lines[idx].lru_time < min_lru)
        {
            min_lru = lines[idx].lru_time;
            victim_way = i;
        }
    }

    int target_way = (empty_way != -1) ? empty_way : victim_way;
    int target_index = base_index + target_way;

    if (lines[target_index].valid && lines[target_index].dirty)
    {
        write_back_line(target_index);
    }

    total_memory_time += MEMORY_ACCESS_TIME;
    memory_reads++;

    for (int i = 0; i < 8; ++i)
    {
        lines[target_index].data[i] = memory.get_byte(block_addr + i, error);
    }
    lines[target_index].tag = tag;
    lines[target_index].valid = true;
    lines[target_index].dirty = false;
    lines[target_index].lru_time = ++lru_clock;

    if (error)
    {
        cerr << "Cache: Memory load error at 0x" << hex << block_addr << dec << endl;
    }

    return target_index;
}

void L1Cache::write_back_line(int index)
{
    if (!lines[index].valid || !lines[index].dirty)
        return;

    bool error = false;
    uint64_t set_index = index / WAYS;
    uint64_t block_addr = get_block_addr(lines[index].tag, set_index);

    total_memory_time += MEMORY_ACCESS_TIME;
    memory_writes++;

    for (int i = 0; i < 8; ++i)
    {
        memory.set_byte(block_addr + i, lines[index].data[i], error);
    }

    if (error)
    {
        cerr << "Cache: Memory write-back error at 0x" << hex << block_addr << dec << endl;
    }

    lines[index].dirty = false;
}

void L1Cache::write_back_all()
{
    std::lock_guard<std::mutex> lock(mtx);

    for (int i = 0; i < NUM_LINES; ++i)
    {
        if (lines[i].valid && lines[i].dirty)
        {
            write_back_line(i);
        }
    }
}

uint8_t L1Cache::get_byte(uint64_t addr, bool &error)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (addr >= memory.MEM_SIZE)
    {
        error = true;
        return 0;
    }

    uint64_t offset = addr & 0x7;
    uint64_t set_index = (addr >> OFFSET_BITS) & (NUM_SETS - 1);
    uint64_t tag = addr >> (SET_BITS + OFFSET_BITS);

    total_cache_time += CACHE_ACCESS_TIME;

    int base_index = set_index * WAYS;
    for (int i = 0; i < WAYS; ++i)
    {
        int idx = base_index + i;
        if (lines[idx].valid && lines[idx].tag == tag)
        {
            hits++;
            lines[idx].lru_time = ++lru_clock;
            return lines[idx].data[offset];
        }
    }

    misses++;
    int idx = load_line(addr, set_index, tag);
    return lines[idx].data[offset];
}

void L1Cache::set_byte(uint64_t addr, uint8_t val, bool &error)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (addr >= memory.MEM_SIZE)
    {
        error = true;
        return;
    }

    uint64_t offset = addr & 0x7;
    uint64_t set_index = (addr >> OFFSET_BITS) & (NUM_SETS - 1);
    uint64_t tag = addr >> (SET_BITS + OFFSET_BITS);

    total_cache_time += CACHE_ACCESS_TIME;

    int base_index = set_index * WAYS;
    for (int i = 0; i < WAYS; ++i)
    {
        int idx = base_index + i;
        if (lines[idx].valid && lines[idx].tag == tag)
        {
            hits++;
            lines[idx].data[offset] = val;
            lines[idx].dirty = true;
            lines[idx].lru_time = ++lru_clock;
            return;
        }
    }

    misses++;
    int idx = load_line(addr, set_index, tag);
    lines[idx].data[offset] = val;
    lines[idx].dirty = true;
}

uint64_t L1Cache::get_word(uint64_t addr, bool &error)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
    {
        uint64_t b = get_byte(addr + i, error);
        val |= (b << (i * 8));
    }
    return val;
}

void L1Cache::set_word(uint64_t addr, int64_t val, bool &error)
{
    uint64_t uval = (uint64_t)val;
    for (int i = 0; i < 8; ++i)
    {
        set_byte(addr + i, (uval >> (i * 8)) & 0xFF, error);
    }
}

void L1Cache::print_report()
{
    uint64_t total_accesses = hits + misses;
    double hit_rate = 0.0;
    if (total_accesses > 0)
    {
        hit_rate = (double)hits / total_accesses;
    }

    int dirty_count = 0;
    for (int i = 0; i < NUM_LINES; ++i)
    {
        if (lines[i].valid && lines[i].dirty)
            dirty_count++;
    }

    uint64_t total_time = total_cache_time + total_memory_time;
    double memory_time_percentage = 0.0;
    if (total_time > 0)
    {
        memory_time_percentage = (double)total_memory_time / total_time * 100.0;
    }

    cerr << "--- L1 Cache Report (4-Way Set Associative, LRU) ---" << endl;
    cerr << "Total Accesses: " << total_accesses << endl;
    cerr << "Hits: " << hits << endl;
    cerr << "Misses: " << misses << endl;
    cerr << "Hit Rate: " << fixed << setprecision(4) << hit_rate * 100.0 << "%" << endl;
    cerr << "Dirty Lines: " << dirty_count << endl;
    cerr << endl;
    cerr << "Total Cache Access Time: " << total_cache_time << " cycles" << endl;
    cerr << "Total Memory Access Time: " << total_memory_time << " cycles" << endl;
    cerr << "Total Access Time (Cache + Memory): " << total_time << " cycles" << endl;
    cerr << "Memory Access Percentage: " << fixed << setprecision(2) << memory_time_percentage << "%" << endl;
    cerr << "------------------------------------------------------" << endl;
}

void L1Cache::print_memory_json(std::ostream &os)
{
    write_back_all();
    memory.print_json_content(os);
}

// ==========================================
// CPU Implementation
// ==========================================

CPU::CPU(L1Cache &cache_ref) : cache(cache_ref)
{
    pc = 0;
    for (int i = 0; i < 15; ++i)
        reg[i] = 0;
    cc.zf = true;
    cc.sf = false;
    cc.of = false;
    stat = STAT_AOK;

    std::fill(reg_last_write_id, reg_last_write_id + 15, 0ULL);
    mem_last_write_id.clear();
    instr_id_counter = 0;
    total_instr = 0;
    total_logic_cycles = 0;
}

void CPU::load_io()
{
    string line;
    while (getline(cin, line))
    {
        parse_line(line);
    }
}

void CPU::parse_line(const string &line)
{
    size_t pipe_pos = line.find('|');
    string content = (pipe_pos == string::npos) ? line : line.substr(0, pipe_pos);

    size_t colon_pos = content.find(':');
    if (colon_pos == string::npos)
        return;

    string addr_str = content.substr(0, colon_pos);
    string bytes_str = content.substr(colon_pos + 1);

    auto trim = [](string &s)
    {
        if (s.empty())
            return;
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
    };
    trim(addr_str);
    trim(bytes_str);

    if (addr_str.empty() || bytes_str.empty())
        return;

    try
    {
        uint64_t addr = stoull(addr_str, nullptr, 16);

        for (size_t i = 0; i < bytes_str.length(); i += 2)
        {
            if (i + 1 >= bytes_str.length())
                break;
            string byte_hex = bytes_str.substr(i, 2);
            if (byte_hex == "  " || byte_hex.find(' ') != string::npos)
                break;
            uint8_t b = (uint8_t)stoi(byte_hex, nullptr, 16);

            bool error = false;
            // Initialize memory directly bypassing cache metrics for setup
            cache.memory.set_byte(addr, b, error);

            addr++;
        }
    }
    catch (const std::exception &e)
    {
        return;
    }
}

void CPU::thread_entry()
{
    {
        std::lock_guard<std::mutex> lock(io_mutex);
        cout << "[" << endl;
    }

    bool first = true;

    while (stat == STAT_AOK)
    {
        step();
        print_state(first);
        first = false;
    }

    {
        std::lock_guard<std::mutex> lock(io_mutex);
        cout << endl
             << "]" << endl;
    }
}

void CPU::run()
{
    std::thread t(&CPU::thread_entry, this);
    t.join();

    cache.write_back_all();

    cache.print_report();
    print_ilp_report();
}

uint64_t CPU::get_mem_dependency_id(uint64_t addr)
{
    uint64_t aligned_addr = addr & ~0x7ULL;
    uint64_t max_id = 0;

    for (int i = 0; i < 8; ++i)
    {
        uint64_t current_addr = aligned_addr + i;
        if (mem_last_write_id.count(current_addr))
        {
            max_id = std::max(max_id, mem_last_write_id.at(current_addr));
        }
    }
    return max_id;
}

void CPU::update_mem_dependency_id(uint64_t addr, uint64_t instr_id)
{
    // uint64_t aligned_addr = addr & ~0x7ULL;

    for (int i = 0; i < 8; ++i)
    {
        mem_last_write_id[addr + i] = instr_id;
    }
}

void CPU::step()
{
    instr_id_counter++;
    uint64_t current_instr_id = instr_id_counter;

    total_instr++;

    const uint64_t MAX_INSTRUCTIONS = 20000;
    if (total_instr > MAX_INSTRUCTIONS)
    {
        cerr << "Warning: Instruction limit reached (" << MAX_INSTRUCTIONS << "). Stopping simulation." << endl;
        stat = STAT_HLT;
        return;
    }

    bool imem_error = false;
    bool dmem_error = false;
    bool instr_error = false;

    uint64_t f_pc = pc;
    uint8_t icode_ifun = cache.get_byte(f_pc, imem_error);
    uint8_t icode = (icode_ifun >> 4) & 0xF;
    uint8_t ifun = icode_ifun & 0xF;

    uint64_t valP = f_pc + 1;

    uint8_t rA = RNONE, rB = RNONE;
    int64_t valC = 0;

    bool need_reg = (icode == 2 || icode == 3 || icode == 4 || icode == 5 ||
                     icode == 6 || icode == 0xA || icode == 0xB);
    bool need_valC = (icode == 3 || icode == 4 || icode == 5 || icode == 7 || icode == 8);

    if (need_reg)
    {
        uint8_t reg_byte = cache.get_byte(valP, imem_error);
        rA = (reg_byte >> 4) & 0xF;
        rB = reg_byte & 0xF;
        valP++;
    }

    if (need_valC)
    {
        valC = (int64_t)cache.get_word(valP, imem_error);
        valP += 8;
    }

    if (imem_error)
    {
        stat = STAT_ADR;
        return;
    }

    if (icode > 0xB)
    {
        stat = STAT_INS;
        return;
    }

    uint64_t next_pc = valP;

    // --- Dependency Analysis： Calculate ILP ---
    uint64_t max_dependency_id = 0;

    auto check_reg_read = [&](uint8_t reg_id)
    {
        if (reg_id != RNONE && reg_id < 15)
        {
            max_dependency_id = std::max(max_dependency_id, reg_last_write_id[reg_id]);
        }
    };

    auto update_reg_write = [&](uint8_t reg_id)
    {
        if (reg_id != RNONE && reg_id < 15)
        {
            reg_last_write_id[reg_id] = current_instr_id;
        }
    };

    auto check_mem_read = [&](int64_t addr)
    {
        max_dependency_id = std::max(max_dependency_id, get_mem_dependency_id(addr));
    };

    auto update_mem_write = [&](int64_t addr)
    {
        update_mem_dependency_id(addr, current_instr_id);
    };

    switch (icode)
    {
    case 0x0:
    case 0x1:
        break;

    case 0x2:
        check_reg_read(rA);
        update_reg_write(rB);
        break;

    case 0x3:
        update_reg_write(rB);
        break;

    case 0x4: // rmmovq (Store)
        check_reg_read(rA);
        check_reg_read(rB);
        update_mem_write(reg[rB] + valC);
        break;

    case 0x5: // mrmovq (Load)
        check_reg_read(rB);
        check_mem_read(reg[rB] + valC);
        update_reg_write(rA);
        break;

    case 0x6: // OPq
        check_reg_read(rA);
        check_reg_read(rB);
        update_reg_write(rB);
        break;

    case 0x7: // jXX
        break;

    case 0x8: // call
        check_reg_read(RSP);
        update_mem_write(reg[RSP] - 8);
        update_reg_write(RSP);
        break;

    case 0x9: // ret
        check_reg_read(RSP);
        check_mem_read(reg[RSP]);
        update_reg_write(RSP);
        break;

    case 0xA: // pushq
        check_reg_read(rA);
        check_reg_read(RSP);
        update_mem_write(reg[RSP] - 8);
        update_reg_write(RSP);
        break;

    case 0xB: // popq
        check_reg_read(RSP);
        check_mem_read(reg[RSP]);
        update_reg_write(RSP);
        update_reg_write(rA);
        break;

    default:
        break;
    }

    uint64_t completion_cycle = max_dependency_id + 1;
    total_logic_cycles = std::max(total_logic_cycles, completion_cycle);
    // --- Dependency Analysis End ---

    switch (icode)
    {
    case 0x0:
        stat = STAT_HLT;
        break;
    case 0x1:
        break;

    case 0x2: // rrmovq / cmovXX
    {
        bool cond = false;
        switch (ifun)
        {
        case 0:
            cond = true;
            break;
        case 1:
            cond = (cc.sf ^ cc.of) || cc.zf;
            break;
        case 2:
            cond = (cc.sf ^ cc.of);
            break;
        case 3:
            cond = cc.zf;
            break;
        case 4:
            cond = !cc.zf;
            break;
        case 5:
            cond = !(cc.sf ^ cc.of);
            break;
        case 6:
            cond = !(cc.sf ^ cc.of) && !cc.zf;
            break;
        default:
            instr_error = true;
            break;
        }
        if (cond && !instr_error)
        {
            reg[rB] = reg[rA];
        }
        break;
    }

    case 0x3:
        reg[rB] = valC;
        break;

    case 0x4: // rmmovq
    {
        int64_t addr = reg[rB] + valC;
        cache.set_word(addr, reg[rA], dmem_error);
        break;
    }

    case 0x5: // mrmovq
    {
        int64_t addr = reg[rB] + valC;
        reg[rA] = (int64_t)cache.get_word(addr, dmem_error);
        break;
    }

    case 0x6: // OPq
    {
        int64_t a = reg[rA], b = reg[rB], res = 0;
        switch (ifun)
        {
        case 0:
            res = b + a;
            cc.of = (b < 0 && a < 0 && res >= 0) || (b >= 0 && a >= 0 && res < 0);
            break;
        case 1:
            res = b - a;
            cc.of = (b < 0 && a >= 0 && res >= 0) || (b >= 0 && a < 0 && res < 0);
            break;
        case 2:
            res = b & a;
            cc.of = false;
            break;
        case 3:
            res = b ^ a;
            cc.of = false;
            break;
        default:
            instr_error = true;
            break;
        }
        if (!instr_error)
        {
            reg[rB] = res;
            cc.zf = (res == 0);
            cc.sf = (res < 0);
        }
        break;
    }

    case 0x7: // jXX
    {
        bool cond = false;
        switch (ifun)
        {
        case 0:
            cond = true;
            break;
        case 1:
            cond = (cc.sf ^ cc.of) || cc.zf;
            break;
        case 2:
            cond = (cc.sf ^ cc.of);
            break;
        case 3:
            cond = cc.zf;
            break;
        case 4:
            cond = !cc.zf;
            break;
        case 5:
            cond = !(cc.sf ^ cc.of);
            break;
        case 6:
            cond = !(cc.sf ^ cc.of) && !cc.zf;
            break;
        default:
            instr_error = true;
            break;
        }

        // --- Branch Prediction Hook Start (BTFNT) ---
        if (!instr_error)
        {
            // Pass the target address (valC) to determine direction
            branch_predictor.update(f_pc, valC, cond);
        }

        if (!instr_error && cond)
        {
            next_pc = valC;
        }
        break;
    }

    case 0x8: // call
    {
        int64_t val = (int64_t)valP;
        reg[RSP] -= 8;
        cache.set_word(reg[RSP], val, dmem_error);
        next_pc = valC;
        break;
    }

    case 0x9: // ret
    {
        int64_t val = (int64_t)cache.get_word(reg[RSP], dmem_error);
        reg[RSP] += 8;
        next_pc = val;
        break;
    }

    case 0xA: // pushq
    {
        int64_t val = reg[rA];
        reg[RSP] -= 8;
        cache.set_word(reg[RSP], val, dmem_error);
        break;
    }

    case 0xB: // popq
    {
        int64_t val = (int64_t)cache.get_word(reg[RSP], dmem_error);
        reg[RSP] += 8;
        reg[rA] = val;
        break;
    }

    default:
        stat = STAT_INS;
        break;
    }

    if (instr_error)
        stat = STAT_INS;
    else if (dmem_error)
        stat = STAT_ADR;

    if (stat == STAT_AOK)
    {
        pc = next_pc;
    }
}

void CPU::print_ilp_report()
{
    // Output ILP report to standard error to keep JSON output pure

    BranchPredictor::Stats bp_stats = branch_predictor.get_stats();

    // Assume misprediction penalty is 2 cycles (pipeline flush)
    const uint64_t MISPRED_PENALTY = 2;

    auto calc_ilp = [&](uint64_t extra_penalty) -> double
    {
        uint64_t final_cycles = total_logic_cycles + extra_penalty;
        return (final_cycles > 0) ? (double)total_instr / final_cycles : 0.0;
    };

    double mpki = 0.0;
    if (total_instr > 0)
    {
        mpki = (double)bp_stats.mispredictions / (total_instr / 1000.0);
    }

    double acc = bp_stats.total_branches > 0 ? (1.0 - (double)bp_stats.mispredictions / bp_stats.total_branches) * 100.0 : 0.0;

    cerr << "--- ILP & Branch Prediction Analysis ---" << endl;
    cerr << "Total Instructions (I): " << total_instr << endl;
    cerr << "Base Logic Cycles  (C): " << total_logic_cycles << " (Data Dependency Only)" << endl;
    cerr << "Total Branches        : " << bp_stats.total_branches << endl;
    cerr << "------------------------------------------" << endl;

    // Baseline: Perfect Prediction
    cerr << "Baseline: Perfect Prediction" << endl;
    cerr << "  Mispredictions: 0" << endl;
    cerr << "  Penalty Cycles: 0" << endl;
    cerr << "  Real ILP      : " << fixed << setprecision(4) << calc_ilp(0) << endl;
    cerr << endl;

    // Strategy: BTFNT
    cerr << "Strategy: BTFNT (Back-Taken, Fwd-NotTaken)" << endl;
    cerr << "  Mispredictions: " << bp_stats.mispredictions << endl;
    cerr << "  Accuracy      : " << fixed << setprecision(2) << acc << "%" << endl;
    cerr << "  MPKI          : " << fixed << setprecision(2) << mpki << endl;
    cerr << "  Penalty Cycles: " << bp_stats.mispredictions * MISPRED_PENALTY << endl;
    cerr << "  Real ILP      : " << fixed << setprecision(4) << calc_ilp(bp_stats.mispredictions * MISPRED_PENALTY) << endl;
    cerr << "------------------------------------------" << endl;
}

void CPU::print_state(bool first)
{
    std::lock_guard<std::mutex> lock(io_mutex);

    if (!first)
        cout << "," << endl;

    cout << "  {" << endl;

    cout << "    \"CC\": {" << endl;
    cout << "      \"OF\": " << (cc.of ? 1 : 0) << "," << endl;
    cout << "      \"SF\": " << (cc.sf ? 1 : 0) << "," << endl;
    cout << "      \"ZF\": " << (cc.zf ? 1 : 0) << endl;
    cout << "    }," << endl;

    cout << "    \"MEM\": {";

    cache.print_memory_json(cout);

    cout << "}," << endl;

    cout << "    \"PC\": " << pc << "," << endl;

    cout << "    \"REG\": {" << endl;
    for (int i = 0; i < 15; ++i)
    {
        cout << "      \"" << REG_NAMES[i] << "\": " << reg[i];
        if (i < 14)
            cout << ",";
        cout << endl;
    }
    cout << "    }," << endl;

    cout << "    \"STAT\": " << stat << endl;

    cout << "  }";
}

int main()
{
    std::ios::sync_with_stdio(false);
    cin.tie(nullptr);

    SharedMemory system_mem;
    L1Cache l1_cache(system_mem);
    CPU cpu(l1_cache);

    cpu.load_io();
    cpu.run();

    return 0;
}