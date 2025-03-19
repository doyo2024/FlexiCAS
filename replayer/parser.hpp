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
      // case ECALL_TOKEN:
      //   parseEcall(buffer, line, threadId, eventId);
      //   break;
      case KERNEL_TOKEN:
        parseToKernel(buffer, line, threadId, eventId);
        break;
      case USER_TOKEN:
        parseToUser(buffer, line, threadId, eventId);
        break;
      case END_TOKEN:
        parseEnd(buffer, line, threadId, eventId);
        break;
      default:
        std::cout << "Error: Invalid Event in thread " << threadId << " at event " << eventId << " with undefined token " << line[0] << std::endl;
    }
  }

private:
  static const char COMP_TOKEN      = '0';
  static const char MEM_TOKEN       = '1';
  static const char COMM_TOKEN      = '2';
  static const char THREADAPI_TOKEN = '3';
  // static const char ECALL_TOKEN     = '4';        // just for test, may be deleted
  static const char END_TOKEN       = '@';
  static const char KERNEL_TOKEN    = '#';
  static const char USER_TOKEN      = '$';

  void parseComp(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Compute events, may contain iops or flops.
     *  Patterns: 0 pc iops flops
     */
    std::regex pattern(R"(0 ([a-f0-9]+) (\d+) (\d+))");
    std::smatch matches;
    addr_t pc;
    uint32_t iops, flops;

    if (std::regex_search(line.cbegin(), line.cend(), matches, pattern)) {
      pc    = std::stoul(matches[1], nullptr, 16);
      iops  = std::stoi(matches[2]);
      flops = std::stoi(matches[3]);
      buffer.emplace(traceEvent::CompTag, pc, iops, flops);
    }

    // pc    = std::stoul(line.substr(2, 16), nullptr, 16);
    // iops  = line[19] - '0';
    // flops = line[21] - '0';
    // buffer.emplace(traceEvent::CompTag, pc, iops, flops);
  }

  void parseMem(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Memory Events. 
     *  Patterns: 1 pc type addr bytes
     *  type = 0, read; type = 1 writes
     */
    std::regex pattern(R"(1 ([a-f0-9]+) (\d) ([a-f0-9]+) (\d+))");
    std::smatch matches;
    addr_t pc, addr;
    uint64_t bytes;
    int type;

    if (std::regex_search(line.cbegin(), line.cend(), matches, pattern)) {
      pc    = std::stoul(matches[1], nullptr, 16);
      type  = std::stoi(matches[2]);
      addr  = std::stoul(matches[3], nullptr, 16);
      bytes = std::stoi(matches[4]);
      buffer.emplace(traceEvent::MemTag, pc, MemEvent{addr, bytes, type});
    }

    // pc    = std::stoul(line.substr(2, 16), nullptr, 16);
    // type  = line[19] - '0';
    // addr  = std::stoul(line.substr(21, 16), nullptr, 16);
    // bytes = line[38] - '0';
    // buffer.emplace(traceEvent::MemTag, pc, MemEvent{addr, bytes, type});
  }

  void parseComm(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Communication Events between different threads.
     *  Patterns: 2 pc addr bytes: threadId eventId;
     *  There may be multiple (threadId, eventId) pairs.
     */
    std::regex pattern(R"(2 ([a-f0-9]+) ([a-f0-9]+) (\d+):)");
    std::regex commPattern(R"((\d+) (\d+);)");
    std::smatch matches;
    // int pos;
    std::string::const_iterator pos(line.cbegin());
    addr_t pc, addr;
    uint64_t bytes;
    ThreadID tid;
    EventID  eid;
    CommList* comm;

    if (std::regex_search(pos, line.cend(), matches, pattern)) {
      pc    = std::stoul(matches[1], nullptr, 16);
      addr  = std::stoul(matches[2], nullptr, 16);
      bytes = std::stoi(matches[3]);

      comm = new CommList;
      for (pos = matches.suffix().first; std::regex_search(pos, line.cend(), matches, commPattern); pos = matches.suffix().first) {
        tid = std::stoi(matches[1]);
        eid = std::stoi(matches[2]);
        comm->emplace_back(eid, tid);
      }

      buffer.emplace(traceEvent::CommTag, pc, CommEvent{addr, bytes, comm});
    }

    // pc    = std::stoul(line.substr(2, 16), nullptr, 16);
    // addr  = std::stoul(line.substr(19, 16), nullptr, 16);
    // bytes = line[36] - '0';

    // comm = new CommList;
    // pos = 38;
    // // while (pos + 1 < line.size()) {
    //   line = line.substr(pos + 1);
    //   int div = line.find(' ');
    //   pos = line.find(';');

    //   tid = std::stoi(line.substr(0, div));
    //   eid = std::stoi(line.substr(div + 1, pos - div - 1));
    //   comm->emplace_back(eid, tid);
    // // } 
    

    // buffer.emplace(traceEvent::CommTag, pc, CommEvent{addr, bytes, comm});
  }

  void parseThreadAPI(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** Pthread APIs. 
     *  Patterns: 3 API.addr API.type API.args
     *  This function may be changed soon, as different API may have different patterns.
     *  This event only corresponds to the entry address of a pthread API instead of any instruction,
     *  so when replay it shouldn't take any cycle.
     */

    std::regex pattern(R"(3 ([a-f0-9]+) (\d+))");
    std::smatch matches;
    addr_t addr;
    ThreadAPI type;
    ThreadID targetId = 0;
    addr_t targetAddr = 0;
    uint64_t arg = 0;

    if (std::regex_search(line.cbegin(), line.cend(), matches, pattern)) {
      addr = std::stoul(matches[1], nullptr, 16);
      type = ThreadAPI(std::stoi(matches[2]));
      switch (type) {
        case ThreadAPI::PTHREAD_CREATE:
        case ThreadAPI::PTHREAD_JOIN:
        {
          std::regex argsPattern(R"((\d+))");
          std::string::const_iterator pos(matches.suffix().first);
          if (std::regex_search(pos, line.cend(), matches, argsPattern)) {
            targetId = std::stoi(matches[1]);
          }
          break;
        }
        case ThreadAPI::PTHREAD_MUTEX_LOCK:
        case ThreadAPI::PTHREAD_MUTEX_UNLOCK:
        case ThreadAPI::PTHREAD_SPIN_LOCK:
        case ThreadAPI::PTHREAD_SPIN_UNLOCK:
        case ThreadAPI::PTHREAD_BARRIER_WAIT:
        case ThreadAPI::PTHREAD_BARRIER_DESTROY:
        {
          std::regex argsPattern(R"(([a-f0-9]+))");
          std::string::const_iterator pos(matches.suffix().first);
          if (std::regex_search(pos, line.cend(), matches, argsPattern)) {
            targetAddr = std::stoul(matches[1], nullptr, 16);
          }
          break;
        }
        case ThreadAPI::PTHREAD_BARRIER_INIT:
        {
          std::regex argsPattern(R"(([a-f0-9]+) (\d+))");
          std::string::const_iterator pos(matches.suffix().first);
          if (std::regex_search(pos, line.cend(), matches, argsPattern)) {
            targetAddr = std::stoul(matches[1], nullptr, 16);
            arg = std::stoi(matches[2]);
          }
          break;
        }
        default:
          break;
      }
      buffer.emplace(traceEvent::PThreadTag, addr, PThread{type, targetId, targetAddr, arg});
    }

    // addr = std::stoul(line.substr(2, 16), nullptr, 16);
    // type = ThreadAPI(line[19] - '0');
    // switch (type) {
    //   case ThreadAPI::PTHREAD_CREATE:
    //   case ThreadAPI::PTHREAD_JOIN:
    //   {
    //     targetId = std::stoi(line.substr(21));
    //     break;
    //   }
    //   case ThreadAPI::PTHREAD_MUTEX_LOCK:
    //   case ThreadAPI::PTHREAD_MUTEX_UNLOCK:
    //   case ThreadAPI::PTHREAD_SPIN_LOCK:
    //   case ThreadAPI::PTHREAD_SPIN_UNLOCK:
    //   case ThreadAPI::PTHREAD_BARRIER_WAIT:
    //   case ThreadAPI::PTHREAD_BARRIER_DESTROY:
    //   {
    //     targetAddr = std::stoul(line.substr(21), nullptr, 16);
    //     break;
    //   }
    //   case ThreadAPI::PTHREAD_BARRIER_INIT:
    //   {
    //     targetAddr = std::stoul(line.substr(21, 16), nullptr, 16);
    //     arg = std::stoi(line.substr(38));
    //     break;
    //   }
    //   default:
    //     break;
    // }
    // buffer.emplace(traceEvent::PThreadTag, addr, PThread{type, targetId, targetAddr, arg});
  }

  // void parseEcall(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
  //   /** Ecall Events.
  //    *  Patterns: 4 pc id
  //    *  Just for tests right now, may be deleted soon.
  //    */
  //   std::regex pattern(R"(4 ([a-f0-9]+) (\d+))");
  //   std::smatch matches;
  //   addr_t pc;
  //   uint64_t id;

  //   if (std::regex_search(line.cbegin(), line.cend(), matches, pattern)) {
  //     pc = std::stoi(matches[1]);
  //     id = std::stoi(matches[2]);
  //     buffer.emplace(Tag::ECALL, pc, id);
  //   }
  // }

  void parseToKernel(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /**
     * Switch to Kernel Mode.
     * Patterns: $ offset(of kernel trace)
     */
    std::regex pattern(R"(# (\d+) (\d+))");
    std::smatch matches;
    uint64_t offset;
    EventID eid;

    // line = line.substr(2);
    // int pos = line.find(' ');
    // offset = std::stoi(line.substr(0, pos));
    // eid = std::stoi(line.substr(pos + 1));

    // buffer.emplace(traceEvent::ToKernelTag, offset, eid);

    if (std::regex_search(line.cbegin(), line.cend(), matches, pattern)) {
      offset = std::stoi(matches[1]);
      eid = std::stoi(matches[2]);
      buffer.emplace(traceEvent::ToKernelTag, offset, eid);
    }
  }

  void parseToUser(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /**
     *  Switch to User Mode.
     *  Patterns: $
     */
    // if (line[0] == '$') {
      buffer.emplace(traceEvent::ToUserTag);
    // }
  }

  void parseEnd(EventBuffer& buffer, std::string line, ThreadID threadId, EventID eventId) {
    /** End of Events.
     *  Patterns: @
     */
    // if (line[0] == '@') {
      buffer.emplace(traceEvent::EndTag);
    // }
  }
};

#endif