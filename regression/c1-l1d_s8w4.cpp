#include "cache/cache.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/coherence.hpp"
#include "cache/msi.hpp"
#include "cache/memory.hpp"
#include "util/random.hpp"

#define L1IW 3
#define L1WN 4
#define L1Toff 9

typedef Data64B data_type;
typedef MetadataMSIBroadcast<48, L1IW, L1Toff> l1_metadata_type;
typedef IndexNorm<L1IW,6> l1_indexer_type;
typedef ReplaceLRU<L1IW,L1WN,1> l1_replacer_type;
typedef void l1_delay_type;
typedef CacheNorm<L1IW,L1WN,l1_metadata_type,data_type,l1_indexer_type,l1_replacer_type,void,true> l1_type;
typedef MSIPolicy<l1_metadata_type,1,1> l1_policy_type;

typedef OuterCohPortUncached memory_port_type;
typedef CoherentL1CacheNorm<l1_type,memory_port_type,CoreInterface> l1_cache_type;
typedef SimpleMemoryModel<data_type,void> memory_type;

static uint64_t gi = 703;
CMHasher hasher(gi);

int main() {
  l1_policy_type *l1_policy = new l1_policy_type();
  l1_cache_type * l1d = new l1_cache_type(l1_policy, "l1d");
  CoreInterface * core = static_cast<CoreInterface *>(l1d->inner);
  memory_type * mem = new memory_type("mem");
  l1d->outer->connect(mem, mem->connect(l1d->outer));

  SimpleTracer tracer;
  l1d->attach_monitor(&tracer);

  std::vector<uint32_t> addr_pool(128);
  for(int i=0; i<128; i++) addr_pool[i] = hasher(gi++);

  for(int i=0; i<256; i++) {
    auto addr = addr_pool[hasher(gi++)%128];
    core->read(addr, nullptr);
  }

  return 0;
}
