#include "replayer/replayer.hpp"
#include "util/cache_type.hpp"
#include "util/delay.hpp"
#include "util/regression.hpp"
#include "util/monitor.hpp"
#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "cache/msi.hpp"
#include "cache/replace.hpp"
#include "cache/cache.hpp"
#include "cache/coherence.hpp"
#include <chrono>
#include <iostream>

// #define L1IW 4
// #define L1WN 4

// #define L2IW 5
// #define L2WN 8

// 32K 8W, both I and D
#define L1IW 6
#define L1WN 8

// 256K, 4W, exclusive 
#define L2IW 10
#define L2WN 4

// 2M per core
#define L3IW (11+1)
#define L3WN 16

#define NThread 1
#define NCore 1

std::string traceDir = "/home/yzx/yzx/trace/spec/502";
std::string resultDir = "/home/yzx/yzx/results/spec/502";

int main(){

  auto st = std::chrono::high_resolution_clock::now();

  using policy_l3 = MESIPolicy<false, true, policy_memory>;
  using policy_l2 = ExclusiveMSIPolicy<false, false, policy_l3, false>;
  using policy_l1d = MSIPolicy<true, false, policy_l2>;
  using policy_l1i = MSIPolicy<true, true, policy_l2>;

  auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1d, false, DelayL1<1, 1, 1>, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l1i, true, DelayL1<1, 1, 1>, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);
  // auto l2 = cache_gen_inc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceLRU, MSIPolicy, policy_l2, true, DelayCoherentCache<2, 3, 4>, true>(1, "l2")[0];
  auto l2 = cache_gen_exc<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceSRRIP, ExclusiveMSIPolicy, policy_l2, false, DelayCoherentCache<2, 3, 4>, true>(NCore, "l2");
  auto l3 = cache_gen_inc<L3IW, L3WN, void, MetadataDirectoryBase, ReplaceSRRIP, MESIPolicy, policy_l3, true, DelayCoherentCache<3, 4, 6>, true>(NCore, "l3");
  auto dispatcher = new SliceDispatcher<SliceHashNorm<> >("disp", NCore);
  auto mem = new SimpleMemoryModel<void, DelayMemory<10>, false>("mem");
  // MissMonitor tracer(resultDir, false);
  std::vector<MissMonitor *> tracers;

  for(int i = 0; i < NCore; i++) {
    // l1i[i]->outer->connect(l2->inner);
    // l1d[i]->outer->connect(l2->inner);
    l1i[i]->outer->connect(l2[i]->inner);
    l1d[i]->outer->connect(l2[i]->inner);
    dispatcher->connect(l3[i]->inner);
    l2[i]->outer->connect_by_dispatch(dispatcher, l3[0]->inner);
    if constexpr (!policy_l2::is_uncached()) // normally this check is useless as L2 is cached, but provied as an example
      for(int j=1; j<NCore; j++) l3[j]->inner->connect(l2[i]->outer);
    l3[i]->outer->connect(mem);
    // l1i[i]->attach_monitor(&tracer);
    // l1d[i]->attach_monitor(&tracer);
    // l2[i]->attach_monitor(&tracer);
    // l3[i]->attach_monitor(&tracer);

    MissMonitor *l1im = new MissMonitor(resultDir, "l1i", i);
    l1i[i]->attach_monitor(l1im);
    tracers.push_back(l1im);

    MissMonitor *l1dm = new MissMonitor(resultDir, "l1d", i);
    l1d[i]->attach_monitor(l1dm);
    tracers.push_back(l1dm);

    MissMonitor *l2m = new MissMonitor(resultDir, "l2", i);
    l2[i]->attach_monitor(l2m);
    tracers.push_back(l2m);

    MissMonitor *l3m = new MissMonitor(resultDir, "l3", i);
    l3[i]->attach_monitor(l3m);
    tracers.push_back(l3m);
  }

  MissMonitor *memm = new MissMonitor(resultDir, "mem", 0);
  mem->attach_monitor(memm);
  tracers.push_back(memm);

  for (auto it : tracers)
    it->start();

  // l2->outer->connect(mem);
  // l2->attach_monitor(&tracer);
  // tracer.start();

  puts("cache ok");

  traceReplayer<NThread, NCore> replayer(traceDir, 1.0, 2.0, 1, 1, 300000, core_data, core_inst, tracers);
  replayer.init();
  puts("Start to Replay:");
  replayer.start();

  // tracer.stop();
  for (auto it : tracers) {
    it->stop();
    delete it;
  }

  delete_caches(l1d);
  delete_caches(l1i);
  delete_caches(l2);
  delete_caches(l3);
  // delete l2;
  delete mem;

  auto ed = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> time = ed - st;
  std::cout << "Time: " << time.count() << "ms\n";
}