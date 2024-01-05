#include "util/monitor.hpp"
#include "util/random.hpp"
#include <iostream>
#include <boost/format.hpp>
#include "cache/metadata.hpp"
#include <numeric>

static boost::format    read_fmt("%-10s read  %016x %02d %04d %02d %1x");
static boost::format   write_fmt("%-10s write %016x %02d %04d %02d %1x");
static boost::format invalid_fmt("%-10s evict %016x %02d %04d %02d  ");
static boost::format    data_fmt("%016x");

void SimpleTracer::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::cout << (read_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit);

  if(meta)
    std::cout << " [" << meta->to_string() << "]";
  else if(data)
    std::cout << "      ";

  if(data) {
    std::cout << " ";
    if(compact_data) std::cout << (data_fmt % (data->read(0)));
    else             std::cout << data->to_string();
  }
  std::cout << std::endl;
}

void SimpleTracer::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  std::cout << (write_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit);

  if(meta)
    std::cout << " [" << meta->to_string() << "]";
  else if(data)
    std::cout << "      ";

  if(data) {
    std::cout << " ";
    if(compact_data) std::cout << (data_fmt % (data->read(0)));
    else             std::cout << data->to_string();
  }
  std::cout << std::endl;
}

void SimpleTracer::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, const CMMetadataBase *meta, const CMDataBase *data) {
  std::cout << (invalid_fmt % UniqueID::name(cache_id) % addr % ai % s % w) ;

  if(meta)
    std::cout << " [" << meta->to_string() << "]";
  else if(data)
    std::cout << "      ";

  if(data) {
    std::cout << " ";
    if(compact_data) std::cout << (data_fmt % (data->read(0)));
    else             std::cout << data->to_string();
  }
  std::cout << std::endl;
}

bool HLLMonitor::HLL_detect(){
  double sum = 0.0;
  for (uint32_t i = 0; i < m_; i++) {
    sum += 1.0 / (1 << M_[i]);
  }
  estimate = alphaMM_ / sum; 
  if (estimate <= 2.5 * m_) {
    uint32_t V_zeros = 0;
    for (uint32_t i = 0; i < m_; i++) {
      if (M_[i] == 0) {
        V_zeros++;
      }
    }
    if (V_zeros != 0) {
      estimate = m_ * std::log(static_cast<double>(m_) / V_zeros);
    }
  }
  else if (estimate > (1.0 / 30.0) * pow_2_32) {
    estimate = neg_pow_2_32 * log(1.0 - (estimate / pow_2_32));
  }
  if(estimate > threshold){
    return true;
  }  
  return false;
}

void HLLMonitor::HLL_update(uint64_t addr){
  uint32_t hash;
  std::hash<std::string> h;
  hash = h(std::to_string(addr));
  uint32_t index = hash >> (32 - b_);
  int leading_zero_len = 32;
  leading_zero_len = __builtin_clz((hash << b_)) + 1;
  uint8_t rank = std::min((uint8_t)(32 - b_), (uint8_t)leading_zero_len);
  if (rank > M_[index]) {
    M_[index] = rank;
  }
}

void HLLMonitor::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  cnt_access++;
  if(!hit) cnt_miss++;
  HLL_update(addr);
  if(cnt_access != 0 && (cnt_access % period) == 0){
    if(HLL_detect()){
      std::cout << (read_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit) << " estimate:" << estimate << std::endl;
    }
    clean_history();
  }
}

void HLLMonitor::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  cnt_access++;
  cnt_write++;
  if(!hit) {
    cnt_miss++;
    cnt_write_miss++;
  }
  HLL_update(addr);
  if(cnt_access != 0 && (cnt_access % period) == 0){
    if(HLL_detect()){
      std::cout << (write_fmt % UniqueID::name(cache_id) % addr % ai % s % w % hit) << " estimate:" << estimate << std::endl;
    }
    clean_history();
  }
}

void HLLMonitor::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, const CMMetadataBase *meta, const CMDataBase *data) {
  cnt_invalid++;
}

void HLLMonitor::clean_history() {
  std::fill(M_.begin(),M_.end(),0);
  estimate = 0.0;
}    

bool SetCSAMonitor::set_evict_detect(){
  double qrm  = sqrt(std::accumulate(evicts.begin(), evicts.end(), 0.0,
                                     [](double q, const uint64_t& d){return q + d * d;})
                     / (nset-1.0)
                     );
  double mu   = std::accumulate(evicts.begin(), evicts.end(), 0.0) / (double)(nset);
  for(size_t i=0; i<nset; i++) {
    double delta = qrm == 0.0 ? 0.0 : (evicts[i] - mu) * (evicts[i] - mu) / qrm;
    set_evict_history[i] = factor * set_evict_history[i] +
      (evicts[i] > mu ? delta : -delta);
  }

  for(size_t i=0; i<nset; i++)
    if(set_evict_history[i] >= threshold) {
      //std::cerr << "found set " << i << std::endl;
      return true;
    }

  return false;
}

void SetCSAMonitor::read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  accesses++;
  if(remap_enable && access_period != 0 && (accesses % access_period) == 0) {
    if(set_evict_detect()) {
      //if(wall_time)
      //  std::cerr << *wall_time << ":";
      //std::cerr << " remapped by detect @" << accesses  << std::endl;
    }
    evicts = std::vector<uint64_t>(nset, 0);
  }
}

void SetCSAMonitor::write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, bool hit, const CMMetadataBase *meta, const CMDataBase *data) {
  accesses++;
  if(remap_enable && access_period != 0 && (accesses % access_period) == 0) {
    if(set_evict_detect()) {
      //if(wall_time)
      //  std::cerr << *wall_time << ":";
      //std::cerr << " remapped by detect @" << accesses  << std::endl;
    }
    evicts = std::vector<uint64_t>(nset, 0);
  }
}

void SetCSAMonitor::invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, const CMMetadataBase *meta, const CMDataBase *data) {
  total_evicts++;
  evicts[s]++;
  if(remap_enable && evict_period != 0 && (total_evicts % evict_period) == 0) {
    //if(wall_time)
    //    std::cerr << *wall_time << ":";
    //  std::cerr << " remapped by eviction limit @" << total_evicts  << std::endl;
  }
}

void SetCSAMonitor::clean_history() {
  evicts = std::vector<uint64_t>(nset, 0);
  set_evict_history = std::vector<double>(nset, 0.0);
  if(access_period != 0) accesses -= (accesses % access_period);
  if(evict_period != 0) total_evicts -= (total_evicts % evict_period);
} 