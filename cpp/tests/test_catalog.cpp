#include "TestSupport.hpp"
using namespace graphdb;
using namespace nova_test;
NOVA_TEST(catalog_contains_all_index_sections, "catalog", "") {
  EngineFixture f;
  CHECK(f.engine
            ->execute_dsl("create index on N(v); upsert node N a set v=1; upsert node N b; upsert "
                          "edge E a -> b")
            .ok);
  CHECK(f.engine->checkpoint().ok);
  auto saved = Catalog::load(f.dir.path() / "catalog.db");
  CHECK(saved);
  CHECK(saved.value.indexes.sections[0].size() == 1);
  CHECK(saved.value.indexes.sections[1].size() == 1);
  CHECK(!saved.value.indexes.sections[2].empty());
  CHECK(!saved.value.indexes.sections[3].empty());
}
NOVA_TEST(catalog_rejects_stale_postings_and_bad_sections, "catalog", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {}).ok);
  CHECK(f.engine->checkpoint().ok);
  auto path = f.dir.path() / "catalog.db";
  auto data = Catalog::load(path);
  CHECK(data);
  data.value.indexes.sections[0]["N"]["n/a"]++;
  CHECK(!Catalog::save(path, data.value).ok);
  PosixFileIO io;
  auto file = io.read(path, 64 * 1024 * 1024);
  CHECK(file);
  auto original = file.value;
  for (size_t cut = 0; cut < original.size(); ++cut) {
    std::vector<uint8_t> truncated(original.begin(), original.begin() + cut);
    CHECK(io.write_all(path, truncated, WriteMode::replace).ok);
    auto loaded = Catalog::load(path);
    CHECK(!loaded);
    auto unchanged = io.read(path, 64 * 1024 * 1024);
    CHECK(unchanged.value == truncated);
  }
  CHECK(io.write_all(path, original, WriteMode::replace).ok);
}

NOVA_TEST(retained_catalog_scans_large_payload_with_bounded_reads, "catalog", "") {
  class ScanIO:public PosixFileIO {
  public:
    bool forbid_whole=false;size_t largest=0;
    Result<std::vector<uint8_t>> read(const std::filesystem::path &p,size_t n) override {
      CHECK(!forbid_whole);return PosixFileIO::read(p,n);
    }
    ssize_t read_some(int fd,void *p,size_t n) override {largest=std::max(largest,n);return PosixFileIO::read_some(fd,p,n);}
  };
  TempDirectory dir;auto io=std::make_shared<ScanIO>();auto recovery=Recovery::open(dir.path(),io);CHECK(recovery);
  MemoryHotStore hot;GraphObject object;object.kind=ObjectKind::Node;object.id="large";object.label_or_type="N";object.version=1;
  object.properties={{"data",std::string(100000,'x')}};StoredRecord rec;rec.object=object;
  CatalogData data;data.records={rec};hot.load_records(data.records,{});data.indexes=hot.dump_indexes();
  CHECK(recovery.value->checkpoint(data).ok);
  auto descriptor=recovery.value->root().current;auto path=dir.path()/descriptor.filename;
  io->forbid_whole=true;io->largest=0;
  QueryOptions options;options.working_bytes=65536;QueryContext context(options);size_t cold=0;
  CHECK(scan_retained_catalog(*io,path,descriptor,recovery.value->root().database_id,context,[&](std::string_view){++cold;}).ok);
  CHECK(cold==0 && io->largest<=16384);
  auto wrong=descriptor;wrong.crc^=1;
  CHECK(!scan_retained_catalog(*io,path,wrong,recovery.value->root().database_id,context,[](std::string_view){}).ok);
  io->forbid_whole=false;auto bytes=io->read(path,64*1024*1024);CHECK(bytes);bytes.value[bytes.value.size()-5]^=1;
  CHECK(io->write_all(path,bytes.value,WriteMode::replace).ok);io->forbid_whole=true;
  CHECK(!scan_retained_catalog(*io,path,descriptor,recovery.value->root().database_id,context,[](std::string_view){}).ok);
}

#include "ModelFileIO.hpp"
NOVA_TEST(retained_catalog_rejects_identity_extent_io_and_cancellation_failures, "catalog", "") {
  TempDirectory dir;auto io=std::make_shared<ModelFileIO>();auto root=Recovery::open(dir.path(),io);CHECK(root);
  auto descriptor=root.value->root().current;auto path=dir.path()/descriptor.filename;
  auto original=io->read(path,64*1024*1024);CHECK(original);
  auto scan=[&](const CatalogDescriptor &d){QueryContext ctx;return scan_retained_catalog(*io,path,d,root.value->root().database_id,ctx,[](std::string_view){});};
  auto wrong=descriptor;wrong.generation++;CHECK(!scan(wrong).ok);
  wrong=descriptor;wrong.committed_lsn++;CHECK(!scan(wrong).ok);
  wrong=descriptor;wrong.bytes++;CHECK(!scan(wrong).ok);
  wrong=descriptor;wrong.version++;CHECK(scan(wrong).code==ErrorCode::unsupportedVersion);
  for(auto failure:{"openReader","read","closeReader"}){io->fail_next=failure;CHECK(scan(descriptor).code==ErrorCode::ioFailure);CHECK(scan(descriptor).ok);}
  for(size_t extent:{size_t(0),size_t(19),original.value.size()-1}){
    CHECK(io->write_all(path,std::vector<uint8_t>(original.value.begin(),original.value.begin()+extent),WriteMode::replace).ok);CHECK(!scan(descriptor).ok);}
  CHECK(io->write_all(path,original.value,WriteMode::replace).ok);
  QueryContext cancelled;cancelled.cancel();CHECK(scan_retained_catalog(*io,path,descriptor,root.value->root().database_id,cancelled,[](std::string_view){}).code==ErrorCode::cancelled);
  auto identity=root.value->root().database_id;identity[0]^=1;QueryContext context;
  CHECK(!scan_retained_catalog(*io,path,descriptor,identity,context,[](std::string_view){}).ok);
}
