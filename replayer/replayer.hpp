#ifndef REPLAYER_SCHEDULER_HPP
#define REPLAYER_SCHEDULER_HPP

#include <thread>

#include "cache/coherence.hpp"
#include "event.hpp"
#include "reader.hpp"
#include "thread.hpp"
#include "cache/coherence.hpp"

class traceReplayerBase
{
protected:

 /**************************************************************************
   * Synchronization state
  */
  
  /** the count of uncompleted threads on each core */
  std::vector<ThreadID> threadCount;

  /** the context for each thread and kernel */
  std::vector<ThreadContext *> threadContexts;
  std::vector<ThreadContext *> kernelContexts;  // the kernel's context for each core

  // /** Holds which threads are waiting for communication */
  // std::vector<std::priority_queue<CommInfo>> commQueue;
  // std::mutex commMtx;

  /** about mutex*/
  std::map<addr_t, std::queue<ThreadID>> perMutexThread;
  std::mutex mutexMtx;

  /** about spinlocks */
  std::set<addr_t> spinLocks;
  std::mutex spinLocksMtx;

  /** about the replay of pthread barrier */
  std::unordered_map<addr_t, uint64_t> barrierMap;            // the number of threads that need to wait for a barrier
  std::map<addr_t, std::vector<ThreadID>> barrierToThread;    // the threads waiting for a barrier
  std::mutex barrierMtx;

  /** Holds which threads currently possess a mutex lock */
  std::vector<std::vector<addr_t>> perThreadLocksHeld;

  /** Holds which threads waiting for the thread to complete */
  std::vector<std::vector<ThreadID>> joinList;
  std::mutex joinMtx;

  /** Directory of trace file */
  std::string eventDir;

  /** data cache */
  std::vector<CoreInterfaceBase *>& core_data;

  /** instruction cache */
  std::vector<CoreInterfaceBase *>& core_inst;
  std::mutex cacheMtx;

  /** mutex for output */
  std::mutex outputMtx;

  /** threads for each core */
  std::vector<std::thread> replayCore;

  uint64_t memDelay;

  virtual bool checkComm(const CommInfo comm) = 0;
  virtual uint64_t accessDCache(CoreID coreId, uint64_t addr, uint32_t bytes, ReqType type) = 0;
  virtual uint64_t accessICache(CoreID coreId, uint64_t addr) = 0;

  virtual bool mutexTryLock(ThreadContext* tcxt, CoreID coreId, uint64_t pthaddr) = 0;
  virtual void mutexUnlock(ThreadContext* tcxt, CoreID coreId, uint64_t pthaddr) = 0;

