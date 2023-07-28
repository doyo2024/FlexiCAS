#ifndef CM_CACHE_VICTIM_HPP
#define CM_CACHE_VICTIM_HPP

#include "cache/msi.hpp"
#include <iostream>

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
    if(hit = this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if(ai == this->cache->get_Partition_size()){ // If addr hits in the victim cache, it needs to be swapped into the cache
        DT copy_data;
        bool shared, dirty;
        this->cache->replace(addr, &v_ai, &v_s, &v_w);
        mmeta = this->cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = this->cache->get_data(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) copy_data.copy(data);
        if(meta->is_dirty()) dirty = true; if(meta->is_shared()) shared = true;
        this->cache->hook_manage(addr, ai, s, w, true, true, false, delay);
        if(mmeta->is_valid()){
          addrr = mmeta->addr(v_s);
          Policy::meta_copy_state(meta, mmeta, addrr);
          if constexpr (!std::is_void<DT>::value) data->copy(ddata);
          this->cache->hook_manage(addrr, v_ai, v_s, v_w, true, true, false, delay);
          this->cache->hook_read(addrr, ai, s, w, false, delay);
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
      this->cache->replace(addr, &ai, &s, &w, &v_ai, &v_s, &v_w);
      meta = this->cache->access(ai, s, w); 
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if(meta->is_valid()){
        addrr = meta->addr(s);
        mmeta = this->cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = this->cache->get_data(v_ai, v_s, v_w);
        if(mmeta->is_valid()) {
            auto replace_addr = mmeta->addr(s);
            if(Policy::need_sync(Policy::cmd_for_evict(), mmeta)) this->probe_req(replace_addr, mmeta, ddata, Policy::cmd_for_sync(Policy::cmd_for_evict()), delay); // sync if necessary
            if(writeback = mmeta->is_dirty()) this->outer->writeback_req(replace_addr, mmeta, ddata, Policy::cmd_for_evict(), delay); // writeback if dirty
            this->cache->hook_manage(replace_addr, v_ai, v_s, v_w, true, true, writeback, delay);
        }
        Policy::meta_copy_state(mmeta, meta, addrr); meta->to_invalid(); meta->to_clean();
        ddata->copy(data);
        this->cache->hook_manage(addrr, ai, s, w, true, true, false, delay);
        this->cache->hook_read(addrr, v_ai, v_s, v_w, false, delay);
      }
      this->outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    if constexpr (!std::is_void<DT>::value) data_inner->copy(this->cache->get_data(ai, s, w));
    Policy::meta_after_acquire(cmd, meta);
    this->cache->hook_read(addr, ai, s, w, hit, delay);
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
    if(hit = this->cache->hit(addr, &ai, &s, &w)) { // hit
      meta = this->cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if(ai == this->cache->get_Partition_size()){ // If addr hits in the victim cache, it needs to be swapped into the cache
        DT copy_data;
        bool shared, dirty;
        this->cache->replace(addr, &v_ai, &v_s, &v_w);
        mmeta = this->cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = this->cache->get_data(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) copy_data.copy(data);
        if(meta->is_dirty()) dirty = true; if(meta->is_shared()) shared = true;
        this->cache->hook_manage(addr, ai, s, w, true, true, false, delay);
        if(mmeta->is_valid()){
          addrr = mmeta->addr(v_s);
          Policy::meta_copy_state(meta, mmeta, addrr);
          if constexpr (!std::is_void<DT>::value) data->copy(ddata);
          this->cache->hook_manage(addrr, v_ai, v_s, v_w, true, true, false, delay);
          this->cache->hook_read(addrr, ai, s, w, false, delay);
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
      this->cache->replace(addr, &ai, &s, &w, &v_ai, &v_s, &v_w);
      meta = this->cache->access(ai, s, w);
      if constexpr (!std::is_void<DT>::value) data = this->cache->get_data(ai, s, w);
      if(meta->is_valid()){
        addrr = meta->addr(s);
        mmeta = this->cache->access(v_ai, v_s, v_w);
        if constexpr (!std::is_void<DT>::value) ddata = this->cache->get_data(v_ai, v_s, v_w);
        if(mmeta->is_valid()) {
            auto replace_addr = mmeta->addr(s);
            if(writeback = mmeta->is_dirty()) this->outer->writeback_req(replace_addr, mmeta, ddata, Policy::cmd_for_evict(), delay); // writeback if dirty
            this->cache->hook_manage(replace_addr, v_ai, v_s, v_w, true, true, writeback, delay);
        }
        Policy::meta_copy_state(mmeta, meta, addrr); meta->to_invalid(); meta->to_clean();
        ddata->copy(data);
        this->cache->hook_manage(addrr, ai, s, w, true, true, false, delay);
        this->cache->hook_read(addrr, v_ai, v_s, v_w, delay);
      }
      this->outer->acquire_req(addr, meta, data, cmd, delay); // fetch the missing block
    }
    if(cmd == Policy::cmd_for_core_write()) {
      meta->to_dirty();
      this->cache->hook_write(addr, ai, s, w, hit, delay);
    } else
      this->cache->hook_read(addr, ai, s, w, hit, delay);
    return data;
  }
};


#endif


