#include "TestSupport.hpp"
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(gc_retains_previous_catalog_payloads_then_reclaims_orphans, "gc", "") {
  TempDirectory dir;
  auto root = Recovery::open(dir.path());
  CHECK(root);
  EngineConfig config;
  config.database_dir = dir.path();
  config.checkpoint_on_destroy = false;
  auto hot = std::make_shared<MemoryHotStore>();
  auto disk = std::make_shared<FileDiskStore>(dir.path() / "cold");
  GraphEngine engine(hot, disk, config, nullptr, root.value);
  CHECK(engine.recover().ok);
  QueryContext ctx;
  CHECK(engine.upsert_node("N", "a", {{"v", int64_t(1)}}).ok);
  CHECK(engine.trim_memory(0, ctx));
  auto payload = hot->get("n/a")->disk_key;
  CHECK(engine.checkpoint().ok);
  CHECK(engine.get_node("a"));
  CHECK(engine.delete_node("a").ok);
  CHECK(engine.checkpoint().ok);
  CHECK(engine.collect_garbage(100, ctx));
  CHECK(std::filesystem::exists(dir.path() / "cold" / payload));
  CHECK(engine.checkpoint().ok);
  CHECK(engine.collect_garbage(100, ctx));
  CHECK(!std::filesystem::exists(dir.path() / "cold" / payload));
  PosixFileIO io;
  CHECK(io.write_all(dir.path() / "cold" / "external.txt", {1}, WriteMode::exclusive).ok);
  CHECK(engine.collect_garbage(100, ctx));
  CHECK(std::filesystem::exists(dir.path() / "cold" / "external.txt"));
}
NOVA_TEST(cancelled_gc_removes_nothing, "gc", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {}).ok);
  f.cold("n/a");
  auto file = f.hot->get("n/a")->disk_key;
  QueryContext context;
  context.cancel();
  CHECK(!f.engine->collect_garbage(100, context));
  CHECK(std::filesystem::exists(f.dir.path() / "cold" / file));
}
NOVA_TEST(repeated_promote_delete_cycles_have_bounded_disk_usage, "gc", "") {
  EngineFixture f;
  QueryContext ctx;
  for (int i = 0; i < 15; ++i) {
    CHECK(f.engine->upsert_node("N", "a", {{"v", int64_t(i)}}).ok);
    CHECK(f.engine->trim_memory(0, ctx));
    CHECK(f.engine->get_node("a"));
    CHECK(f.engine->delete_node("a").ok);
    CHECK(f.engine->collect_garbage(4, ctx));
  }
  for (int i = 0; i < 20; ++i)
    CHECK(f.engine->collect_garbage(4, ctx));
  CHECK(std::filesystem::is_empty(f.dir.path() / "cold"));
}

#include "ModelFileIO.hpp"
NOVA_TEST(gc_one_candidate_on_large_graph_fits_sixty_four_kib, "gc", "") {
  TempDirectory dir;auto io=std::make_shared<ModelFileIO>();auto root=Recovery::open(dir.path(),io);CHECK(root);
  EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.checkpoint_on_destroy=false;config.memory.query_bytes=65536;
  auto hot=std::make_shared<MemoryHotStore>();GraphEngine engine(hot,std::make_shared<FileDiskStore>(dir.path()/"cold",io),config,nullptr,root.value);
  CHECK(engine.recover().ok);
  for(int i=0;i<1000;++i)CHECK(engine.upsert_node("N","node:"+std::to_string(i),{}).ok);
  auto orphan=dir.path()/"cold"/("payload-"+std::string(32,'0')+".rec");CHECK(io->write_all(orphan,{1},WriteMode::exclusive).ok);
  QueryOptions options;options.working_bytes=65536;QueryContext context(options);
  auto collected=engine.collect_garbage(1,context);CHECK(collected);CHECK(collected.value.removed==1);
  CHECK(!io->read(orphan,100));
}

