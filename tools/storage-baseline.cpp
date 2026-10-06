// Run each tier in a separate process; ru_maxrss is a process high-water mark.
#include "graphdb/GraphEngine.hpp"
#include <sys/resource.h>
#include <iostream>
#include <unistd.h>
using namespace graphdb;
uint64_t peak_bytes() { rusage r{};getrusage(RUSAGE_SELF,&r);
#ifdef __APPLE__
return r.ru_maxrss;
#else
return r.ru_maxrss*1024;
#endif
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  auto dir=std::filesystem::temp_directory_path()/("nova-baseline-"+std::to_string(getpid()));
  struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{dir};
  auto hot=std::make_shared<MemoryHotStore>();auto disk=std::make_shared<FileDiskStore>(dir/"cold");
  EngineConfig cfg;cfg.database_dir=dir;GraphEngine engine(hot,disk,cfg);
  for(int i=0;i<1000;++i){auto s=engine.upsert_node("Node",std::to_string(i),{{"payload",std::string(512,'x')}});if(!s.ok)return 3;
    if(i){s=engine.upsert_edge("NEXT",std::to_string(i-1),std::to_string(i),{});if(!s.ok)return 3;}}
  if(std::string(argv[1])=="cold")for(const auto& rec:hot->dump_records()){
    auto key=object_key(rec.object.kind,rec.object.id);auto written=disk->write(key,rec.object);if(!written)return 4;
    if(!hot->move_to_cold_if_version(key,rec.object.version,written.value))return 4;}
  const auto before=peak_bytes();auto result=engine.checkpoint();if(!result.ok)return 5;const auto after=peak_bytes();
  size_t files=0,bytes=0;for(auto& entry:std::filesystem::recursive_directory_iterator(dir))if(entry.is_regular_file()){++files;bytes+=entry.file_size();}
  std::cout<<"{\"tier\":\""<<argv[1]<<"\",\"nodes\":1000,\"edges\":999,\"propertyBytesPerNode\":512,\"files\":"<<files<<",\"fileBytes\":"<<bytes<<",\"catalogBytes\":"<<std::filesystem::file_size(dir/"catalog.db")<<",\"peakRSSBeforeCheckpoint\":"<<before<<",\"peakRSSAfterCheckpoint\":"<<after<<",\"peakRSSIncrease\":"<<(after-before)<<"}\n";
}