  virtual void SwitchToKernel(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void SwitchToUser(ThreadContext* tcxt, CoreID coreId) = 0;

  virtual void replayComp(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void replayMem(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void replayComm(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void replayEnd(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void replayThreadAPI(ThreadContext* tcxt, CoreID coreId) = 0;
  virtual void replayKernel(ThreadContext* tcxt, CoreID coreId) = 0;
  // virtual void replayEcall(ThreadContext* tcxt, CoreID coreId) = 0;

  virtual void wakeupCore(ThreadID threadId, CoreID coreId) = 0;

public:
  traceReplayerBase(std::string eventDir, std::vector<CoreInterfaceBase *>& core_data, std::vector<CoreInterfaceBase *>& core_inst) : 
   eventDir(eventDir), core_data(core_data), core_inst(core_inst) {

  }

  virtual void init()  = 0;
  virtual void start() = 0;
};

// NT: num threads, NC: num cores
template <ThreadID NT, CoreID NC>
class traceReplayer : public traceReplayerBase
{
private:
  ThreadScheduler<NT, NC> scheduler;

protected:
  virtual void SwitchToKernel(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::SWITCH_TO_KERNEL);

    tcxt->status = ThreadStatus::BLOCKED_KERNEL;
    tcxt->traces.popEvent();
    tcxt->curEventId++;
    scheduler.toKernel(coreId);

    ThreadContext* kcxt = kernelContexts[coreId];
    kcxt->traces.moveTo(ev.evMark.info, ev.evMark.coreId);
    kcxt->curEventId = ev.evMark.eventId;
    // std::cout << "Thread " << tcxt->threadId << " Event " << tcxt->curEventId << " " << " On Core " << (int)coreId << ":\n";
    // std::cout << " Switch to Core: Trace: " << (int)ev.evMark.coreId << "Pos: " << ev.evMark.info << " Event: " << kcxt->curEventId << std::endl;
  }

  virtual void SwitchToUser(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::SWITCH_TO_USER && tcxt->threadId == 0);

    tcxt->status = ThreadStatus::COMPLETED;  // always change the kernel status to COMPLETED before swtich to user mode
    tcxt->traces.popEvent();
    tcxt->curEventId++;
    scheduler.toUser(coreId);
  }

  /**
   * Access instruction cache.
   * Call the API offered by FlexiCAS.
   */
  virtual uint64_t accessICache(CoreID coreId, uint64_t addr) {
    uint64_t delay = 0;
    cacheMtx.lock();
    core_inst[coreId]->read(addr, &delay);
    cacheMtx.unlock();
    return delay;
  }

  virtual void replayComp(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::COMPUTE);

    uint64_t icacheDelay = accessICache(coreId, ev.pc);

    // simulate time for the iops/flops
    scheduler.schedule(tcxt,
            icacheDelay + scheduler.CPI_IOPS * ev.compEvent.iops + scheduler.CPI_FLOPS * ev.compEvent.flops);
    // tcxt->status = ThreadStatus::WAIT_COMPUTE;
    tcxt->traces.popEvent();
    tcxt->curEventId++;
  }

  /**
   * Access data cache.
   * Call the API offered by FlexiCAS.
   */
  virtual uint64_t accessDCache(CoreID coreId, uint64_t addr, uint32_t bytes, ReqType type) {
    uint64_t delay = 0;

    cacheMtx.lock();
    if(type == ReqType::REQ_READ)
      core_data[coreId]->read(addr, &delay);
    else
      core_data[coreId]->write(addr, nullptr, &delay);
    cacheMtx.unlock();

    memDelay = delay;
    return delay;
  }

  virtual void replayMem(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent& ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::MEMORY);
    // Send the load/store

    uint64_t icacheDelay = accessICache(coreId, ev.pc);
    uint64_t dcacheDelay = accessDCache(coreId, ev.commEvent.addr, ev.commEvent.bytes, ReqType::REQ_READ);
    scheduler.schedule(tcxt, icacheDelay + dcacheDelay);
    // tcxt->status = ThreadStatus::WAIT_MEMORY;
    tcxt->traces.popEvent();
    tcxt->curEventId++;
  }

  /**
   * Check if the producer has reached the dependent event. 
   */
  virtual bool checkComm(const CommInfo comm) {
    return (!comm.second) || (scheduler.checkEvent(comm.second) > comm.first);
  }

  virtual void replayComm(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent& ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::COMMUNICATION);

    bool blocked = false;

    if (perThreadLocksHeld[tcxt->threadId].empty()) {
      // if the thread holds a mutex or spinlock, don't check the communication dependency to avoid deadlock
      for (auto it : *(ev.commEvent.comm)) {
        if (checkComm(it))
          continue;
        
        blocked = true;
        scheduler.waitComm(tcxt->threadId, it);
        // commMtx.lock();
        // commQueue[it->second].push(std::make_pair(it->first, tcxt->threadId));
        // commMtx.unlock();
      }
    }

    if (blocked) {
      scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_COMM);
    } else {
      // If the dependency is not satisfied, visit the memory.
      uint64_t icacheDelay = accessICache(coreId, ev.pc);
      uint64_t dcacheDelay = accessDCache(coreId, ev.commEvent.addr, ev.commEvent.bytes, ReqType::REQ_READ);
      scheduler.schedule(tcxt, icacheDelay + dcacheDelay);
      // tcxt->status = ThreadStatus::WAIT_MEMORY;

      delete ev.commEvent.comm;
      tcxt->traces.popEvent();
      tcxt->curEventId++;
    }

    // for (auto it = ev.commEvent.comm->begin(); it != ev.commEvent.comm->end(); it++) {
    //   if (checkComm(*it) || !perThreadLocksHeld[tcxt->threadId].empty())
    //     continue;

    //   // If the dependency is not satisfied, schedule to try again.
    //   tcxt->status = ThreadStatus::WAIT_COMM;
    //   if (!scheduler.tryCxtSwap(coreId))
    //     scheduler.schedule(tcxt, 1);
    //   else
    //     scheduler.schedule(tcxt, scheduler.schedSliceCycles);
    //   return;
    // }

