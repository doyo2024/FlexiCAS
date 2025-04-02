#ifndef REPLAYER_THREAD_HPP
#define REPLAYER_THREAD_HPP

#include "event.hpp"
#include "reader.hpp"

enum class ThreadStatus {
  INACTIVE,
  ACTIVE,
  ACTIVE_TRYLOCK,
  WAIT_LOCK,
  WAIT_COMM,
  WAIT_COMPUTE,
  WAIT_MEMORY,
  WAIT_THREAD,
  WAIT_SCHED,
  BLOCKED_MUTEX,
  BLOCKED_BARRIER,
  BLOCKED_JOIN,
  BLOCKED_COMM,
  BLOCKED_KERNEL,
  COMPLETED,
  NUM_STATUSES
};

const char* toString(ThreadStatus status) {
  switch (status) {
    case ThreadStatus::INACTIVE:
      return "INACTIVE";
    case ThreadStatus::ACTIVE:
      return "ACTIVE";
    case ThreadStatus::ACTIVE_TRYLOCK:
      return "ACTIVE_TRYLOCK";
    case ThreadStatus::WAIT_LOCK:
      return "WATI_LOCK";
    case ThreadStatus::WAIT_THREAD:
      return "WAIT_THREAD";
    case ThreadStatus::WAIT_MEMORY:
      return "WAIT_MEMORY";
    case ThreadStatus::WAIT_COMPUTE:
      return "WAIT_COMPUTE";
    case ThreadStatus::WAIT_SCHED:
      return "WAIT_SCHED";
    case ThreadStatus::WAIT_COMM:
      return "WAIT_COMM";
    case ThreadStatus::BLOCKED_MUTEX:
      return "BLOCKED_MUTEX";
    case ThreadStatus::BLOCKED_BARRIER:
      return "BLOCKED_BARRIER";
    case ThreadStatus::BLOCKED_JOIN:
      return "BLOCKED_JOIN";
    case ThreadStatus::BLOCKED_COMM:
      return "BLOCKED_COMM";
    case ThreadStatus::BLOCKED_KERNEL:
      return "BLOCKED_KERNEL";
    case ThreadStatus::COMPLETED:
      return "COMPLETED";
    default:
      std::cerr << "Unexpected Thread Status" << std::endl;
      assert(0);
  }
}

struct ThreadContext {
  ThreadID threadId;
  CoreID coreId;
  EventID curEventId;
  ThreadStatus status;
  uint64_t wakeupClock;
  uint64_t restSliceCycles;

  threadReader traces;

  std::chrono::duration<double, std::milli> time;

  ThreadContext(ThreadID threadId, const std::string& eventDir, CoreID nCores)
        : threadId{threadId}, coreId(0), curEventId{1}, 
            status{ThreadStatus::INACTIVE},
            wakeupClock(0),
            restSliceCycles(0),
            traces{threadId, eventDir, nCores},
            time(0.0)
        {}

  // Note that a 'running' thread may be:
  // - deadlocked
  //
  bool active() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::WAIT_LOCK; }

  bool running() const { return status > ThreadStatus::INACTIVE && status < ThreadStatus::BLOCKED_MUTEX; }

  bool waiting() const { return status > ThreadStatus::ACTIVE_TRYLOCK && status < ThreadStatus::BLOCKED_MUTEX; }

  bool blocked() const { return status > ThreadStatus::WAIT_SCHED && status < ThreadStatus::COMPLETED; }

  bool completed() const { return status == ThreadStatus::COMPLETED; }
};

class ThreadSchedulerBase {
protected:

  /** clock */
  std::vector<uint64_t> clock;
  std::vector<uint32_t> checkCount;

  /** how many threads are runnning on each core */
  std::vector<int> workingCount;
  
  /** the threads runnning on each core */
  typedef std::deque<ThreadContext*> ThreadQueue;
  std::vector<ThreadQueue> threadQueue;

  /** Holds which threads are waiting for communication */
  std::vector<std::priority_queue<CommInfo>> commQueue;
  std::mutex commMtx;

  /** each thread's current EventID */
  std::vector<EventID> curEvent;
  std::mutex eventMtx;

  /** wakeup signal for each thread */
  std::vector<bool> wakeupSig;
  std::mutex wakeupMtx;

