#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "util/random.hpp"
#include "util/monitor.hpp"
#include "util/concept_macro.hpp"
#include "util/query.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/metadata.hpp"

//#include <iostream>

// base class for a cache array:
class CacheArrayBase
{
protected:
  const std::string name;               // an optional name to describe this cache

public:
  CacheArrayBase(std::string name = "") : name(name) {}
  virtual ~CacheArrayBase() {}

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;
  virtual CMMetadataCommon * get_meta(uint32_t s, uint32_t w) = 0;
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) = 0;
};

// normal set associative cache array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
template<int IW, int NW, typename MT, typename DT>
  requires C_DERIVE(MT, CMMetadataCommon) && C_DERIVE_OR_VOID(DT, CMDataBase)
class CacheArrayNorm : public CacheArrayBase
{
protected:
  std::vector<MT *> meta;   // meta array
  std::vector<DT *> data;   // data array, could be null
  const unsigned int way_num;

public:
  static constexpr uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(unsigned int extra_way = 0, std::string name = "") : CacheArrayBase(name), way_num(NW+extra_way){
    size_t meta_num = nset * way_num;
    constexpr size_t data_num = nset * NW;

    meta.resize(meta_num);
    for(auto &m:meta) m = new MT();
    if(extra_way)
      for(unsigned int s=0; s<nset; s++)
        for(unsigned int w=NW; w<way_num; w++)
          meta[s*way_num+w]->to_extend();

    if constexpr (!C_VOID(DT)) {
      data.resize(data_num);
      for(auto &d:data) d = new DT();
    }
  }

  virtual ~CacheArrayNorm() {
    for(auto m:meta) delete m;
    if constexpr (!C_VOID(DT)) for(auto d:data) delete d;
  }

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const {
    for(unsigned int i=0; i<way_num; i++)
      if(meta[s*way_num + i]->match(addr)) {
        *w = i;
        return true;
      }
    return false;
  }

  virtual CMMetadataCommon * get_meta(uint32_t s, uint32_t w) { return meta[s*way_num + w]; }
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) {
    if constexpr (C_VOID(DT)) return nullptr;
    else                      return data[s*NW + w];
  }
};

//////////////// define cache ////////////////////

// base class for a cache
class CacheBase : public CacheMonitorSupport
{
protected:
  const uint32_t id;                    // a unique id to identify this cache
  const std::string name;               // an optional name to describe this cache

  // a vector of cache arrays
  // set-associative: one CacheArrayNorm objects
  // with VC: two CacheArrayNorm objects (one fully associative)
  // skewed: partition number of CacheArrayNorm objects (each as a single cache array)
  // MIRAGE: parition number of CacheArrayNorm (meta only) with one separate CacheArrayNorm for storing data (in derived class)
  std::vector<CacheArrayBase *> arrays;

public:
  CacheBase(std::string name) : id(UniqueID::new_id(name)), name(name) {}

  virtual ~CacheBase() {
    for(auto a: arrays) delete a;
  }

  virtual bool hit(uint64_t addr,
                   uint32_t *ai,  // index of the hitting cache array in "arrays"
                   uint32_t *s, uint32_t *w
                   ) = 0;

  bool hit(uint64_t addr) {
    uint32_t ai, s, w;
    return hit(addr, &ai, &s, &w);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) = 0;

  virtual CMMetadataCommon *access(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_meta(s, w);
  }

  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) {
    return arrays[ai]->get_data(s, w);
  }

  virtual CMDataBase *data_copy_buffer() = 0;               // allocate a copy buffer, needed by exclusive cache with extended meta
  virtual void data_return_buffer(CMDataBase *buf) = 0;     // return a copy buffer, used to detect conflicts in copy buffer
  virtual CMMetadataBase *meta_copy_buffer() = 0;           // allocate a copy buffer, needed by exclusive cache with extended meta
  virtual void meta_return_buffer(CMMetadataBase *buf) = 0; // return a copy buffer, used to detect conflicts in copy buffer

  virtual std::tuple<int, int, int> size() const = 0;           // return the size parameters of the cache
  uint32_t get_id() const { return id; }
  const std::string& get_name() const { return name;} 

  // access both meta and data in one function call
  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) = 0;

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB) = 0;
  virtual LocInfo query_loc(uint64_t addr) { return LocInfo(id, this, addr); }
  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) = 0;
};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
  requires C_DERIVE(MT, CMMetadataBase) && C_DERIVE_OR_VOID(DT, CMDataBase) &&
           C_DERIVE(IDX, IndexFuncBase) && C_DERIVE(RPC, ReplaceFuncBase) && C_DERIVE_OR_VOID(DLY, DelayBase)