NOVA_TEST(gc_large_retained_catalogs_fit_small_workspace_and_fail_before_deletion, "gc", "") {
  class InspectIO:public ModelFileIO {
  public:
    bool forbid_whole=false,cancel_after_remove=false;size_t lists=0,removes=0,syncs=0;
    QueryContext *context=nullptr;
    Result<std::vector<uint8_t>> read(const std::filesystem::path &p,size_t n) override {CHECK(!forbid_whole);return ModelFileIO::read(p,n);}
    Result<std::vector<std::string>> list(const std::filesystem::path &p,size_t offset,size_t limit) override {++lists;return ModelFileIO::list(p,offset,limit);}
    Status remove(const std::filesystem::path &p) override {++removes;auto s=ModelFileIO::remove(p);if(cancel_after_remove && context)context->cancel();return s;}
    Status sync_directory(const std::filesystem::path &p) override {++syncs;return ModelFileIO::sync_directory(p);}
  };
  TempDirectory dir;auto io=std::make_shared<InspectIO>();auto root=Recovery::open(dir.path(),io);CHECK(root);
  auto cold_name="payload-"+std::string(32,'1')+".rec";
  CatalogData data;StoredRecord large;large.object.kind=ObjectKind::Node;large.object.id="large";large.object.label_or_type="N";large.object.version=1;
  large.object.properties={{"data",std::string(100000,'x')}};
  StoredRecord cold;cold.object.kind=ObjectKind::Node;cold.object.id="old";cold.object.label_or_type="N";cold.object.version=1;cold.tier=StorageTier::Cold;cold.disk_key=cold_name;cold.payload_bytes=100;
  CHECK(io->write_all(dir.path()/"cold"/cold_name,{1},WriteMode::exclusive).ok);
  data.records={large,cold};MemoryHotStore index;index.load_records(data.records,{});data.indexes=index.dump_indexes();CHECK(root.value->checkpoint(data).ok);
  data.records={large};index.load_records(data.records,{});data.indexes=index.dump_indexes();CHECK(root.value->checkpoint(data).ok);
  auto hot=std::make_shared<MemoryHotStore>();EngineConfig config;config.database_dir=dir.path();config.file_io=io;config.memory.query_bytes=65536;config.checkpoint_on_destroy=false;
  GraphEngine engine(hot,std::make_shared<FileDiskStore>(dir.path()/"cold",io),config,nullptr,root.value);
  CHECK(engine.recover().ok);
  auto orphan="payload-"+std::string(32,'0')+".rec";
  CHECK(io->write_all(dir.path()/"cold"/orphan,{1},WriteMode::exclusive).ok);
  CHECK(io->write_all(dir.path()/"cold"/cold_name,{1},WriteMode::replace).ok);
  auto previous=dir.path()/root.value->root().previous->filename;
  auto bytes=io->read(previous,64*1024*1024);CHECK(bytes);auto corrupt=bytes.value;corrupt[corrupt.size()-5]^=1;
  CHECK(io->write_all(previous,corrupt,WriteMode::replace).ok);io->forbid_whole=true;
  QueryOptions options;options.working_bytes=65536;
  {QueryContext ctx(options);auto result=engine.collect_garbage(10,ctx);CHECK(!result);CHECK(io->removes==0);}
  CHECK(io->write_all(previous,bytes.value,WriteMode::replace).ok);
  io->fail_next="closeReader";
  {QueryContext ctx(options);CHECK(!engine.collect_garbage(10,ctx));CHECK(io->removes==0);}
  {QueryContext ctx(options);auto result=engine.collect_garbage(10,ctx);CHECK(result);CHECK(result.value.removed>=1);}
  io->forbid_whole=false;CHECK(!io->read(dir.path()/"cold"/orphan,10));CHECK(io->read(dir.path()/"cold"/cold_name,10));
  size_t lists=io->lists;QueryOptions tiny;tiny.working_bytes=1;QueryContext insufficient(tiny);
  CHECK(engine.collect_garbage(0,insufficient));CHECK(io->lists==lists);
  CHECK(!engine.collect_garbage(1,insufficient));CHECK(io->lists==lists);
  CHECK(io->write_all(dir.path()/"cold"/orphan,{1},WriteMode::exclusive).ok);
  QueryContext cancelled(options);io->cancel_after_remove=true;io->context=&cancelled;auto syncs=io->syncs;
  auto cancelled_result=engine.collect_garbage(10,cancelled);CHECK(!cancelled_result);
  CHECK(cancelled_result.status.code==ErrorCode::cancelled && cancelled_result.value.removed==1);CHECK(io->syncs>syncs);
}
NOVA_TEST(gc_small_pages_converge_without_losing_live_or_external_files, "gc", "") {
  EngineFixture f;
  for(int i=0;i<40;++i){std::ostringstream name;name<<"payload-"<<std::setfill('0')<<std::setw(32)<<i<<".rec";
    CHECK(f.config.file_io->write_all(f.dir.path()/"cold"/name.str(),{1},WriteMode::exclusive).ok);}
  CHECK(f.config.file_io->write_all(f.dir.path()/"cold"/"external",{1},WriteMode::exclusive).ok);
  CHECK(f.engine->upsert_node("N","live",{}).ok);f.cold("n/live");auto live=f.hot->get("n/live")->disk_key;
  for(int i=0;i<200;++i){QueryOptions options;options.working_bytes=65536;QueryContext ctx(options);CHECK(f.engine->collect_garbage(1,ctx));}
  auto names=f.config.file_io->list(f.dir.path()/"cold");CHECK(names && names.value.size()==2);
  CHECK(std::filesystem::exists(f.dir.path()/"cold"/live));CHECK(std::filesystem::exists(f.dir.path()/"cold"/"external"));
}