  /** whether this core has switched to kernel mode */
  std::vector<bool> inKernel;

  /** the last running thread before switch to kernel mode */
  std::vector<ThreadID> lastUser;

  /** mutex for output */
  std::mutex outputMtx;

public:

  /** Abstract cpi estimation for integer ops */
  const float CPI_IOPS;

  /** Abstract cpi estimation for floating point ops */
  const float CPI_FLOPS;

  /** Cycles for context switch on a core */
  const uint32_t cxtSwitchCycles;

  /** Cycles for pthread event */
  const uint32_t pthCycles;

  /** Cycles to simulate the time slice the scheduler gives to a thread */
  const uint32_t schedSliceCycles;
  
  ThreadSchedulerBase(float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, uint32_t pthCycles, uint32_t schedSliceCycles) :
   CPI_IOPS(CPI_IOPS), CPI_FLOPS(CPI_FLOPS), cxtSwitchCycles(cxtSwitchCycles), pthCycles(pthCycles),  
   schedSliceCycles(schedSliceCycles) {

  }

  inline uint64_t curClock(CoreID coreId) const { return clock[coreId]; }
  uint64_t nextClock(CoreID coreId) { return ++clock[coreId]; }

  virtual CoreID threadIdToCoreId(ThreadID threadId) const = 0;

  virtual void init(ThreadContext* tcxt) = 0;

  virtual void recordEvent(ThreadContext* tcxt) = 0;
  virtual EventID checkEvent(ThreadID threadId) = 0;

  virtual void sendReady(ThreadID threadId) = 0;
  virtual bool checkReady(ThreadID threadId) = 0;
  virtual void getReady(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void getBlocked(ThreadContext* tcxt, ThreadStatus status) = 0;

  virtual void toKernel(CoreID coreId) = 0;
  virtual void toUser(CoreID coreId) = 0;

  virtual void waitComm(ThreadID threadId, CommInfo comm) = 0;

  virtual void swapByID(CoreID coreId, ThreadID threadId) = 0;
  virtual bool tryCxtSwap(CoreID coreId) = 0;
  virtual void schedule(ThreadContext* tcxt, uint64_t cycles) = 0;

  virtual ThreadID findActive(CoreID coreId) = 0;
  virtual void checkStatus(CoreID coreId) = 0;
  
  virtual void wakeupDebugLog(CoreID coreId) = 0;
};

// NT: num threads, NC: num cores
template <ThreadID NT, CoreID NC>
class ThreadScheduler : public ThreadSchedulerBase
{
public:
  ThreadScheduler(float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles)
  : ThreadSchedulerBase(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles) 
  {
    clock.resize(NC);
    checkCount.resize(NC);
    threadQueue.resize(NC); 
    workingCount.resize(NC);
    inKernel.resize(NC);
    lastUser.resize(NC);

    wakeupSig.resize(NT + 1);
    curEvent.resize(NT + 1);
    commQueue.resize(NT + 1);

    for (CoreID i = 0; i < NC; i++) {
      clock[i] = 0;
      checkCount[i] = 0;
      workingCount[i] = 0;
      inKernel[i] = 0;
    }
    for (ThreadID i = 1; i <= NT; i++) {
      wakeupSig[i] = false;
      curEvent[i] = 0;
    }
  }

  CoreID threadIdToCoreId(ThreadID threadId) const{
    return (threadId - 1) % NC;
  } 

  virtual void init(ThreadContext* tcxt) {
    if (tcxt->threadId)  // if this is not kernel, allocate it to a core.
      tcxt->coreId = threadIdToCoreId(tcxt->threadId);
    threadQueue.at(tcxt->coreId).emplace_back(tcxt);
  }

  virtual void recordEvent(ThreadContext* tcxt) {
    eventMtx.lock();
    curEvent[tcxt->threadId] = tcxt->curEventId;
    eventMtx.unlock();
  }

  virtual EventID checkEvent(ThreadID threadId) {
    eventMtx.lock();
    EventID eventId = curEvent[threadId];
    eventMtx.unlock();
    return eventId;
  }

  virtual void sendReady(ThreadID threadId) {
    wakeupMtx.lock();
    wakeupSig[threadId] = true;
    wakeupMtx.unlock();
  }

