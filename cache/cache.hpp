#ifndef CM_CACHE_CACHE_HPP
#define CM_CACHE_CACHE_HPP

#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <set>
#include <map>
#include <vector>

#include "util/random.hpp"
#include "util/monitor.hpp"
#include "cache/index.hpp"
#include "cache/replace.hpp"
#include "cache/delay.hpp"

class CMMetadataBase
{
public:
  // implement a totally useless base class
  virtual bool match(uint64_t addr) const { return false; } // wether an address match with this block
  virtual void reset() {}                                   // reset the metadata
  virtual void init(uint64_t addr) = 0;                     // initialize the meta for addr
  virtual uint64_t addr(uint32_t s) const = 0;              // assemble the block address from the metadata

  virtual void to_invalid() {}       // change state to invalid
  virtual void to_shared() {}        // change to shared
  virtual void to_modified() {}      // change to modified
  virtual void to_owned() {}         // change to owned
  virtual void to_exclusive() {}     // change to exclusive
  virtual void to_dirty() {}         // change to dirty
  virtual void to_clean() {}         // change to dirty
  virtual bool is_valid() const { return false; }
  virtual bool is_shared() const { return false; }
  virtual bool is_modified() const { return false; }
  virtual bool is_owned() const { return false; }
  virtual bool is_exclusive() const { return false; }
  virtual bool is_dirty() const { return false; }

  virtual ~CMMetadataBase() {}
};

class CMDataBase
{
public:
  // implement a totally useless base class
  virtual void reset() {} // reset the data block, normally unnecessary
  virtual uint64_t read(unsigned int index) const { return 0; } // read a 64b data
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) {} // write a 64b data with wmask
  virtual void write(uint64_t *wdata) {} // write the whole cache block
  virtual void copy(const CMDataBase *block) {} // copy the content of block

  virtual ~CMDataBase() {}
};

// typical 64B data block
class Data64B : public CMDataBase
{
protected:
  uint64_t data[8];

public:
  Data64B() : data{0} {}
  virtual ~Data64B() {}

  virtual void reset() { for(auto d:data) d = 0; }
  virtual uint64_t read(unsigned int index) const { return data[index]; }
  virtual void write(unsigned int index, uint64_t wdata, uint64_t wmask) { data[index] = (data[index] & (~wmask)) | (wdata & wmask); }
  virtual void write(uint64_t *wdata) { for(int i=0; i<8; i++) data[i] = wdata[i]; }
  virtual void copy(const CMDataBase *m_block) {
    auto block = static_cast<const Data64B *>(m_block);
    for(int i=0; i<8; i++) data[i] = block->data[i];
  }
};

//////////////// define cache array ////////////////////

// base class for a cache array:
class CacheArrayBase
{
protected:
  const std::string name;               // an optional name to describe this cache

public:
  CacheArrayBase(std::string name = "") : name(name) {}
  virtual ~CacheArrayBase() {}

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const = 0;
  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) = 0;
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) = 0;
};

// normal set associative cache array
// IW: index width, NW: number of ways, MT: metadata type, DT: data type (void if not in use)
template<int IW, int NW, typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type, // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class CacheArrayNorm : public CacheArrayBase
{
protected:
  std::vector<MT *> meta;   // meta array
  std::vector<DT *> data;   // data array, could be null

public:
  static constexpr uint32_t nset = 1ul<<IW;  // number of sets

  CacheArrayNorm(std::string name = "") : CacheArrayBase(name) {
    constexpr size_t num = nset * NW;
    meta.resize(num);
    for(auto &m:meta) m = new MT();
    if constexpr (!std::is_void<DT>::value) {
      data.resize(num);
      for(auto &d:data) d = new DT();
    }
  }

  virtual ~CacheArrayNorm() {
    for(auto m:meta) delete m;
    if constexpr (!std::is_void<DT>::value) for(auto d:data) delete d;
  }

  virtual bool hit(uint64_t addr, uint32_t s, uint32_t *w) const {
    for(int i=0; i<NW; i++)
      if(meta[s*NW + i]->match(addr)) {
        *w = i;
        return true;
      }

    return false;
  }

  virtual CMMetadataBase * get_meta(uint32_t s, uint32_t w) { return meta[s*NW + w]; }
  virtual CMDataBase * get_data(uint32_t s, uint32_t w) {
    if constexpr (std::is_void<DT>::value) {
      return nullptr;
    } else
      return data[s*NW + w];
  }
};

//////////////// define cache ////////////////////

// base class for a cache
class CacheBase
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

  // monitor related
  std::set<MonitorBase *> monitors;

public:
  CacheBase(std::string name) : id(UniqueID::new_id()), name(name) {}

  virtual ~CacheBase() { for(auto a: arrays) delete a; }

  virtual bool hit(uint64_t addr,
                   uint32_t *ai,  // index of the hitting cache array in "arrays"
                   uint32_t *s, uint32_t *w
                   ) = 0;

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) = 0;

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint32_t* v_ai, uint32_t *v_s, uint32_t* v_w) {} // only useful for CacheSkewed(Norm)WithVC

  // hook interface for replacer state update, Monitor and delay estimation
  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) = 0;
  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) = 0;
  // probe, invalidate and writeback
  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) = 0;

  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w) = 0;
  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w) = 0;

  virtual uint32_t get_Partition_size() { return 0; }

  // monitor related
  bool attach_monitor(MonitorBase *m) {
    if(m->attach(id)) {
      monitors.insert(m);
      return true;
    } else
      return false;
  }

  // support run-time assign/reassign mointors
  void detach_monitor() { monitors.clear(); }
};

