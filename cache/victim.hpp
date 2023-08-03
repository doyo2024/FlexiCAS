#ifndef CM_CACHE_VICTIM_HPP
#define CM_CACHE_VICTIM_HPP

#include "cache/msi.hpp"
#include <iostream>

class CacheVCBase : public CacheBase
{
public:
  CacheVCBase(std::string name = "") : CacheBase(name){}

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) = 0;
  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint32_t* v_ai, uint32_t *v_s, uint32_t* v_w) = 0;

  virtual bool check_victim_entry(uint32_t ai, uint32_t s, uint32_t w) = 0;
};

template<int IW, int NW, int VW, int P, typename MT, typename DT, typename IDX, typename RPC, typename VRPC, typename DLY, bool EnMon,
         typename = typename std::enable_if<std::is_base_of<CMMetadataBase, MT>::value>::type,  // MT <- CMMetadataBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type, // DT <- CMDataBase or void
         typename = typename std::enable_if<std::is_base_of<IndexFuncBase, IDX>::value>::type,  // IDX <- IndexFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, RPC>::value>::type,  // RPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<ReplaceFuncBase, VRPC>::value>::type,  // VRPC <- ReplaceFuncBase
         typename = typename std::enable_if<std::is_base_of<DelayBase, DLY>::value || std::is_void<DLY>::value>::type>  // DLY <- DelayBase or void
class CacheSkewedWithVC : public CacheVCBase
{
protected:
  IDX indexer;     // index resolver
  RPC replacer[P]; // replacer
  DLY *timer;      // delay estimator
  VRPC v_replacer; // victim replacer
public:
  CacheSkewedWithVC(std::string name = "")
    :  CacheVCBase(name)
  {
    arrays.resize(P+1);
    for(int i = 0; i < P; i++) arrays[i] = new CacheArrayNorm<IW, NW, MT, DT>;
    arrays[P] = new CacheArrayNorm<0, VW, MT, DT>;
    if constexpr (!std::is_void<DLY>::value) timer = new DLY();
  }

  virtual ~CacheSkewedWithVC() {
    if constexpr (!std::is_void<DLY>::value) delete timer;
  }

  virtual bool check_victim_entry(uint32_t ai, uint32_t s, uint32_t w){
    return (ai == P) && (s == 0) && (w < VW);
  }

  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w ) {
    for(*ai=0; *ai<P; (*ai)++) {
      *s = indexer.index(addr, *ai);
      for(*w=0; *w<NW; (*w)++)
        if(access(*ai, *s, *w)->match(addr)) return true;
    }
    *ai = P; *s = 0;
    for(*w=0; *w<VW; (*w)++)
      if(access(*ai, *s, *w)->match(addr)) return true;
    return false;
  }

  virtual void hook_read(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    if(ai == P) v_replacer.access(s, w);
    else        replacer[ai].access(s, w);    
    if constexpr (EnMon) for(auto m:monitors) m->read(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) timer->read(addr, ai, s, w, hit, delay);
  }

  virtual void hook_write(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, uint64_t *delay) {
    if(ai == P) v_replacer.access(s, w);
    else        replacer[ai].access(s, w);  
    if constexpr (EnMon) for(auto m:monitors) m->write(addr, ai, s, w, hit);
    if constexpr (!std::is_void<DLY>::value) timer->write(addr, ai, s, w, hit, delay);
  }

  virtual void hook_manage(uint64_t addr, uint32_t ai, uint32_t s, uint32_t w, bool hit, bool evict, bool writeback, uint64_t *delay) {
    if(hit && evict) {
      if(ai == P) v_replacer.invalid(s, w);
      else        replacer[ai].invalid(s, w);  
      if constexpr (EnMon) for(auto m:monitors) m->invalid(addr, ai, s, w);
    }
    if constexpr (!std::is_void<DLY>::value) timer->manage(addr, ai, s, w, hit, evict, writeback, delay);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w) {
    if constexpr (P==1) *ai = 0;
    else                *ai = (cm_get_random_uint32() % P);
    *s = indexer.index(addr, *ai);
    replacer[*ai].replace(*s, w);
  }

  virtual void replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, uint32_t* v_ai, uint32_t *v_s, uint32_t* v_w){
    if constexpr (P==1) *ai = 0;
    else                *ai = (cm_get_random_uint32() % P);
    *s = indexer.index(addr, *ai);
    replacer[*ai].replace(*s, w);

    *v_ai = P; *v_s = 0;
    v_replacer.replace(*v_s, v_w);
  }

  virtual CMMetadataBase *access(uint32_t ai, uint32_t s, uint32_t w){
    return arrays[ai]->get_meta(s, w);
  }
  virtual CMDataBase *get_data(uint32_t ai, uint32_t s, uint32_t w){
    return arrays[ai]->get_data(s, w);
  }

};

