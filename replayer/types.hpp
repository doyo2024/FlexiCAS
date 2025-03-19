#ifndef REPALYER_ENCODING_HPP
#define REPALYER_ENCODING_HPP

#include <cstdint>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>

#include <vector>
#include <queue>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

// maybe just for test

#include <chrono>

/********** common useful types **********/

typedef uint64_t insn_bits_t;
typedef uint64_t addr_t;

typedef uint8_t CoreID;
typedef int16_t ThreadID;
typedef uint32_t CurEventID;
typedef uint64_t EventID;

/********** for Output **********/

#define output(width, var) std::hex << std::setw(width) << std::setfill('0') << var

/********** for Communication Events **********/

typedef std::pair<EventID, ThreadID> CommInfo;
typedef std::vector<CommInfo> CommList; // buffer for communication info

/********** for Shadow Memory **********/

#define SMbits 12
const addr_t SMmask = (1 << SMbits) - 1;

/********** for instruction decode **********/

#define insn_length(x) \
  (((x) & 0x03) < 0x03 ? 2 : \
   ((x) & 0x1f) < 0x1f ? 4 : \
   ((x) & 0x3f) < 0x3f ? 6 : \
   8)

#define MATCH_C_ADD 0x9002
#define MATCH_C_BEQZ 0xc001
#define MATCH_C_BNEZ 0xe001
#define MATCH_C_J 0xa001
#define MATCH_C_JAL 0x2001
#define MATCH_C_JALR 0x9002
#define MATCH_C_JR 0x8002
#define MATCH_C_MV 0x8002
#define MATCH_C_SLLI 0x2
#define MATCH_ECALL 0x73
#define MATCH_MRET 0x30200073
#define MATCH_SRET 0x10200073

/********** for user mode and kernel mode **********/

// const addr_t KERNEL_ADDR = 0x7fff00000000;
// const addr_t MACHINE_ADDR = 0x80000000;
const addr_t KERNEL_ADDR = 0x80000000;  // used to detect when to swtich to kernel mode, may be deleted soon

/********** for read and write **********/

enum class ReqType: uint8_t {
  REQ_READ,
  REQ_WRITE
};

/********** for thread API **********/

enum class ThreadAPI : uint8_t {
  UNDEFINED,
  PTHREAD_CREATE,
  PTHREAD_JOIN,
  PTHREAD_MUTEX_LOCK,
  PTHREAD_MUTEX_UNLOCK,
  PTHREAD_SPIN_LOCK,
  PTHREAD_SPIN_UNLOCK,
  PTHREAD_BARRIER_INIT,
  PTHREAD_BARRIER_WAIT,
  PTHREAD_BARRIER_DESTROY,
  PTHREAD_COND_WAIT,
  PTHREAD_COND_SIGNAL
};

#endif