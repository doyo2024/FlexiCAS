#ifndef REPLAYER_PARSER_HPP
#define REPLAYER_PARSER_HPP

#include <regex>
#include <fstream>
#include "event.hpp"

class traceParser {
public:
  traceParser() {

  }

  void parseEvent(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    switch(line[0]) {
      case COMP_TOKEN:
        parseComp(buffer, line, threadId, eventId);
        break;
      case MEM_TOKEN:
        parseMem(buffer, line, threadId, eventId);
        break;
      case COMM_TOKEN:
        parseComm(buffer, line, threadId, eventId);
        break;
      case THREADAPI_TOKEN:
        parseThreadAPI(buffer, line, threadId, eventId);
        break;
      case IN_KERNEL_TOKEN:
        parseInKernel(buffer, line, threadId, eventId);
        break;
      case TO_KERNEL_TOKEN:
        parseToKernel(buffer, line, threadId, eventId);
        break;
      case TO_USER_TOKEN:
        parseToUser(buffer, line, threadId, eventId);
        break;
      case END_TOKEN:
        parseEnd(buffer, line, threadId, eventId);
        break;
      default:
        std::cout << "Error: Invalid Event in thread " << threadId << " at event " << eventId << " with undefined token " << line[0] << std::endl;
        assert("0");
    }
  }

private:
  static const char COMP_TOKEN      = '0';
  static const char MEM_TOKEN       = '1';
  static const char COMM_TOKEN      = '2';
  static const char THREADAPI_TOKEN = '!';
  static const char IN_KERNEL_TOKEN = '4';
  static const char TO_KERNEL_TOKEN = '@';
  static const char TO_USER_TOKEN   = '#';
  static const char END_TOKEN       = '$';

  void parseComp(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Compute events, may contain iops or flops.
     *  Patterns: 0 pc iops flops
     */
    std::istringstream str(line);
    char token;
    addr_t pc;
    uint32_t iops, flops;

    str >> token >> std::hex >> pc >> iops;
    buffer.emplace(traceEvent::CompTag, pc, iops, iops^1);
  }

  void parseMem(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Memory Events. 
     *  Patterns: 1 pc type addr bytes
     *  type = 0, read; type = 1 writes
     */
    std::istringstream str(line);
    char token;
    addr_t pc, addr;
    uint64_t bytes;
    int type;

    str >> token >> std::hex >> pc >> type >> addr >> std::dec >> bytes;
    buffer.emplace(traceEvent::MemTag, pc, MemEvent{addr, bytes, type});
  }

  void parseComm(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Communication Events between different threads.
     *  Patterns: 2 pc addr bytes: threadId eventId;
     *  There may be multiple (threadId, eventId) pairs.
     */
    // std::regex pattern(R"(2 ([a-f0-9]+) ([a-f0-9]+) (\d+):)");
    // std::regex commPattern(R"((\d+) (\d+);)");
    // std::smatch matches;
    // int pos;
    // std::string::const_iterator pos(line.cbegin());
    char token;
    addr_t pc, addr;
    uint64_t bytes;
    ThreadID tid;
    EventID  eid;
    CommList* comm;
    std::istringstream str(line);
    
    comm = new CommList;
    str >> token >> std::hex >> pc >> addr >> std::dec >> bytes;
    while (str >> tid >> eid) {
      comm->emplace_back(eid, tid);
    }
    buffer.emplace(traceEvent::CommTag, pc, CommEvent{addr, bytes, comm});
  }

  void parseThreadAPI(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Pthread APIs. 
     *  Patterns: 3 API.addr API.type API.args
     *  This function may be changed soon, as different API may have different patterns.
     *  This event only corresponds to the entry address of a pthread API instead of any instruction,
     *  so when replay it shouldn't take any cycle.
     */

    std::istringstream str(line);
    char token;
    addr_t addr;
    ThreadAPI type;
    ThreadID targetId = 0;
    addr_t targetAddr = 0;
    uint64_t arg = 0;

    int a;
    str >> token >> std::hex >> addr >> std::dec >> a;
    type = ThreadAPI(a);
    switch (type) {
      case ThreadAPI::PTHREAD_CREATE:
      case ThreadAPI::PTHREAD_JOIN:
      {
        str >> targetId;
        break;
      }
      case ThreadAPI::PTHREAD_MUTEX_LOCK:
      case ThreadAPI::PTHREAD_MUTEX_UNLOCK:
      case ThreadAPI::PTHREAD_SPIN_LOCK:
      case ThreadAPI::PTHREAD_SPIN_UNLOCK:
      case ThreadAPI::PTHREAD_BARRIER_WAIT:
      case ThreadAPI::PTHREAD_BARRIER_DESTROY:
      {
        str >> std::hex >> targetAddr;
        break;
      }
      case ThreadAPI::PTHREAD_BARRIER_INIT:
      {
        str >> std::hex >> targetAddr >> std::dec >> arg;
        break;
      }
      default:
        break;
    }
    buffer.emplace(traceEvent::PThreadTag, addr, PThread{type, targetId, targetAddr, arg});
  }

  void parseInKernel(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /**
     * Compressed kernel trace
     * Patterns: 4 iops flops mems
     */

    std::istringstream str(line);
    char token;
    uint64_t iops, flops, mems;

    str >> token >> std::hex >> iops >> flops >> mems;
    buffer.emplace(traceEvent::InKernelTag, InKernel{iops, flops, mems});
  }

  void parseToKernel(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /**
     * Switch to Kernel Mode.
     * Patterns: 4 offset(of kernel trace)
     */
    std::istringstream str(line);
    char token;
    uint64_t offset;
    EventID eid;
    int coreId;

    str >> token >> std::dec >> offset >> eid >> coreId;
    // std::cout << "ToKernelEvent: offset: " << offset << " EID: " << eid << " CoreID: " << (int)coreId << std::endl;
    // std::cout << line << std::endl;
    buffer.emplace(traceEvent::ToKernelTag, offset, eid, (CoreID)coreId);
  }

  void parseToUser(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /**
     *  Switch to User Mode.
     *  Patterns: 5
     */
    buffer.emplace(traceEvent::ToUserTag);
  }

  void parseEnd(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** End of Events.
     *  Patterns: 6
     */
    buffer.emplace(traceEvent::EndTag);
  }
};

#endif