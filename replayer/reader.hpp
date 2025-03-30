#ifndef REPLAYER_READER_HPP
#define REPLAYER_READER_HPP

#include "parser.hpp"

class traceReaderBase {
public:
  traceReaderBase(ThreadID threadId, const std::string& eventDir, CoreID nCores, size_t BSize = 1024) :
    threadId(threadId), eventId(0), nCores(nCores), now(0), time(0.0) {
      // if (threadId) 
      //   filename = eventDir + "/trace-" + std::to_string(threadId) + ".out";
      // else 
      //   filename = eventDir + "/kernel.out";
      // traceFile.open(filename.c_str());
      // if (!traceFile.is_open()) {
      //   std::cerr << "Error opening file:" << filename << std::endl;
      //   assert(0); 
      // }

      if (threadId) {
        traceFile.resize(1);
        filename = eventDir + "/trace-" + std::to_string(threadId) + ".out";
        std::cout << "Opening File: " << filename << std::endl;
        traceFile[0].open(filename.c_str());
        if (!traceFile[0].is_open()) {
          std::cerr << "Error opening file:" << filename << std::endl;
          assert(0); 
        }
        BufferSize = BSize;
      } else {
        traceFile.resize(nCores);
        for (int i = 0; i < nCores; i++) {
          filename = eventDir + "/kernel-" + std::to_string(i) + ".out";
          std::cout << "Opening File: " << filename << std::endl;
          traceFile[i].open(filename.c_str());
          if (!traceFile[i].is_open()) {
            std::cerr << "Error opening file:" << filename << std::endl;
            assert(0); 
          }
        }
        BufferSize = 2;
      }
    }

  ~traceReaderBase() {
    clear();
    for (int i = 0; i < nCores; i++) {
      traceFile[i].close();
    }
    // traceFile.close();
  }
  
  const traceEvent getEvent() {
    if (buffer.empty()) 
      refill();
    return buffer.front();
  }
  
  void popEvent() {
    buffer.pop();
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

  CoreID nCores;
  CoreID now;
  std::vector<std::ifstream> traceFile;

  std::string filename;
  // std::ifstream traceFile;
  std::string line;

  traceParser parser;

  std::chrono::duration<double, std::milli> time;
  std::chrono::duration<double, std::milli> time2;

  virtual void refill() = 0;
};

class threadReader : public traceReaderBase {
public:
  threadReader(ThreadID threadId, const std::string& eventDir, CoreID nCores) :
    traceReaderBase(threadId, eventDir, nCores) {
      // refill();
    }

  void moveTo(uint64_t targetPos, CoreID coreId) {
    clear();
    now = coreId;
    traceFile[now].seekg(targetPos);
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
      if (std::getline(traceFile[now], line)) {
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