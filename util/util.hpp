#ifndef CM_UTIL_UTIL_HPP
#define CM_UTIL_UTIL_HPP

#include <cassert>
#define get_thread_id std::hash<std::thread::id>{}(std::this_thread::get_id())

#include <condition_variable>
#include <vector>
#include <mutex>
#include <unordered_map>

typedef struct{
  uint32_t ai    ;
  uint32_t s     ;
  uint32_t w     ;
  std::mutex* mtx;
  std::mutex* cmtx; // cacheline mutex
  std::condition_variable* cv;
  std::vector<uint32_t>* status;
}addr_info;

/////////////////////////////////
// Record inner probing address
class InnerAcquireRecord
{
protected:
  std::mutex  mtx;           // mutex for protecting record
  std::unordered_map<uint64_t, addr_info> map;
public:
  void add(uint64_t addr, addr_info loc){
    std::unique_lock lk(mtx, std::defer_lock);
    lk.lock();
    map[addr] = loc;
    lk.unlock();
  }
  void erase(uint64_t addr){
    std::unique_lock lk(mtx, std::defer_lock);
    lk.lock();
    map.erase(addr);
    lk.unlock();
  }
  std::pair<bool, addr_info> query(uint64_t addr){
    std::unique_lock lk(mtx, std::defer_lock);
    addr_info info;
    bool count;
    lk.lock();
    if(map.count(addr)){
      count = true;
      info = map[addr];
    }else{
      count = false;
    }
    lk.unlock();
    return std::make_pair(count, info);
  }
};

#endif