class CacheSkewed : public CacheBase
{
protected:
  IDX indexer;      // index resolver
  RPC replacer[P];  // replacer
  RandomGen<uint32_t> * loc_random; // a local randomizer for better thread parallelism
  std::unordered_set<CMDataBase *>       data_buffer_pool;
  std::unordered_map<CMDataBase *, bool> data_buffer_state;
  std::unordered_set<CMMetadataBase *>       meta_buffer_pool;
  std::unordered_map<CMMetadataBase *, bool> meta_buffer_state;

public:
  CacheSkewed(std::string name = "", unsigned int extra_par = 0, unsigned int extra_way = 0)
    : CacheBase(name), loc_random(nullptr)
  {
    arrays.resize(P+extra_par);
    for(int i=0; i<P; i++) arrays[i] = new CacheArrayNorm<IW,NW,MT,DT>(extra_way);
    CacheMonitorSupport::monitors = new CacheMonitorImp<DLY, EnMon>(CacheBase::id);

    if constexpr (P>1) loc_random = cm_alloc_rand32();

    // for single thread simulator, we assume a maximum of 2 buffers should be enough
    if constexpr (!C_VOID(DT)) {
      for(int i=0; i<2; i++) {
        auto buffer = new DT();
        data_buffer_pool.insert(buffer);
        data_buffer_state[buffer] = true;
      }
    }
    for(int i=0; i<2; i++) {
      auto buffer = new MT();
      meta_buffer_pool.insert(buffer);
      meta_buffer_state[buffer] = true;
    }
  }

  virtual ~CacheSkewed() {
    delete CacheMonitorSupport::monitors;
    if constexpr (!C_VOID(DT)) for(auto &buf: data_buffer_state) delete buf.first;
    for(auto &buf: meta_buffer_state) delete buf.first;
    if constexpr (P>1) delete loc_random;
  }

  virtual std::tuple<int, int, int> size() const { return std::make_tuple(P, 1ul<<IW, NW); }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      if(CacheBase::arrays[*ai]->hit(addr, *s, w))
        return true;
    }
    return false;
  }

  virtual std::pair<CMMetadataBase *, CMDataBase *> access_line(uint32_t ai, uint32_t s, uint32_t w) {
    auto meta = static_cast<CMMetadataBase *>(arrays[ai]->get_meta(s, w));
    if constexpr (!C_VOID(DT))
      return std::make_pair(meta, w < NW ? arrays[ai]->get_data(s, w) : nullptr);
    else
      return std::make_pair(meta, nullptr);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, unsigned int genre = 0) {
    if constexpr (P==1) *ai = 0;
    else                *ai = ((*loc_random)() % P);
    *s = indexer.index(addr, *ai);
    replacer[*ai].replace(*s, w);
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) replacer[ai].access(s, w, false);
    if constexpr (EnMon || !C_VOID(DLY)) monitors->hook_read(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool is_release, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P) replacer[ai].access(s, w, is_release);
    if constexpr (EnMon || !C_VOID(DLY)) monitors->hook_write(addr, ai, s, w, hit, meta, data, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, const CMMetadataBase * meta, const CMDataBase *data, uint64_t *delay) {
    if(ai < P && hit && evict) replacer[ai].invalid(s, w);
    if constexpr (EnMon || !C_VOID(DLY)) monitors->hook_manage(addr, ai, s, w, hit, evict, writeback, meta, data, delay);
  }

  virtual CMDataBase *data_copy_buffer() {
    if constexpr (C_VOID(DT)) return nullptr;
    else {
      assert(!data_buffer_pool.empty());
      auto buffer = *(data_buffer_pool.begin());
      assert(data_buffer_state[buffer]);
      data_buffer_pool.erase(buffer);
      data_buffer_state[buffer] = false;
      return buffer;
    }
  }

  virtual void data_return_buffer(CMDataBase *buf) {
    if(data_buffer_state.count(buf)) {
      assert(!data_buffer_state[buf]);
      data_buffer_state[buf] = true;
      data_buffer_pool.insert(buf);
    }
  }

  virtual CMMetadataBase *meta_copy_buffer() {
    assert(!meta_buffer_pool.empty());
    auto buffer = *(meta_buffer_pool.begin());
    assert(meta_buffer_state[buffer]);
    meta_buffer_pool.erase(buffer);
    meta_buffer_state[buffer] = false;
    //std::cout << std::hex << name << " alloc meta buffer 0x" << buffer << std::endl;
    return buffer;
  }

  virtual void meta_return_buffer(CMMetadataBase *buf) {
    if(meta_buffer_state.count(buf)) {
      assert(!meta_buffer_state[buf]);
      meta_buffer_state[buf] = true;
      meta_buffer_pool.insert(buf);
      //std::cout << std::hex << name << " return meta buffer 0x" << buf << std::endl;
    }
  }

  virtual bool query_coloc(uint64_t addrA, uint64_t addrB){
    for(int i=0; i<P; i++) 
      if(indexer.index(addrA, i) == indexer.index(addrB, i)) 
        return true;
    return false;
  }

  virtual void query_fill_loc(LocInfo *loc, uint64_t addr) {
    for(int i=0; i<P; i++){
      loc->insert(LocIdx(i, indexer.index(addr, i)), LocRange(0, NW-1));
    }
  }
};

// Normal set-associative cache
// IW: index width, NW: number of ways
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon>;

#endif
