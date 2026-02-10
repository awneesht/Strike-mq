#include "core/types.h"
#include "utils/ring_buffer.h"
#include "utils/memory_pool.h"
#include "storage/partition_log.h"
#include "protocol/kafka_codec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>
using namespace strike;

uint64_t estimate_tsc_freq() {
    auto s = std::chrono::high_resolution_clock::now();
    uint64_t st = rdtsc();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - s).count() < 100);
    return (rdtsc() - st) * 1000000000ULL /
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - s).count();
}

struct TSCConv { uint64_t f; double npc; TSCConv(uint64_t f):f(f),npc(1e9/f){} double ns(uint64_t c)const{return c*npc;} };

struct Hist {
    std::vector<double> s; std::string name;
    Hist(std::string n):name(std::move(n)){}
    void add(double v){s.push_back(v);}
    void print()const{
        if(s.empty())return; auto sorted=s; std::sort(sorted.begin(),sorted.end());
        double sum=std::accumulate(sorted.begin(),sorted.end(),0.0);
        size_t n=sorted.size();
        std::cout<<"┌─ "<<name<<" ("<<n<<" samples)\n"
            <<"│  avg:   "<<std::fixed<<std::setprecision(1)<<sum/n<<" ns\n"
            <<"│  p50:   "<<sorted[n*50/100]<<" ns\n"
            <<"│  p90:   "<<sorted[n*90/100]<<" ns\n"
            <<"│  p99:   "<<sorted[n*99/100]<<" ns\n"
            <<"│  p99.9: "<<sorted[std::min(n-1,n*999/1000)]<<" ns\n"
            <<"│  max:   "<<sorted.back()<<" ns\n"
            <<"│  p99.9 < 1ms: "<<(sorted[std::min(n-1,n*999/1000)]<1e6?"✅ PASS":"❌ FAIL")<<"\n"
            <<"└───────────────────────────────────\n\n";
    }
};

int main(){
    std::cout<<"═══════════════════════════════════════════\n  StrikeMQ Latency Benchmark\n═══════════════════════════════════════════\n\n";
    uint64_t f=estimate_tsc_freq(); TSCConv tsc(f);
    std::cout<<"TSC: "<<f/1000000<<" MHz\n\n";

    {Hist h("SPSC push+pop"); SPSCRingBuffer<uint64_t,65536> r;
     for(int i=0;i<10000;++i){(void)r.try_push(i);r.try_pop();}
     for(int i=0;i<1000000;++i){uint64_t s=rdtsc();(void)r.try_push(i);r.try_pop();h.add(tsc.ns(rdtsc()-s));}
     h.print();}

    {Hist h("Pool alloc+free"); HugePagePool pool(4096,65536);
     for(int i=0;i<10000;++i){void*p=pool.allocate();pool.deallocate(p);}
     for(int i=0;i<1000000;++i){uint64_t s=rdtsc();void*p=pool.allocate();pool.deallocate(p);h.add(tsc.ns(rdtsc()-s));}
     h.print();}

    {Hist h("Log append 1KB"); auto tmp=std::filesystem::temp_directory_path()/"strikemq_bench";
     std::filesystem::create_directories(tmp);
     TopicPartition tp{"bench",0}; storage::PartitionLog log(tp,tmp.string(),1073741824);
     uint8_t payload[1024]; std::memset(payload,'A',sizeof(payload));
     for(int i=0;i<100000;++i){
       RecordBatch b; b.topic_partition=tp; Record r; r.data=payload; r.value_length=sizeof(payload); r.total_size=sizeof(payload);
       b.records.push_back(r); uint64_t s=rdtsc(); (void)log.append(b); h.add(tsc.ns(rdtsc()-s));
     }
     h.print(); std::filesystem::remove_all(tmp);}

    {Hist h("Kafka header decode"); uint8_t buf[128]; protocol::BinaryWriter w(buf,128);
     w.write_int16(0);w.write_int16(7);w.write_int32(42);w.write_string("client");
     uint32_t len=w.position();
     for(int i=0;i<10000;++i){protocol::BinaryReader r(buf,len);protocol::KafkaDecoder::decode_header(r);}
     for(int i=0;i<1000000;++i){uint64_t s=rdtsc();protocol::BinaryReader r(buf,len);protocol::KafkaDecoder::decode_header(r);h.add(tsc.ns(rdtsc()-s));}
     h.print();}

    std::cout<<"═══════════════════════════════════════════\n  Benchmark complete.\n═══════════════════════════════════════════\n";
}
