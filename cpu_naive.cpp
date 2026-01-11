#include "cpu_naive.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <set>
#include <algorithm>
#include <cmath>

using namespace std;

const string REG_NAMES[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14"};

CPU::CPU()
{
    pc = 0;
    for (int i = 0; i < 15; ++i)
        reg[i] = 0;
    cc.zf = true;
    cc.sf = false;
    cc.of = false;
    stat = STAT_AOK;
    total_memory_time = 0;
    memory_access_count = 0;
    total_instructions = 0;
}

// read 1 byte from memory
uint8_t CPU::get_byte(uint64_t addr, bool &error)
{
    if (addr >= MEM_SIZE)
    {
        error = true;
        return 0;
    }

    // Memory access takes MEMORY_ACCESS_TIME cycles
    total_memory_time += MEMORY_ACCESS_TIME;
    memory_access_count++;

    if (mem.find(addr) != mem.end())
    {
        return mem[addr];
    }
    return 0;
}

// write 1 byte in memory
void CPU::set_byte(uint64_t addr, uint8_t val, bool &error)
{
    if (addr >= MEM_SIZE)
    {
        error = true;
        return;
    }

    // Memory access takes MEMORY_ACCESS_TIME cycles
    total_memory_time += MEMORY_ACCESS_TIME;
    memory_access_count++;

    mem[addr] = val;
}

// read 8 bytes from memory

// little-endian: low memory saves low bit
uint64_t CPU::get_word(uint64_t addr, bool &error)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
    {
        uint64_t b = get_byte(addr + i, error);
        val |= (b << (i * 8));
    }
    return val;
}

void CPU::set_word(uint64_t addr, int64_t val, bool &error)
{
    uint64_t uval = (uint64_t)val;
    for (int i = 0; i < 8; ++i)
    {
        set_byte(addr + i, (uval >> (i * 8)) & 0xFF, error);
    }
}

// io & parsing

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
    // split
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
            if (addr >= MEM_SIZE)
            {
                throw std::out_of_range("Memory address out of range");
            }
            mem[addr] = b;
            addr++;
        }
    }
    catch (const std::exception &e)
    {
        cout << "Error parsing line: " << line << endl;
        return;
    }
}

// execution
void CPU::run()
{
    cout << "[" << endl;
    bool first = true;

    while (stat == STAT_AOK)
    {
        step();
        print_state(first);
        first = false;
    }

    cout << endl
         << "]" << endl;
}

void CPU::step()
{
    total_instructions++;

    bool imem_error = false;  // fetch error
    bool dmem_error = false;  // memory error
    bool instr_error = false; // invalid instruction error

    //  Fetch
    uint64_t f_pc = pc;
    uint8_t icode_ifun = get_byte(f_pc, imem_error);
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
        uint8_t reg_byte = get_byte(valP, imem_error);
        rA = (reg_byte >> 4) & 0xF;
        rB = reg_byte & 0xF;
        valP++;
    }

    if (need_valC)
    {
        valC = (int64_t)get_word(valP, imem_error);
        valP += 8;
    }

    // Check instruction memory error immediately
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

    switch (icode)
    {
    case 0x0: // halt
        stat = STAT_HLT;
        break;

    case 0x1: // nop
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

    case 0x3: // irmovq
        reg[rB] = valC;
        break;

    case 0x4: // rmmovq
    {
        int64_t addr = reg[rB] + valC;
        set_word(addr, reg[rA], dmem_error);
        break;
    }

    case 0x5: // mrmovq
    {
        int64_t addr = reg[rB] + valC;
        reg[rA] = (int64_t)get_word(addr, dmem_error);
        break;
    }

    case 0x6:
    {
        int64_t a = reg[rA];
        int64_t b = reg[rB];
        int64_t res = 0;
        switch (ifun)
        {
        case 0: // addq
            res = b + a;
            cc.of = (b < 0 && a < 0 && res >= 0) || (b >= 0 && a >= 0 && res < 0);
            break;
        case 1: // subq
            res = b - a;
            cc.of = (b < 0 && a >= 0 && res >= 0) || (b >= 0 && a < 0 && res < 0);
            break;
        case 2: // andq
            res = b & a;
            cc.of = false;
            break;
        case 3: // xorq
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
        set_word(reg[RSP], val, dmem_error);
        next_pc = valC;
        break;
    }

    case 0x9: // ret
    {
        int64_t val = (int64_t)get_word(reg[RSP], dmem_error);
        reg[RSP] += 8;
        next_pc = val;
        break;
    }

    case 0xA: // pushq
    {
        int64_t val = reg[rA];
        reg[RSP] -= 8;
        set_word(reg[RSP], val, dmem_error);
        break;
    }

    case 0xB: // popq
    {
        int64_t val = (int64_t)get_word(reg[RSP], dmem_error);
        reg[RSP] += 8;
        reg[rA] = val;
        break;
    }

    default:
        stat = STAT_INS;
        break;
    }

    // Final status update
    if (instr_error)
        stat = STAT_INS;
    else if (dmem_error)
        stat = STAT_ADR;

    if (stat == STAT_AOK)
    {
        pc = next_pc;
    }
}

void CPU::print_performance_report()
{
    cerr << "=== Memory Performance Report (No Cache) ===" << endl;
    cerr << "Memory Access Time (per access): " << MEMORY_ACCESS_TIME << " cycle(s)" << endl;
    cerr << "Total Instructions Executed: " << total_instructions << endl;
    cerr << "Total Memory Access Count: " << memory_access_count << endl;
    cerr << "Total Memory Access Time: " << total_memory_time << " cycles" << endl;

    double avg_memory_time_per_instr = 0.0;
    if (total_instructions > 0)
    {
        avg_memory_time_per_instr = (double)total_memory_time / total_instructions;
    }

    cerr << "Average Memory Time per Instruction: " << fixed << setprecision(2) << avg_memory_time_per_instr << " cycles" << endl;
    cerr << "=======================================" << endl;
}

// output
void CPU::print_state(bool first)
{
    if (!first)
        cout << "," << endl;

    cout << "  {" << endl;

    cout << "    \"CC\": {" << endl;
    cout << "      \"OF\": " << (cc.of ? 1 : 0) << "," << endl;
    cout << "      \"SF\": " << (cc.sf ? 1 : 0) << "," << endl;
    cout << "      \"ZF\": " << (cc.zf ? 1 : 0) << endl;
    cout << "    }," << endl;

    cout << "    \"MEM\": {";
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
                cout << ",";
            cout << endl
                 << "      \"" << addr << "\": " << val;
            first_mem = false;
        }
    }
    if (!first_mem)
        cout << endl
             << "    ";
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
    CPU cpu;
    cpu.load_io();
    cpu.run();
    cpu.print_performance_report();
    return 0;
}