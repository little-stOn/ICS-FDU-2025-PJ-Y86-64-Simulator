#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <iostream>

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

class CPU
{
public:
    CPU();
    // input
    void load_io();

    // Execution
    void run();

private:
    uint64_t pc;
    int64_t reg[15];
    CC_t cc;
    Stat_t stat;

    std::map<uint64_t, uint8_t> mem;

    const uint64_t MEM_SIZE = 0x10000;

    uint8_t get_byte(uint64_t addr, bool &error);
    uint64_t get_word(uint64_t addr, bool &error);
    void set_byte(uint64_t addr, uint8_t val, bool &error);
    void set_word(uint64_t addr, int64_t val, bool &error);

    void parse_line(const std::string &line);

    void step();

    void print_state(bool first);
};

#endif