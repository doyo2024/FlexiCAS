#ifndef REPLAYER_READER_HPP
#define REPLAYER_READER_HPP

#include "parser.hpp"

class traceReaderBase {
public:
  traceReaderBase(ThreadID threadId, const std::string& eventDir, size_t BSize = 1024) :
    threadId(threadId), eventId(0), BufferSize(BSize), time(0.0) {
      if (threadId) 
        filename = eventDir + "/trace-" + std::to_string(threadId) + ".out";
      else 
        filename = eventDir + "/kernel.out";
      traceFile.open(filename.c_str());
      if (!traceFile.is_open()) {
        std::cerr << "Error opening file:" << filename << std::endl;
        assert(0); 
      }
    }

  ~traceReaderBase() {
    clear();
    traceFile.close();
  }
  
  const traceEvent getEvent() {
    return buffer.front();
  }
  
  void popEvent() {
    buffer.pop();
    if (buffer.empty()) 
      refill();
  }

  void clear() {
    while(!buffer.empty())
      buffer.pop();
  }
protected:
  ThreadID threadId;
  EventID eventId;

  EventBuffer buffer;
  size_t BufferSize;

  std::string filename;
  std::ifstream traceFile;
  std::string line;

  traceParser parser;

  std::chrono::duration<double, std::milli> time;
  std::chrono::duration<double, std::milli> time2;

  virtual void refill() = 0;
};

class threadReader : public traceReaderBase {
public:
  threadReader(ThreadID threadId, const std::string& eventDir) :
    traceReaderBase(threadId, eventDir) {
      refill();
    }

  void moveTo(uint64_t targetPos) {
    clear();
    traceFile.seekg(targetPos);
    refill();
  }
  
  double getTime() {
    return time.count();
  }

  double getTime2() {
    return time2.count();
  }

protected:
  void refill() {
    auto st = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < BufferSize; i++) {
      if (std::getline(traceFile, line)) {
        ++eventId;

        auto st2 = std::chrono::high_resolution_clock::now();

        parser.parseEvent(buffer, line, threadId, eventId);

        auto ed2 = std::chrono::high_resolution_clock::now();
        time2 += ed2 - st2; 

      } else {
        break;
      }
    }

    auto ed = std::chrono::high_resolution_clock::now();
    time += ed - st; 
  }
};

// class kernelReader : traceReaderBase {
// public:
//   kernelReader(ThreadID threadId, const std::string& eventDir) :
//     traceReaderBase(threadId, eventDir + "/kernel.out") {
//     }

//   void moveTo(EventID targetId) {
//     while (eventId < targetId) {
//       if (std::getline(traceFile, line)) {
//         ++eventId;
//       }
//     }
//   }

// protected:
//   void refill() {
//     for (size_t i = 0; i < BufferSize; i++) {
//       if (std::getline(traceFile, line)) {
//         ++eventId;
//         parser.parseEvent(buffer, line, threadId, eventId);
//         if (buffer.front().tag == Tag::SWITCH_TO_USER) 
//           break;
//       } else {
//         break;
//       }
//     }
//   }
// };

#endif