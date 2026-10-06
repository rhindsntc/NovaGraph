// T21 diagnostic executable. No shipping API or durability behavior is changed.
#include "graphdb/GraphEngine.hpp"
#include "graphdb/DatabaseLock.hpp"
#include "nlohmann/json.hpp"
#include <sqlite3.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <set>
#include <iomanip>
using namespace graphdb;
using json=nlohmann::json;
using Clock=std::chrono::steady_clock;
#ifdef NOVA_RESOURCE_DIAGNOSTICS
#include "AllocationTracker.hpp"
#include "ResourceDisk.hpp"
#else
static std::atomic<uint64_t> new_calls{0};
void* operator new(std::size_t n) {++new_calls;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
#endif
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void require(Status s){check(s.ok,s.message);}
std::string id(size_t n){std::ostringstream s;s<<"n:"<<std::setfill('0')<<std::setw(6)<<n;return s.str();}
struct IO:PosixFileIO {
 uint64_t read_bytes=0,write_bytes=0,cold_bytes=0,reads=0,writes=0,syncs=0;
 Result<std::vector<uint8_t>> read(const std::filesystem::path& p,size_t n) override {auto r=PosixFileIO::read(p,n);++reads;if(r){read_bytes+=r.value.size();if(p.parent_path().filename()=="cold")cold_bytes+=r.value.size();}return r;}
 Result<std::unique_ptr<SequentialReader>> open_reader(const std::filesystem::path& p,size_t maximum,size_t fallback) override {
   auto r=PosixFileIO::open_reader(p,maximum,fallback);if(!r)return {r.status,{}};
   struct Reader:SequentialReader {
     IO *owner;bool cold;std::unique_ptr<SequentialReader> inner;
     Reader(IO *o,bool c,std::unique_ptr<SequentialReader> r):owner(o),cold(c),inner(std::move(r)){}
     uint64_t size() const override{return inner->size();}
     Result<size_t> read(uint8_t *out,size_t n) override {auto r=inner->read(out,n);++owner->reads;if(r){owner->read_bytes+=r.value;if(cold)owner->cold_bytes+=r.value;}return r;}
     Status close() override{return inner->close();}
   };
   return {Status::OK(),std::make_unique<Reader>(this,p.parent_path().filename()=="cold",std::move(r.value))};
 }
 Status write_all(const std::filesystem::path& p,const std::vector<uint8_t>& b,WriteMode m) override{++writes;write_bytes+=b.size();return PosixFileIO::write_all(p,b,m);}
 Status sync_file(const std::filesystem::path& p) override{++syncs;return PosixFileIO::sync_file(p);}
 Status sync_files(const std::vector<std::filesystem::path>& p) override{syncs+=p.size();return PosixFileIO::sync_files(p);}
 json snapshot()const{return {{"readBytes",read_bytes},{"coldReadBytes",cold_bytes},{"writeBytesAttempted",write_bytes},{"readCalls",reads},{"writeCalls",writes},{"fileSyncMembers",syncs}};}
};
uint64_t rss(){mach_task_basic_info_data_t info{};mach_msg_type_number_t n=MACH_TASK_BASIC_INFO_COUNT;check(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,(task_info_t)&info,&n)==KERN_SUCCESS,"task_info failed");return info.resident_size;}
uint64_t peak(){rusage r{};check(getrusage(RUSAGE_SELF,&r)==0,"getrusage failed");return r.ru_maxrss;}
json disk(const std::filesystem::path& p){uint64_t bytes=0,files=0;for(auto& e:std::filesystem::recursive_directory_iterator(p))if(e.is_regular_file()){bytes+=e.file_size();++files;}return {{"bytes",bytes},{"files",files}};}
struct Owner {
 std::unique_ptr<DatabaseLock> lock;
 std::unique_ptr<GraphEngine> engine;
 Owner(const std::filesystem::path& p,std::shared_ptr<IO> io){auto l=DatabaseLock::acquire(p);require(l.status);lock=std::move(l.value);auto r=Recovery::open(p,io);require(r.status);EngineConfig c;c.database_dir=p;c.file_io=io;c.checkpoint_on_destroy=false;engine=std::make_unique<GraphEngine>(std::make_shared<MemoryHotStore>(),std::make_shared<FileDiskStore>(p/"cold",io),c,r.value->wal(),r.value);require(engine->recover());}
};
struct Dataset {
 std::vector<std::vector<size_t>> edges;
 std::string payload;
 Dataset(std::string kind,size_t n,uint64_t seed):edges(n),payload(kind=="messages"?512:kind=="knowledge"?1024:64,'x') {
  auto random=[&](){seed=seed*6364136223846793005ULL+1442695040888963407ULL;return seed;};
  for(size_t i=0;i<n;++i){std::set<size_t> to;
   if(kind=="navigation") {if(2*i+1<n)to.insert(2*i+1);if(2*i+2<n)to.insert(2*i+2);}
   else if(kind=="messages") {if(i+1<n)to.insert(i+1);if(i%16!=0)to.insert(i-i%16);}
   else {for(int k=0;k<(kind=="contacts"?3:5);++k){auto t=random()%n;if(t!=i)to.insert(t);}}
   if(kind=="recommendations" && i==0)for(size_t t=1;t<std::min(n,size_t(257));++t)to.insert(t);
   edges[i]={to.begin(),to.end()};
  }
 }
 std::vector<std::string> walk(size_t from,size_t depth,size_t limit)const {
  std::set<size_t> seen{from};std::queue<std::pair<size_t,size_t>> q;q.push({from,0});std::vector<std::string> result;
  while(!q.empty()&&result.size()<limit){auto [v,d]=q.front();q.pop();if(d==depth)continue;for(auto t:edges[v])if(seen.insert(t).second){result.push_back(id(t));q.push({t,d+1});if(result.size()==limit)break;}}
  std::sort(result.begin(),result.end());return result;
 }
};
struct SQLite {
 sqlite3* db=nullptr;sqlite3_stmt* neighbors=nullptr;
 void exec(const char* s){char* err=nullptr;int rc=sqlite3_exec(db,s,nullptr,nullptr,&err);std::string text=err?err:"sqlite failure";sqlite3_free(err);check(rc==SQLITE_OK,text);}
 explicit SQLite(const Dataset& d,const std::filesystem::path& p){check(sqlite3_open(p.c_str(),&db)==SQLITE_OK,"sqlite open");exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE nodes(id INTEGER PRIMARY KEY,payload TEXT); CREATE TABLE edges(src INTEGER,dst INTEGER,PRIMARY KEY(src,dst)); BEGIN;");sqlite3_stmt* node=nullptr;sqlite3_stmt* edge=nullptr;check(sqlite3_prepare_v2(db,"INSERT INTO nodes VALUES(?,?)",-1,&node,nullptr)==SQLITE_OK,"prepare node");check(sqlite3_prepare_v2(db,"INSERT INTO edges VALUES(?,?)",-1,&edge,nullptr)==SQLITE_OK,"prepare edge");for(size_t i=0;i<d.edges.size();++i){sqlite3_bind_int64(node,1,i);sqlite3_bind_text(node,2,d.payload.c_str(),-1,SQLITE_TRANSIENT);check(sqlite3_step(node)==SQLITE_DONE,"insert node");sqlite3_reset(node);for(auto t:d.edges[i]){sqlite3_bind_int64(edge,1,i);sqlite3_bind_int64(edge,2,t);check(sqlite3_step(edge)==SQLITE_DONE,"insert edge");sqlite3_reset(edge);}}sqlite3_finalize(node);sqlite3_finalize(edge);exec("COMMIT;");check(sqlite3_prepare_v2(db,"SELECT dst FROM edges WHERE src=? ORDER BY dst",-1,&neighbors,nullptr)==SQLITE_OK,"prepare neighbor");}
 ~SQLite(){sqlite3_finalize(neighbors);sqlite3_close(db);}
 std::vector<std::string> walk(size_t from,size_t depth,size_t limit){std::set<size_t> seen{from};std::queue<std::pair<size_t,size_t>> q;q.push({from,0});std::vector<std::string> result;while(!q.empty()&&result.size()<limit){auto[v,d]=q.front();q.pop();if(d==depth)continue;sqlite3_bind_int64(neighbors,1,v);int rc;while((rc=sqlite3_step(neighbors))==SQLITE_ROW){auto t=sqlite3_column_int64(neighbors,0);if(seen.insert(t).second){result.push_back(id(t));q.push({t,d+1});if(result.size()==limit)break;}}check(rc==SQLITE_ROW||rc==SQLITE_DONE,"sqlite traversal");sqlite3_reset(neighbors);}std::sort(result.begin(),result.end());return result;}
};
int main(int argc,char**argv){
 std::filesystem::path root;std::string phase="arguments";size_t committed=0;
 try {
  check(argc==6,"usage: native workload nodes seed samples warmup");std::string kind=argv[1];check(std::set<std::string>{"contacts","messages","knowledge","navigation","recommendations"}.contains(kind),"unknown workload");
  auto number=[](const char* s){std::string v=s;check(!v.empty()&&v.find_first_not_of("0123456789")==std::string::npos,"invalid integer");return std::stoull(v);};
  size_t n=number(argv[2]),samples=number(argv[4]),warmup=number(argv[5]);uint64_t seed=number(argv[3]);check(n>=16&&n<=10000&&samples>=1&&samples<=1000&&warmup<=1000,"out of range");
  root=std::filesystem::temp_directory_path()/("nova-performance-"+new_transaction_id());std::filesystem::create_directories(root);
  Dataset data(kind,n,seed);auto io=std::make_shared<IO>();auto owner=std::make_unique<Owner>(root/"nova",io);auto& e=*owner->engine;
  json out={{"protocolVersion",2},{"mutationBatchSize",250},{"metrics",json::object()},{"cppNewCalls",json::object()}};
#ifdef NOVA_RESOURCE_DIAGNOSTICS
  struct Workspace {
   uint64_t requests=0,peak=0,current=0,limit=0;
   void observe(const QueryContext& c) {++requests;peak=std::max<uint64_t>(peak,c.working_peak());current=std::max<uint64_t>(current,c.working_used());limit=std::max<uint64_t>(limit,c.options().working_bytes);}
  } workspace;
  bool tracking=false;
  out["resources"]={{"schemaVersion",2},{"instrumented",true},{"coverage","Successful process-wide C++ new requests inside named phases, including harness validation; excludes malloc/Swift/allocator overhead. Workspace observes explicit commit/read/trim/GC contexts. Separate untimed DSL traversal/checkpoint probes include parser/serialization reservation work and report admission failure; they do not observe the direct traversal/checkpoint/close phases. Open/recovery and setup index construction have no observable explicit context; zero requests means unavailable, not zero engine workspace. Mutation batch construction and correctness checks are outside measured phases. Regular-file snapshots exclude directories and do not measure transient peaks or exclusive APFS blocks."},{"phases",json::object()},{"diskSnapshots",json::array()}};
  auto observe=[&](const QueryContext& c){if(tracking)workspace.observe(c);};
  auto snapshot=[&](const std::string& stage){out["resources"]["diskSnapshots"].push_back(nova_resource::disk_snapshot(root/"nova",stage));};
#else
  auto observe=[](const QueryContext&){};
  auto snapshot=[](const std::string&){};
#endif
  auto measure=[&](std::string name,auto op){
#ifdef NOVA_RESOURCE_DIAGNOSTICS
   bool tracked=name!="sqliteTraversalUs"&&name!="memoryTraversalUs";
   if(tracked){workspace={};tracking=true;nova_resource::begin_phase();}
#else
   uint64_t allocations=new_calls.load();
#endif
   auto start=Clock::now();op();double us=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
#ifdef NOVA_RESOURCE_DIAGNOSTICS
   auto allocation=tracked?nova_resource::end_phase():nova_resource::AllocationMetrics{};tracking=false;
   auto count=allocation.calls;
   if(tracked){
    auto& r=out["resources"]["phases"][name];
    if(r.is_null())r={{"samples",0},{"allocation",{{"calls",0},{"totalBytes",0},{"peakBytes",0},{"maxRetainedBytes",0}}},{"workspace",{{"requests",0},{"maxPeakBytes",0},{"maxCurrentBytes",0},{"maxLimitBytes",0}}}};
    r["samples"]=r["samples"].get<uint64_t>()+1;
    auto& a=r["allocation"];a["calls"]=a["calls"].get<uint64_t>()+allocation.calls;a["totalBytes"]=a["totalBytes"].get<uint64_t>()+allocation.totalBytes;
    a["peakBytes"]=std::max(a["peakBytes"].get<uint64_t>(),allocation.peakBytes);a["maxRetainedBytes"]=std::max(a["maxRetainedBytes"].get<uint64_t>(),allocation.liveBytes);
    auto& w=r["workspace"];w["requests"]=w["requests"].get<uint64_t>()+workspace.requests;
    w["maxPeakBytes"]=std::max(w["maxPeakBytes"].get<uint64_t>(),workspace.peak);w["maxCurrentBytes"]=std::max(w["maxCurrentBytes"].get<uint64_t>(),workspace.current);w["maxLimitBytes"]=std::max(w["maxLimitBytes"].get<uint64_t>(),workspace.limit);
   }
#else
   auto count=new_calls.load()-allocations;
#endif
   out["metrics"][name].push_back(us);out["cppNewCalls"][name].push_back(count);
  };
  auto read=[&](const std::string& key){
#ifdef NOVA_RESOURCE_DIAGNOSTICS
   QueryContext c;auto session=e.read_snapshot(c);auto r=session.get_node(key);observe(c);return r;
#else
   return e.get_node(key);
#endif
  };
  phase="seed";auto commit=[&](std::vector<Mutation> changes){TransactionBatch b{new_transaction_id(),std::move(changes)};QueryContext c;require(e.commit_batch(b,c).status);observe(c);committed+=b.mutations.size();};
  measure("setupUs",[&]{std::vector<Mutation> batch;for(size_t i=0;i<n;++i){batch.push_back(UpsertNode{"Record",id(i),{{"payload",data.payload},{"ordinal",int64_t(i)}}});if(batch.size()==250){commit(std::move(batch));batch.clear();}}if(!batch.empty())commit(std::move(batch));batch.clear();for(size_t i=0;i<n;++i)for(auto t:data.edges[i]){batch.push_back(UpsertEdge{"LINK",id(i),id(t),{}});if(batch.size()==250){commit(std::move(batch));batch.clear();}}if(!batch.empty())commit(std::move(batch));require(e.create_node_property_index("Record","ordinal"));});
  phase="reads-and-comparisons";auto hot=e.memory_usage();out["memoryBeforeTrim"]={{"hotPayloadBytes",hot.hot_payload_bytes},{"metadataAndIndexBytes",hot.metadata_bytes},{"rssBytes",rss()}};
  for(size_t i=0;i<warmup;++i)require(e.get_node(id(i%n)).status);
  for(size_t i=0;i<samples;++i){measure("hotReadUs",[&]{auto r=read(id(i%n));require(r.status);check(r.value.id==id(i%n),"hot read mismatch");});}
  auto compare=[&](size_t from){auto r=e.traverse_out(id(from),"LINK",2,100);require(r.status);std::vector<std::string> ids;for(auto& o:r.value.nodes)ids.push_back(o.id);std::sort(ids.begin(),ids.end());return ids;};
  {
   SQLite sqlite(data,root/"baseline.sqlite");
   for(size_t i=0;i<warmup+samples;++i){auto from=(i*17)%n;auto want=data.walk(from,2,100);check(compare(from)==want,"Nova traversal differs from reference");check(sqlite.walk(from,2,100)==want,"SQLite traversal differs from reference");if(i>=warmup){measure("novaTraversalUs",[&]{check(compare(from)==want,"Nova result changed");});measure("sqliteTraversalUs",[&]{check(sqlite.walk(from,2,100)==want,"SQLite result changed");});measure("memoryTraversalUs",[&]{check(data.walk(from,2,100)==want,"reference changed");});}}
  }
  auto drain=[&]{TrimResult total;while(true){QueryContext context;auto r=e.trim_memory(0,context);require(r.status);observe(context);total.evicted_bytes+=r.value.evicted_bytes;total.evicted_count+=r.value.evicted_count;if(!r.value.unmet_bytes)return total;check(r.value.evicted_count>0,"trim made no progress");}};
  phase="trim";
  TrimResult trimmed;measure("trimUs",[&]{trimmed=drain();});out["evictedBytes"]=trimmed.evicted_bytes;out["evictedRecords"]=trimmed.evicted_count;snapshot("after-trim");
  auto cold=e.memory_usage();check(cold.hot_payload_bytes==0&&hot.hot_payload_bytes>0,"trim did not reduce accounted payload");out["memoryAfterTrim"]={{"hotPayloadBytes",cold.hot_payload_bytes},{"metadataAndIndexBytes",cold.metadata_bytes},{"rssBytes",rss()}};auto cold_before=io->cold_bytes;
  // Unique point reads: every measured lookup starts cold, even with >n requested samples.
  for(size_t i=0;i<samples;++i){if(i&&i%n==0){drain();}measure("coldReadUs",[&]{auto r=read(id(i%n));require(r.status);check(r.value.id==id(i%n),"cold read mismatch");});}
  out["coldPromotionReadBytes"]=io->cold_bytes-cold_before;
  phase="overwrite-delete-maintenance";for(size_t cycle=0;cycle<3;++cycle){auto maintenance=[&]{std::vector<Mutation> b;for(size_t i=0;i<std::min(n,size_t(100));++i)b.push_back(UpsertNode{"Record",id(i),{{"payload",data.payload+std::to_string(cycle)},{"ordinal",int64_t(i)}}});b.push_back(UpsertNode{"Scratch","scratch",{}});commit(std::move(b));commit({DeleteNode{"scratch"}});require(e.checkpoint());QueryContext c;require(e.collect_garbage(100000,c).status);observe(c);};
#ifdef NOVA_RESOURCE_DIAGNOSTICS
   measure("maintenanceUs",maintenance);
#else
   maintenance();
#endif
   out["maintenanceTrend"].push_back(disk(root/"nova"));snapshot("maintenance-"+std::to_string(cycle));}
  phase="checkpoint";measure("checkpointUs",[&]{require(e.checkpoint());});
  snapshot("final-checkpoint");out["disk"]=disk(root/"nova");out["io"]=io->snapshot();
  measure("closeUs",[&]{require(e.checkpoint());owner.reset();});snapshot("after-close");
  phase="open";measure("openUs",[&]{owner=std::make_unique<Owner>(root/"nova",io);});
  require(owner->engine->get_node(id(n-1)).status);snapshot("after-reopen");require(owner->engine->checkpoint());
#ifdef NOVA_RESOURCE_DIAGNOSTICS
  // These probes run after all graph lifecycle snapshots, outside every named phase.
  // DSL reservation admission can fail even when the direct checkpoint API succeeds.
  for(const auto& probe:std::vector<std::pair<std::string,std::string>>{
      {"traversal","walk from \""+id(0)+"\" over LINK depth 2 limit 100"},{"checkpoint","checkpoint"}}){
   QueryContext context;auto result=owner->engine->execute_dsl(probe.second,{},context);
   check(result.ok||result.status.code==ErrorCode::limitExceeded,"workspace probe failed: "+result.error);
   Workspace observed;observed.observe(context);
   out["resources"]["workspaceProbes"][probe.first]={{"api","execute_dsl"},
    {"status",result.ok?"observed":"limit-exceeded"},{"workspace",{{"requests",observed.requests},
    {"maxPeakBytes",observed.peak},{"maxCurrentBytes",observed.current},{"maxLimitBytes",observed.limit}}}};
  }
#endif
  owner.reset();
  // Mutation data is isolated from graph fixtures and all seven graph disk snapshots.
  // Seed, batch construction, status/result checks, reads and durable reopen are untimed.
  phase="durable-mutations";
  constexpr size_t mutation_records=250;
  auto mutation_io=std::make_shared<IO>();
  auto mutation_owner=std::make_unique<Owner>(root/"mutations",mutation_io);
  std::vector<std::string> mutation_ids;std::vector<int64_t> expected(mutation_records,0);
  TransactionBatch seed_batch{new_transaction_id(),{}};
  for(size_t i=0;i<mutation_records;++i){mutation_ids.push_back(id(i));seed_batch.mutations.push_back(UpsertNode{"Mutation",mutation_ids.back(),{{"value",int64_t(0)}}});}
  {QueryContext c;require(mutation_owner->engine->commit_batch(seed_batch,c).status);}
  auto validate_record=[&](size_t i){auto r=mutation_owner->engine->get_node(mutation_ids[i]);require(r.status);
   check(r.value.label_or_type=="Mutation"&&r.value.id==mutation_ids[i]&&r.value.properties.size()==1&&
         r.value.properties.at("value")==Property(expected[i]),"mutation result mismatch");};
  auto reopen_mutations=[&]{mutation_owner.reset();mutation_owner=std::make_unique<Owner>(root/"mutations",mutation_io);
   for(size_t i=0;i<mutation_records;++i)validate_record(i);};
  for(bool batched:{false,true}){
   for(size_t sample=0;sample<warmup+samples;++sample){
    const size_t first=batched?0:sample%mutation_records,count=batched?mutation_records:1;
    const int64_t value=int64_t(sample+1+(batched?warmup+samples:0));
    TransactionBatch batch{new_transaction_id(),{}};batch.mutations.reserve(count);
    for(size_t i=first;i<first+count;++i)batch.mutations.push_back(UpsertNode{"Mutation",mutation_ids[i],{{"value",value}}});
    QueryContext context;Result<CommitReceipt> receipt;const auto syncs_before=mutation_io->syncs;
    auto operation=[&]{receipt=mutation_owner->engine->commit_batch(batch,context);observe(context);};
    if(sample<warmup)operation();else measure(batched?"batchMutationUs":"singleMutationUs",operation);
    require(receipt.status);check(receipt.value.transaction_id==batch.transaction_id&&receipt.value.committed_lsn>0,"mutation receipt mismatch");
    check(mutation_io->syncs>syncs_before,"mutation commit did not synchronize durable files");
    for(size_t i=first;i<first+count;++i){expected[i]=value;validate_record(i);}
   }
   // No checkpoint on destruction: verification must recover the acknowledged WAL commits.
   reopen_mutations();
  }
  out["mutationProtocol"]={{"records",mutation_records},{"nodePropertyCount",1},{"scalarType","int64"},
    {"operation","upsert-existing-node"},{"durability","wal-sync"},{"database","dedicated"},
    {"samples",samples},{"warmup",warmup},{"validation","each-commit-and-reopen"}};
  out["mutationValidation"]={{"singleCommits",warmup+samples},{"batchCommits",warmup+samples},
    {"mutations",(warmup+samples)*(mutation_records+1)},{"reopenedRecords",mutation_records},{"reopenVerified",true}};
  out["mutationIO"]=mutation_io->snapshot();mutation_owner.reset();
  size_t edges=0;for(auto& v:data.edges)edges+=v.size();
  out["dataset"]={{"name",kind},{"nodes",n},{"edges",edges},{"seed",seed},{"nodeStringPayloadBytes",data.payload.size()},{"nodePropertyCount",2},{"edgePropertyCount",0}};
  out["peakRSSBytes"]=peak();out["sqliteVersion"]=sqlite3_libversion();
#if TARGET_OS_SIMULATOR
  out["platform"]="ios-simulator";
#else
  out["platform"]="macos";
#endif
  std::filesystem::remove_all(root);std::cout<<out.dump()<<'\n';return 0;
 }catch(const std::exception& ex){if(!root.empty()){std::error_code ec;std::filesystem::remove_all(root,ec);}std::cerr<<"Performance run failed in "<<phase<<" after "<<committed<<" committed mutations: "<<ex.what()<<'\n';return 1;}
}