template<int IW, int NW, int VW, typename MT, typename DT, typename IDX, typename RPC, typename VRPC, typename DLY, bool EnMon>
using CacheNormWithVC = CacheSkewedWithVC<IW, NW, 1, VW, MT, DT, IDX, RPC, VRPC, DLY,EnMon>;

template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
using OuterPortMSIUncachedWithVC = OuterPortMSIUncached<MT, DT>;
template<typename MT, typename DT,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
using OuterPortMSIWithVC = OuterPortMSI<MT, DT>;


template<typename MT, typename DT, bool isLLC,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class InnerPortMSIUncachedWithVC : public InnerPortMSIUncached<MT, DT, isLLC>
{
public:
  virtual void acquire_resp(uint64_t addr, CMDataBase *data_inner, uint32_t cmd, uint64_t *delay){
    uint32_t ai, s, w;
    uint32_t v_ai, v_s, v_w;
    CMMetadataBase *meta, *mmeta;
    CMDataBase *data, *ddata;
    uint64_t addrr;
    bool hit, writeback;
    CacheVCBase* cache = static_cast<CacheVCBase *>(this->cache);
    if(hit = cache->hit(addr, &ai, &s, &w)) { // hit
      meta = cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = cache->get_data(ai, s, w);
      if(cache->check_victim_entry(ai, s, w)){ // If addr hits in the victim cache, it needs to be swapped into the cache
        DT copy_data;
        bool shared, dirty;
        cache->replace(addr, &v_ai, &v_s, &v_w);
        mmeta = cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = cache->get_data(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) copy_data.copy(data);
        if(meta->is_dirty()) dirty = true; if(meta->is_shared()) shared = true;
        cache->hook_manage(addr, ai, s, w, true, true, false, delay);
        if(mmeta->is_valid()){
          addrr = mmeta->addr(v_s);
          Policy::meta_copy_state(meta, mmeta, addrr);
          if constexpr (!std::is_void<DT>::value) data->copy(ddata);
          cache->hook_manage(addrr, v_ai, v_s, v_w, true, true, false, delay);
          cache->hook_read(addrr, ai, s, w, false, delay);
        } 
        mmeta->init(addr);
        if(dirty) mmeta->to_dirty(); else mmeta->to_clean();
        if(shared) mmeta->to_shared(); else mmeta->to_modified();
        if constexpr (!std::is_void<DT>::value) ddata->copy(&copy_data); 

        meta = mmeta; ddata = data; ai = v_ai; s = v_s; w = v_w;               
      }
      if(Policy::need_sync(cmd, meta)) this->probe_req(addr, meta, data, Policy::cmd_for_sync(cmd), delay); // sync if necessary
      if constexpr (!isLLC) {
        if(Policy::need_promote(cmd, meta)) {  // promote permission if needed
          this->outer->acquire_req(addr, meta, data, cmd, delay);
          hit = false;
        }
      }
    } else{ // miss
      // get the way to be replaced
      cache->replace(addr, &ai, &s, &w, &v_ai, &v_s, &v_w);
      meta = cache->access(ai, s, w); 
      if constexpr (!std::is_void<DT>::value) data = cache->get_data(ai, s, w);
      if(meta->is_valid()){
        addrr = meta->addr(s);
        mmeta = cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = cache->get_data(v_ai, v_s, v_w);
        if(mmeta->is_valid()) {
            auto replace_addr = mmeta->addr(s);
            if(Policy::need_sync(Policy::cmd_for_evict(), mmeta)) this->probe_req(replace_addr, mmeta, ddata, Policy::cmd_for_sync(Policy::cmd_for_evict()), delay); // sync if necessary
            if(writeback = mmeta->is_dirty()) this->outer->writeback_req(replace_addr, mmeta, ddata, Policy::cmd_for_evict(), delay); // writeback if dirty
            cache->hook_manage(replace_addr, v_ai, v_s, v_w, true, true, writeback, delay);
        }
        Policy::meta_copy_state(mmeta, meta, addrr); meta->to_invalid(); meta->to_clean();
        ddata->copy(data);
        cache->hook_manage(addrr, ai, s, w, true, true, false, delay);
        cache->hook_read(addrr, v_ai, v_s, v_w, false, delay);
      }
      this->outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    if constexpr (!std::is_void<DT>::value) data_inner->copy(cache->get_data(ai, s, w));
    Policy::meta_after_acquire(cmd, meta);
    cache->hook_read(addr, ai, s, w, hit, delay);
  }
};

// full MSI inner port (broadcasting hub, snoop)
template<typename MT, typename DT, bool isLLC>
class InnerPortMSIBroadcastWithVC : public InnerPortMSIUncachedWithVC<MT, DT, isLLC>
{
public:
  virtual void probe_req(uint64_t addr, CMMetadataBase *meta, CMDataBase *data, uint32_t cmd, uint64_t *delay) {
    for(uint32_t i=0; i<this->coh.size(); i++)
      if(Policy::need_probe(cmd, i))
        this->coh[i]->probe_resp(addr, meta, data, cmd, delay);
  }
};

template<typename MT, typename DT, bool EnableDelay, bool isLLC,
         typename = typename std::enable_if<std::is_base_of<MetadataMSIBase, MT>::value>::type, // MT <- MetadataMSIBase
         typename = typename std::enable_if<std::is_base_of<CMDataBase, DT>::value || std::is_void<DT>::value>::type> // DT <- CMDataBase or void
class CoreInterfaceMSIWithVC : public CoreInterfaceMSI<MT, DT, EnableDelay, isLLC>
{
  inline CMDataBase *access(uint64_t addr, uint32_t cmd, uint64_t *delay) {
    uint32_t ai, s, w;
    uint32_t v_ai, v_s, v_w;
    CMMetadataBase *meta, *mmeta;
    CMDataBase *data, *ddata;
    uint64_t addrr;
    bool hit, writeback;
    CacheVCBase* cache = static_cast<CacheVCBase *>(this->cache);
    if(hit = cache->hit(addr, &ai, &s, &w)) { // hit
      meta = cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = cache->get_data(ai, s, w);
      if(cache->check_victim_entry(ai, s, w)){ // If addr hits in the victim cache, it needs to be swapped into the cache
        DT copy_data;
        bool shared, dirty;
        cache->replace(addr, &v_ai, &v_s, &v_w);
        mmeta = cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = cache->get_data(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) copy_data.copy(data);
        if(meta->is_dirty()) dirty = true; if(meta->is_shared()) shared = true;
        cache->hook_manage(addr, ai, s, w, true, true, false, delay);
        if(mmeta->is_valid()){
          addrr = mmeta->addr(v_s);
          Policy::meta_copy_state(meta, mmeta, addrr);
          if constexpr (!std::is_void<DT>::value) data->copy(ddata);
          cache->hook_manage(addrr, v_ai, v_s, v_w, true, true, false, delay);
          cache->hook_read(addrr, ai, s, w, false, delay);
        } 
        mmeta->init(addr);
        if(dirty) mmeta->to_dirty(); else mmeta->to_clean();
        if(shared) mmeta->to_shared(); else mmeta->to_modified();
        if constexpr (!std::is_void<DT>::value) ddata->copy(copy_data); 

        meta = mmeta; ddata = data; ai = v_ai; s = v_s; w = v_w;               
      }
      if constexpr (!isLLC) {
        if(Policy::need_promote(cmd, meta)) {
          this->outer->acquire_req(addr, meta, data, cmd, delay);
          hit = false;
        }
      }
    } else { // miss
      // get the way to be replaced
      cache->replace(addr, &ai, &s, &w, &v_ai, &v_s, &v_w);
      meta = cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = cache->get_data(ai, s, w);
      if(meta->is_valid()){
        addrr = meta->addr(s);
        mmeta = cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = cache->get_data(v_ai, v_s, v_w);
        if(mmeta->is_valid()) {
            auto replace_addr = mmeta->addr(s);
            if(writeback = mmeta->is_dirty()) this->outer->writeback_req(replace_addr, mmeta, ddata, Policy::cmd_for_evict(), delay); // writeback if dirty
            cache->hook_manage(replace_addr, v_ai, v_s, v_w, true, true, writeback, delay);
        }
        Policy::meta_copy_state(mmeta, meta, addrr); meta->to_invalid(); meta->to_clean();
        ddata->copy(data);
        cache->hook_manage(addrr, ai, s, w, true, true, false, delay);
        cache->hook_read(addrr, v_ai, v_s, v_w, false, delay);
      }
      this->outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    if(cmd == Policy::cmd_for_core_write()) {
      meta->to_dirty();
      cache->hook_write(addr, ai, s, w, hit, delay);
    } else
      cache->hook_read(addr, ai, s, w, hit, delay);
    return data;
  }
};


#endif