    // // If the dependency is not satisfied, visit the memory.
    // scheduler.schedule(tcxt, accessDCache(coreId, ev.commEvent.addr, ev.commEvent.bytes, ReqType::REQ_READ));
    // tcxt->status = ThreadStatus::WAIT_MEMORY;

    // delete(ev.commEvent.comm);    // free the space for the communication list
    // tcxt->traces.popEvent();
    // tcxt->curEventId++;
  }

  /**
   * Compressed trace of kernel.
   * Just used to estimate the time used in kernel
   */
  virtual void replayKernel(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent& ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::KERNEL);

    uint64_t evCnt = ev.kernelEvent.flops + ev.kernelEvent.iops + ev.kernelEvent.mems;
    tcxt->curEventId += evCnt;
    scheduler.schedule(tcxt, ev.kernelEvent.iops * scheduler.CPI_IOPS +
                             ev.kernelEvent.flops * scheduler.CPI_FLOPS +
                             ev.kernelEvent.mems * memDelay);
    tcxt->traces.popEvent();
  }

  /**
   * The end of a thread.
   * Note that kernel will not replay this kind of event,
   * because it always switches back to user thread at the end of its trace.
   */
  virtual void replayEnd(ThreadContext* tcxt, CoreID coreId) {
    tcxt->traces.popEvent();
    tcxt->curEventId++;
    scheduler.recordEvent(tcxt);

    threadCount[coreId]--;

    if (tcxt->coreId) {
      outputMtx.lock();
      std::cout << "Thread " << tcxt->threadId << " finished on Core " << (int)coreId << std::endl;
      outputMtx.unlock();
    }
    

    joinMtx.lock();
    scheduler.getBlocked(tcxt, ThreadStatus::COMPLETED);
    for (ThreadID threadId : joinList[tcxt->threadId])
      scheduler.sendReady(threadId);
    joinMtx.unlock();
  }

  /**
   * Try to get a mutex.
   * Return true if the thread succeed to get the mutex, else return false;
   * For each mutex maintain a queue which holds the threadId waiting for it. 
   * The thread at the front of the queue holds the mutex.
   * 
   * When a thread try to get a mutex for the first time, it will be in status ACTIVE.
   * If the thread has tried to get the mutex before, it will be in status ACTIVE_TRYLOCK, 
   * and it should not be add to the waiting queue again. 
   * This is just an optimization to avoid traversing the queue each time.
   */
  virtual bool mutexTryLock(ThreadContext* tcxt, CoreID coreId, addr_t pthaddr) {
    mutexMtx.lock();

    auto it = perMutexThread.find(pthaddr);   // check wether the mutex has appeared
    if (it != perMutexThread.end()) {
      if (tcxt->status != ThreadStatus::ACTIVE_TRYLOCK) {
        it->second.push(tcxt->threadId);
      }
      bool getMutex = (tcxt->threadId == it->second.front());

      mutexMtx.unlock();
      return getMutex;
    } else {
      perMutexThread[pthaddr].push(tcxt->threadId);

      mutexMtx.unlock();
      return true;
    }
  }

  /**
   * Try to give up a mutex.
   */
  virtual void mutexUnlock(ThreadContext* tcxt, CoreID coreId, addr_t pthaddr) {
    mutexMtx.lock();

    auto it = perMutexThread.find(pthaddr);
    assert(it != perMutexThread.end());
    assert(!(it->second.empty()));
    assert(it->second.front() == tcxt->threadId);
    it->second.pop();
    assert(((!it->second.empty()) && (it->second.front() != tcxt->threadId))
          || (it->second.empty()));
    if (!(it->second.empty())) {
      scheduler.sendReady(it->second.front());  // wake up a thread waiting for the mutex if such thread exists
    }

    mutexMtx.unlock();
  }

  /**
   * Replay important pthread API.
   * Note these events match the entry of each pthread API instead of any instruction,
   * and the operations of these API have been recorded in trace.
   * So it takes 0 cycle to replay them.
   */

  virtual void replayThreadAPI(ThreadContext* tcxt, CoreID coreId) {
    const traceEvent& ev = tcxt->traces.getEvent();
    assert(ev.tag == Tag::PTHREAD);
    const addr_t pthAddr = ev.pThread.targetAddr;

    switch (ev.pThread.type) {
      case ThreadAPI::PTHREAD_CREATE:
      {
        assert(tcxt->status == ThreadStatus::ACTIVE);

        const ThreadID newThreadId = ev.pThread.targetId;

        outputMtx.lock();
        std::cout << "Thread " << tcxt->threadId << " on Core " << (int)coreId << " at Event " << tcxt->curEventId << std::endl; 
        std::cout << " New Thread ID: " << newThreadId << " Total Thread Number: " << threadContexts.size() - 1 << std::endl;
        outputMtx.unlock();

        assert(newThreadId < threadContexts.size());
        assert(threadContexts[newThreadId]->status == ThreadStatus::INACTIVE);

        scheduler.sendReady(newThreadId);
        
        // tcxt->status = ThreadStatus::WAIT_THREAD;
        // scheduler.schedule(tcxt, 0);
        
        tcxt->traces.popEvent();
        tcxt->curEventId++;
        break;
      }
      case ThreadAPI::PTHREAD_JOIN:
      {
        const ThreadContext* targettcxt = threadContexts[ev.pThread.targetId];

        joinMtx.lock();

        if(targettcxt->completed()) {

          joinMtx.unlock();

          // tcxt->status = ThreadStatus::WAIT_THREAD;

          outputMtx.lock();
          std::cout << "Thread " << tcxt->threadId << " on Core " << (int)coreId << " at Event " << tcxt->curEventId << std::endl; 
          std::cout << " Thread " << targettcxt->threadId << " joined" << std::endl; 
          outputMtx.unlock();

          tcxt->traces.popEvent();
          tcxt->curEventId++;
          // scheduler.schedule(tcxt, scheduler.pthCycles);
        } else if (targettcxt->running() || targettcxt->blocked() || scheduler.checkReady(targettcxt->threadId)) {
          joinList[targettcxt->threadId].push_back(tcxt->threadId);

          joinMtx.unlock();
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_JOIN);
        } else {

          joinMtx.unlock();

          // failed joined Thread
          assert(0);
        }
        break;
      }
      case ThreadAPI::PTHREAD_MUTEX_LOCK:
      {
        if (mutexTryLock(tcxt, coreId, pthAddr)) {
          perThreadLocksHeld[tcxt->threadId].push_back(pthAddr);
          tcxt->traces.popEvent();
          tcxt->curEventId++;
          tcxt->status = ThreadStatus::ACTIVE;
          // tcxt->status = ThreadStatus::WAIT_THREAD;
          // scheduler.schedule(tcxt, scheduler.pthCycles);
        } else {
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_MUTEX);
        }
        break;
      }
      case ThreadAPI::PTHREAD_MUTEX_UNLOCK:
      {
        std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt->threadId];
        auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);
        assert(v_it != locksHeld.end());
        locksHeld.erase(v_it);
        mutexUnlock(tcxt, coreId, ev.pThread.targetAddr);
        tcxt->traces.popEvent();
        tcxt->curEventId++;
        // tcxt->status = ThreadStatus::WAIT_THREAD;
        // scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case ThreadAPI::PTHREAD_SPIN_LOCK:
      {
        assert(tcxt->status == ThreadStatus::ACTIVE);

        spinLocksMtx.lock();
        auto p = spinLocks.insert(ev.pThread.targetAddr);
        spinLocksMtx.unlock();

        if (p.second){
          tcxt->traces.popEvent();
          tcxt->curEventId++;
          perThreadLocksHeld[tcxt->threadId].push_back(ev.pThread.targetAddr);
        }

        // Reschedule regardless of whether the lock was acquired.
        // If the lock wasn't acquired, we spin and try again the next cycle.
        // tcxt->status = ThreadStatus::WAIT_THREAD;
        // scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case ThreadAPI::PTHREAD_SPIN_UNLOCK:
      {
        assert(tcxt->status == ThreadStatus::ACTIVE);
        
        spinLocksMtx.lock();

        auto it = spinLocks.find(ev.pThread.targetAddr);
        if (it != spinLocks.end()) {
          spinLocks.erase(it);
          std::vector<uint64_t>& locksHeld = perThreadLocksHeld[tcxt->threadId];
          auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);
          assert(v_it != locksHeld.end());
          locksHeld.erase(v_it);
        }

        spinLocksMtx.unlock();

        tcxt->traces.popEvent();
        tcxt->curEventId++;
        // tcxt->status = ThreadStatus::WAIT_THREAD;
        // scheduler.schedule(tcxt, scheduler.pthCycles);
        break;
      }
      case ThreadAPI::PTHREAD_BARRIER_INIT:
      {
        barrierMtx.lock();
        barrierMap[pthAddr] = ev.pThread.arg;
        barrierMtx.unlock();

        outputMtx.lock();
        std::cout << "initialize barrier at " << output(16, pthAddr) << std::endl;
        std::cout << ev.pThread.arg << " threads wait at barrier " << output(16, pthAddr) << std::endl; 
        outputMtx.unlock();

        tcxt->traces.popEvent();
        tcxt->curEventId++;
        break;
      }
      case ThreadAPI::PTHREAD_BARRIER_WAIT:
      {
        barrierMtx.lock();
        
        barrierMap[pthAddr]--;

        outputMtx.lock();
        std::cout << barrierMap[pthAddr] << " threads still needed at barrier " << pthAddr << std::endl;
        outputMtx.unlock();

        if (barrierMap[pthAddr]) {
          barrierToThread[pthAddr].push_back(tcxt->threadId);
          barrierMtx.unlock();
          scheduler.getBlocked(tcxt, ThreadStatus::BLOCKED_BARRIER);
        } else {
          barrierMap[pthAddr] = barrierToThread[pthAddr].size() + 1;  // reset the barrier
          for (auto tid : barrierToThread[pthAddr]) {                 // unblock all the threads waiting for the barrier
            threadContexts[tid]->traces.popEvent();
            threadContexts[tid]->curEventId++;
            scheduler.sendReady(tid);
          }
          barrierMtx.unlock();
          tcxt->traces.popEvent();
          tcxt->curEventId++;
          // tcxt->status = ThreadStatus::WAIT_THREAD;
          // scheduler.schedule(tcxt, scheduler.pthCycles);
        }
        break;
      }
      case ThreadAPI::PTHREAD_BARRIER_DESTROY:
      {
        barrierMtx.lock();
        barrierMap.erase(pthAddr);
        barrierMtx.unlock();
        tcxt->traces.popEvent();
        tcxt->curEventId++;
        break;
      }
      case ThreadAPI::PTHREAD_COND_WAIT:
      // case ThreadAPI::PTHREAD_COND_SG:
      // case ThreadAPI::PTHREAD_COND_BR:
        std::cerr << "Unsupported Semaphore Event Type encountered" << std::endl; 
        break;
      default:
        std::cerr << "Unexpected Thread Event Type encountered" << std::endl;
        break;
    }
  }

  virtual void wakeupCore(ThreadID threadId, CoreID coreId) {
    ThreadContext* tcxt = threadId ? threadContexts[threadId] : kernelContexts[coreId];
    assert(tcxt->running());

    // auto st = std::chrono::high_resolution_clock::now();
    
    const traceEvent& ev = tcxt->traces.getEvent();
    switch (ev.tag) {
      case Tag::COMPUTE:
        replayComp(tcxt, coreId);
        break;
      case Tag::MEMORY:
        replayMem(tcxt, coreId);
        break;
      case Tag::COMMUNICATION:
        replayComm(tcxt, coreId);
        break;
      case Tag::PTHREAD:
        replayThreadAPI(tcxt, coreId);
        break;
      case Tag::KERNEL:
        replayKernel(tcxt, coreId);
        break;
      case Tag::SWITCH_TO_KERNEL:
        SwitchToKernel(tcxt, coreId);
        break;
      case Tag::SWITCH_TO_USER:
        SwitchToUser(tcxt, coreId);
        break;
      case Tag::END_OF_EVENTS:
        replayEnd(tcxt, coreId);
        break;
      default:
        outputMtx.lock();
        std::cout << "Undefined Tag: " << (int)ev.tag << " in Thread " << threadId << " on Core " << (int)coreId << " at Event " << tcxt->curEventId << std::endl; 
        outputMtx.unlock();
        assert(0);
    }

    // auto ed = std::chrono::high_resolution_clock::now();
    // tcxt->time += ed - st;
  }

  static void replay(CoreID coreId, traceReplayer* replayer) {

    auto st = std::chrono::high_resolution_clock::now();

    while(true) {
      // check whether all of the threads finished running
      if (replayer->threadCount[coreId] == 0)
        break;
      
      // check the status of each thread on this core
      replayer->scheduler.checkStatus(coreId);

      ThreadID tid = replayer->scheduler.findActive(coreId);
      if (tid >= 0) {
        replayer->wakeupCore(tid, coreId);
      } else {
        std::this_thread::yield();
      }

      // if(replayer->scheduler.nextClock(coreId) % 100000 == 0) {
      // if(replayer->scheduler.nextClock(coreId) % 100000 < 5) {
      //   replayer->scheduler.wakeupDebugLog(coreId);
      // }
      (void)replayer->scheduler.nextClock(coreId);
    }

    replayer->outputMtx.lock();
    std::cout << "All threads on Core " << (int)coreId << " completed at " << replayer->scheduler.curClock(coreId) << ".\n" << std::endl;
    replayer->outputMtx.unlock();

    auto ed = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> time = ed - st;
    replayer->outputMtx.lock();
    std::cout << "Core " << (int)coreId << " Running Time: " << time.count() << "ms\n";
    replayer->outputMtx.unlock();
  }

