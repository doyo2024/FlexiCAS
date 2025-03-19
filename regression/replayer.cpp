#include "replayer/parser.hpp"
#include "replayer/replayer.hpp"
#include "util/cache_type.hpp"
#include "util/delay.hpp"
#include "cache/memory.hpp"
#include "cache/metadata.hpp"
#include "cache/policy_multi.hpp"
#include "cache/msi.hpp"
#include "cache/replace_multi.hpp"
#include "cache/cache_multi.hpp"
#include "cache/coherence_multi.hpp"
#include "cache/memory_multi.hpp"

#include <chrono>
#include <iostream>

#define L1IW 4
#define L1WN 4

#define L2IW 5
#define L2WN 8

#define NThread 2
#define NCore 2

int main(){

  auto st = std::chrono::high_resolution_clock::now();

  auto l1d = cache_gen_multi_thread_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRUMultiThread, MSIMultiThreadPolicy, false, false, DelayL1<1, 1, 1>, true>(NCore, "l1d");
  auto core_data = get_l1_core_interface(l1d);
  auto l1i = cache_gen_multi_thread_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRUMultiThread, MSIMultiThreadPolicy, false, true, DelayL1<1, 1, 1>, true>(NCore, "l1i");
  auto core_inst = get_l1_core_interface(l1i);

  auto l2 = cache_gen_multi_thread_l2<L2IW, L2WN, void, MetadataBroadcastBase, ReplaceLRUMultiThread, MSIMultiThreadPolicy, true, DelayCoherentCache<2, 3, 4>, true>(1, "l2")[0];
  auto mem = new SimpleMultiThreadMemoryModel<void, DelayMemory<10>, false>("mem");

  for(int i=0; i<NCore; i++) {
    l1i[i]->outer->connect(l2->inner, l2->inner->connect(l1i[i]->outer, true));
    l1d[i]->outer->connect(l2->inner, l2->inner->connect(l1d[i]->outer));
  }
  l2->outer->connect(mem, mem->connect(l2->outer));

  traceReplayer<NThread, NCore> replayer("/home/spike/Desktop/trace/mutex", 1.0, 2.0, 1, 1, 300000, core_data);
  replayer.init();
  replayer.start();

  auto ed = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> time = ed - st;
  std::cout << "Time: " << time.count() << "ms\n";
}