  virtual bool checkReady(ThreadID threadId) {
    wakeupMtx.lock();
    bool ready = wakeupSig[threadId];
    wakeupMtx.unlock();
    return ready;
  }

  /** Switch a thread to ready status. */
  virtual void getReady(ThreadContext* tcxt, CoreID coreId) {
    if (tcxt->running())
      return;

    wakeupMtx.lock();
    wakeupSig[tcxt->threadId] = false;
    wakeupMtx.unlock();

    if (tcxt->status == ThreadStatus::BLOCKED_MUTEX)
      tcxt->status = ThreadStatus::WAIT_LOCK;
    else
      tcxt->status = ThreadStatus::WAIT_SCHED;

    workingCount[coreId]++;

    if (workingCount[coreId] == 1) {
      // if this is the only active thread on the core,
      // switch it on core immediately.
      swapByID(coreId, tcxt->threadId);
    }

    schedule(tcxt, pthCycles);
  }

  virtual void getBlocked(ThreadContext* tcxt, ThreadStatus status) {
    if (tcxt->blocked()) 
      return;

    tcxt->status = status;
    CoreID coreId = tcxt->coreId;
    workingCount[coreId]--;
    (void)tryCxtSwap(coreId);
  }

  virtual void toKernel(CoreID coreId) {
    ThreadContext* tcxt = threadQueue[coreId].front();
    inKernel[coreId] = true;
    lastUser[coreId] = tcxt->threadId;
    swapByID(coreId, 0);
    ThreadContext* kcxt = threadQueue[coreId].front();
    kcxt->status = ThreadStatus::ACTIVE;
  }

  virtual void toUser(CoreID coreId) {
    inKernel[coreId] = false;
    swapByID(coreId, lastUser[coreId]);
    ThreadContext* tcxt = threadQueue[coreId].front();
    tcxt->status = ThreadStatus::ACTIVE;
    // outputMtx.lock();
    // std::cout << "switch back to user thread " << tcxt->threadId << std::endl;
    // outputMtx.unlock();
  }

  virtual void waitComm(ThreadID threadId, CommInfo comm) {
    commMtx.lock();
    commQueue[comm.second].push(std::make_pair(comm.first, threadId));
    commMtx.unlock();
  }

  virtual void swapByID(CoreID coreId, ThreadID threadId) {
    auto& threadsOnCore = threadQueue[coreId];
    assert(threadsOnCore.size() > 0);

    auto it = std::find_if(threadsOnCore.begin(),
                           threadsOnCore.end(),
                           [threadId](const ThreadContext* tcxt)
                           { return tcxt->threadId == threadId; });
    assert(it != threadsOnCore.end());
    std::rotate(threadsOnCore.begin(), it, threadsOnCore.end());
  }

  virtual bool tryCxtSwap(CoreID coreId) {
    auto& threadsOnCore = threadQueue[coreId];
    assert(threadsOnCore.size() > 0);

    auto it = std::find_if(std::next(threadsOnCore.begin()),
                           threadsOnCore.end(),
                           [](const ThreadContext* tcxt)
                           { return tcxt->running(); });

    // if no threads were found that could be swapped
    if (it == threadsOnCore.end())
      return false;

    // else we found a thread to swap.
    // Rotate threads round-robin and schedule the context swap.
    std::rotate(threadsOnCore.begin(), it, threadsOnCore.end());
    ThreadContext* tcxt = threadsOnCore.front();
    if (tcxt->status == ThreadStatus::ACTIVE_TRYLOCK || tcxt->status == ThreadStatus::WAIT_LOCK)
      tcxt->status = ThreadStatus::WAIT_LOCK;
    else
      tcxt->status = ThreadStatus::WAIT_SCHED;
    schedule(tcxt, cxtSwitchCycles);
    return true;
  }

  virtual void schedule(ThreadContext* tcxt, uint64_t cycles) {
    assert(tcxt->threadId <= NT);

    clock[tcxt->coreId] += cycles;

    // CoreID coreId = threadIdToCoreId(tcxt->threadId);
    tcxt->wakeupClock = curClock(tcxt->coreId) + cycles - 1;
    if (tcxt->threadId && tcxt->status != ThreadStatus::WAIT_SCHED && !tcxt->blocked())
      tcxt->restSliceCycles -= cycles;
  }