public:
  traceReplayer(std::string eventDir, float CPI_IOPS, float CPI_FLOPS, uint32_t cxtSwitchCycles, 
    uint32_t pthCycles, uint32_t schedSliceCycles, std::vector<CoreInterfaceBase *>& core_data,
    std::vector<CoreInterfaceBase *>& core_inst) 
  : traceReplayerBase(eventDir, core_data, core_inst),
    scheduler(CPI_IOPS, CPI_FLOPS, cxtSwitchCycles, pthCycles, schedSliceCycles)
  {
    threadContexts.resize(NT + 1);
    perThreadLocksHeld.resize(NT + 1);
    joinList.resize(NT + 1);

    kernelContexts.resize(NC);
    replayCore.resize(NC);
    threadCount.resize(NC);
  }

  virtual void init() {
    for (CoreID i = 0; i < NC; i++) {
      // initialize kernel for each core
      ThreadContext *kcxt = new ThreadContext(0, eventDir, NC);
      kcxt->coreId = i;
      kcxt->status = ThreadStatus::COMPLETED;
      kernelContexts[i] = kcxt;
      scheduler.init(kcxt);

      threadCount[i] = 0;
    }

    // initialize each thread
    for (ThreadID i = 1; i <= NT; i++) {
      ThreadContext *tcxt = new ThreadContext(i, eventDir, NC);
      threadContexts[i] = tcxt;
      scheduler.init(tcxt);
      threadCount[tcxt->coreId]++;
    }
    
    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    threadContexts[1]->restSliceCycles = scheduler.schedSliceCycles;
    scheduler.sendReady(1);
  }

  virtual void start() {
    // the main thread
    for (CoreID i = 0; i < NC; i++) {
      // allocate a thread for each core
      replayCore[i] = std::thread(std::bind(&(this->replay), i, this));
    }

    for (auto &c : replayCore) {
      c.join();
    }

    std::cout << "Replay Completed!" << std::endl;

    for (CoreID i = 0; i < NC; i++) {
      std::cout << "Kernel " << (int)i << " reading time: " << kernelContexts[i]->traces.getTime() << "ms" << std::endl;
    }

    for (ThreadID i = 1; i <= NT; i++) {
      std::cout << "Thread " << i << " reading time: " << threadContexts[i]->traces.getTime() << "ms" << std::endl;
    }


    for (CoreID i = 0; i < NC; i++) {
      std::cout << "Kernel " << (int)i << " parsing time: " << kernelContexts[i]->traces.getTime2() << "ms" << std::endl;
    }

    for (ThreadID i = 1; i <= NT; i++) {
      std::cout << "Thread " << i << " parsing time: " << threadContexts[i]->traces.getTime2() << "ms" << std::endl;
    }

    // for (CoreID i = 0; i < NC; i++) {
    //   std::cout << "Kernel " << (int)i << " running time: " << kernelContexts[i]->time.count() << "ms" << std::endl;
    // }

    // for (ThreadID i = 1; i <= NT; i++) {
    //   std::cout << "Thread " << i << " running time: " << threadContexts[i]->time.count() << "ms" << std::endl;
    // }
  }
};

#endif