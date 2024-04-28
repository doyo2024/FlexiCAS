#ifndef CM_REPLAYER_SYNCHROTRACE_HPP
#define CM_REPLAYER_SYNCHROTRACE_HPP

#include "st_parser.hpp"
#include <cstdint>
enum class ThreadStatus {
  INACTIVE,
  ACTIVE,
  BLOCKED_COMM,
  BLOCKED_MUTEX,
  BLOCKED_BARRIER,
  BLOCKED_COND,
  BLOCKED_JOIN,
  COMPLETED,
  NUM_STATUSES
};

template <int BW>
struct ThreadContext
{
  ThreadID threadId;
  StEventID currEventId;
  StEventStream<BW> evStream;
  ThreadStatus status;

  ThreadContext(ThreadID threadId, const std::string& eventDir, uint32_t blockSizeBytes, uint64_t memSizeBytes)
          : threadId{threadId}, currEventId{0}, 
            evStream{threadId+1, eventDir},
            status{ThreadStatus::INACTIVE}
        {}

  // Note that a 'running' thread may be:
  // - blocked
  // - deadlocked
  //
  bool running() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::COMPLETED; }

  bool blocked() const { return status > ThreadStatus::ACTIVE && status < ThreadStatus::COMPLETED; }

  bool completed() const { return status == ThreadStatus::COMPLETED; }
};


// NT: num threads, BW: buffer size
template <ThreadID NT, int BW>
class SynchroTraceReplayer
{


private:

  /** Abstract cpi estimation for integer ops */
  const float CPI_IOPS;

  /** Abstract cpi estimation for floating point ops */
  const float CPI_FLOPS;

  /**************************************************************************
   * Synchronization state
  */
  StTracePthreadMetadata pthMetadata;

  std::vector<ThreadContext<BW>> threadContexts;

  /** stats: holds if thread can proceed past a barrier */
  std::vector<bool> perThreadBarrierBlocked;

  /** Holds which threads currently possess a mutex lock */
  std::vector<std::vector<uint64_t>> perThreadLocksHeld;

  /** Holds mutex locks in use */
  std::set<uint64_t> mutexLocks;

  /** Holds spin locks in use */
  std::set<uint64_t> spinLocks;

  /** Holds condition variables signaled by broadcasts and signals */
  std::vector<std::map<uint64_t, int>> condSignals;

  /** Holds which threads are waiting for a barrier */
  std::map<uint64_t, std::set<ThreadID>> threadBarrierMap;

};


#endif