// Skewed Cache
// IW: index width, NW: number of ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int P, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type,  // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, IDX>::value>::type,  // IDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, RPC>::value>::type,  // RPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
class CacheSkewed : public CacheBase
{
protected:
  IDX indexer;     // index resolver
  RPC replacer[P]; // replacer
  DLY *timer;      // delay estimator

public:
  CacheSkewed(std::string name = "")
    : CacheBase(name)
  {
    arrays.resize(P);
    for(auto &a:arrays) a = new CacheArrayNorm<IW,NW,MT,DT>();
    if constexpr (!std::is_void<DLY>::value) timer = new DLY();
  }

  virtual ~CacheSkewed() {
    if constexpr (!std::is_void<DLY>::value) delete timer;
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      for(*w=0; *w<NW; (*w)++)
        if(access(*ai, *s, *w)->match(addr)) return true;
    }
    return false;
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) {
    if constexpr (P==1) *ai = 0;
    else                *ai = (cm_get_random_uint32() % P);
    *s = indexer.index(addr, *ai);
    replacer[*ai].replace(*s, w);
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    replacer[ai].access(s, w);
    if constexpr (EnMon) for(auto m:this->monitors) m->read(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) timer->read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    replacer[ai].access(s, w);
    if constexpr (EnMon) for(auto m:this->monitors) m->write(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) timer->write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) {
    if(hit && evict) {
      replacer[ai].invalid(s, w);
      if constexpr (EnMon) for(auto m:this->monitors) m->invalid(addr, ai, s, w);
    }
    if constexpr (!std::is_void<DLY>::value) timer->manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w){
    return arrays[ai]->get_meta(s, w);
  }
  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w){
    return arrays[ai]->get_data(s, w);
  }

  virtual uint32_t get_Partition_size() { return P; }
};

// Normal set-associative cache
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY, bool EnMon>
using CacheNorm = CacheSkewed<IW, NW, 1, MT, DT, IDX, RPC, DLY, EnMon>;

// IW: index width, NW: number of ways, VW: number of victim cache ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int VW, int P, typename MT, typename DT, typename IDX, typename RPC, typename VPRC, typename DLY, bool EnMon,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type,  // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, IDX>::value>::type,  // IDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, RPC>::value>::type,  // RPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
class CacheSkewedWithVC : public CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon> 
{
protected:
  VPRC v_replacer;
public:
  CacheSkewedWithVC(std::string name = "")
    :  CacheSkewed<IW, NW, P, MT, DT, IDX, RPC, DLY, EnMon>(name)
  {
    this->arrays.resize(P+1);
    this->arrays[P] = new CacheArrayNorm<0, VW, MT, DT>;
  }

  virtual ~CacheSkewedWithVC() {}

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = this->indexer.index(addr, *ai);
      for(*w=0; *w<NW; (*w)++)
        if(this->access(*ai, *s, *w)->match(addr)) return true;
    }
    *ai = P; *s = 0;
    for(*w=0; *w<VW; (*w)++)
      if(this->access(*ai, *s, *w)->match(addr)) return true;
    return false;
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    if(ai == P) v_replacer.access(s, w);
    else        this->replacer[ai].access(s, w);    
    if constexpr (EnMon) for(auto m:this->monitors) m->read(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) this->timer->read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    if(ai == P) v_replacer.access(s, w);
    else        this->replacer[ai].access(s, w);  
    if constexpr (EnMon) for(auto m:this->monitors) m->write(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) this->timer->write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) {
    if(hit && evict) {
      if(ai == P) v_replacer.invalid(s, w);
      else        this->replacer[ai].invalid(s, w);  
      if constexpr (EnMon) for(auto m:this->monitors) m->invalid(addr, ai, s, w);
    }
    if constexpr (!std::is_void<DLY>::value) this->timer->manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint32_t* v_ai, uint32_t *v_s, uint32_t* v_w){
    if constexpr (P==1) *ai = 0;
    else                *ai = (cm_get_random_uint32() % P);
    *s = this->indexer.index(addr, *ai);
    this->replacer[*ai].replace(*s, w);

    *v_ai = P; *v_s = 0;
    v_replacer.replace(*v_s, v_w);
  }
};

// IW: index width, NW: number of ways, VW: number of victim cache ways, P: number of partitions
// MT: metadata type, DT: data type (void if not in use)
// IDX: indexer type, RPC: replacer type
// EnMon: whether to enable monitoring
template<int IW, int NW, int VW, typename MT, typename DT, typename IDX, typename RPC, typename VPRC, typename DLY, bool EnMon>
using CacheNormWithVC = CacheSkewedWithVC<IW, NW, VW, 1, MT, DT, IDX, RPC, VPRC, DLY, EnMon>;

#endif