  virtual ThreadID findActive(CoreID coreId) {
    if (workingCount[coreId] == 0)
      return -1;

    ThreadContext* tcxt = threadQueue[coreId].front();
    if (!tcxt->active())
      return -1;

    if (tcxt->status == ThreadStatus::ACTIVE) {
      if (tcxt->restSliceCycles <= 0) {                   // if the thread has used up its slice
        tcxt->restSliceCycles = schedSliceCycles;
        if (tcxt->threadId && tryCxtSwap(coreId)) {
          tcxt->status = ThreadStatus::WAIT_SCHED;
          tcxt->wakeupClock = curClock(coreId) + schedSliceCycles;
        }
      }
    }

    // if the thread replays an unsuccessful COMM/MUTEX_LOCK, it must haven't used up its slice
    // so there is no need to check it.

    return tcxt->threadId;
  }

  virtual void checkStatus(CoreID coreId) {
    ThreadQueue threadsOnCore = threadQueue[coreId];
    for (auto tcxt : threadsOnCore) {
      switch (tcxt->status) {
        case ThreadStatus::WAIT_LOCK:
          if(curClock(coreId) >= tcxt->wakeupClock) {
            tcxt->status = ThreadStatus::ACTIVE_TRYLOCK;
          }
          break;
        case ThreadStatus::WAIT_COMPUTE:
        case ThreadStatus::WAIT_MEMORY:
        case ThreadStatus::WAIT_THREAD:
        case ThreadStatus::WAIT_SCHED:
        case ThreadStatus::WAIT_COMM:
          if(curClock(coreId) >= tcxt->wakeupClock) {
            tcxt->status = ThreadStatus::ACTIVE;
          }
          break;
        case ThreadStatus::ACTIVE:
        case ThreadStatus::ACTIVE_TRYLOCK: 
          break;
        case ThreadStatus::BLOCKED_MUTEX:
        case ThreadStatus::BLOCKED_BARRIER:
        case ThreadStatus::BLOCKED_JOIN:
        case ThreadStatus::BLOCKED_COMM:
        case ThreadStatus::INACTIVE:
          if (checkCount[coreId] % 100 == 0 && checkReady(tcxt->threadId))
            getReady(tcxt, coreId);
          break;
        case ThreadStatus::BLOCKED_KERNEL: 
          // When the replay of kernel trace ends, it has done necessary operations to switch to next thread,
          // so there is no need to wait for schedule.
        case ThreadStatus::COMPLETED:
        default:
          break;
      }

      if (tcxt->threadId && checkCount[coreId] % 100 == 0) {
        recordEvent(tcxt);
        commMtx.lock();
        while (!commQueue[tcxt->threadId].empty() && commQueue[tcxt->threadId].top().first < tcxt->curEventId) {
          sendReady(commQueue[tcxt->threadId].top().second);
          commQueue[tcxt->threadId].pop();
        }
        commMtx.unlock();
      }
    }

    if (checkCount[coreId] == 9999) {
      // wakeupDebugLog(coreId);
      checkCount[coreId] = 0;
    } else {
      checkCount[coreId]++;
    }
  }

  virtual void wakeupDebugLog(CoreID coreId) {
    if (!workingCount[coreId])
      return;

    outputMtx.lock();
    printf("CoreID<%d>:clock:%ld\n", coreId, curClock(coreId));
    for (const auto cxt : threadQueue[coreId]) {
      if (cxt->threadId)
        printf("Thread<%d>:", cxt->threadId);
      else
        printf("Kernel<%d>:", coreId);
      printf("Event<%ld>:Status<%s>", cxt->curEventId, toString(cxt->status));

      if (cxt->threadId == threadQueue[coreId].front()->threadId)
        printf(" OnCore=True");
      else if (cxt->status == ThreadStatus::BLOCKED_KERNEL)
        printf(" In Kernel");
      else 
        printf(" OnCore=False");

      if (cxt->waiting())
        printf(" wakeupClock=%lu\n", cxt->wakeupClock);
      else
        printf("\n");
    }
    
    outputMtx.unlock();
  }
};

#endif