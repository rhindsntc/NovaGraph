#include "TestSupport.hpp"
#include "ModelFileIO.hpp"
#include "ObservedHotStore.hpp"
using namespace graphdb;
using namespace nova_test;

namespace {
class TrimIO : public ModelFileIO {
public:
  size_t directory_syncs{0}, reads{0};
  Result<std::vector<uint8_t>> read(const std::filesystem::path &path, size_t maximum) override {
    ++reads; return ModelFileIO::read(path,maximum);
  }
  std::function<void(std::string_view)> on_fault_point;
  void fault_point(std::string_view name) override {
    if (on_fault_point)
      on_fault_point(name);
  }
  Status sync_directory(const std::filesystem::path &path) override {
    ++directory_syncs;
    return ModelFileIO::sync_directory(path);
  }
};
struct TrimFixture {
  TempDirectory dir;
  std::shared_ptr<ObservedHotStore> hot = std::make_shared<ObservedHotStore>();
  std::shared_ptr<TrimIO> io = std::make_shared<TrimIO>();
  std::shared_ptr<FileDiskStore> disk = std::make_shared<FileDiskStore>(dir.path()/"cold", io);
  EngineConfig config;
  TrimFixture(size_t count) {
    config.database_dir = dir.path();
    config.file_io = io;
    config.checkpoint_on_destroy = false;
    for (size_t i = 0; i < count; ++i) {
      GraphObject object;
      object.id = "n" + std::to_string(i);
      object.label_or_type = "N";
      object.properties = {{"v", std::string(100, 'x')}};
      CHECK(hot->put_hot(object_key(object.kind, object.id), object).ok);
    }
  }
};
}

NOVA_TEST(trim_accounting_work_does_not_rescan_per_record, "tiering", "") {
  TrimFixture f(130);
  const auto initial = f.hot->memory_usage().hot_payload_bytes;
  f.hot->accounting_scans = 0;
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  auto trimmed = engine.trim_memory(0, context);
  CHECK(trimmed);
  CHECK(trimmed.value.evicted_count == 130);
  CHECK(trimmed.value.evicted_bytes == initial);
  CHECK(trimmed.value.unmet_bytes == 0);
  // A fixed number of complete graph/index scans, independent of evictions.
  CHECK(f.hot->accounting_scans <= 2);
  CHECK(f.hot->store.memory_usage().hot_payload_bytes == 0);
}

NOVA_TEST(trim_groups_durable_payloads_before_releasing_memory, "tiering", "") {
  TrimFixture f(130);
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  auto trimmed = engine.trim_memory(0, context);
  CHECK(trimmed);
  CHECK(trimmed.value.evicted_count == 130);
  // 64-record durability groups replace a directory sync for every record.
  CHECK(f.io->directory_syncs == 3);
  f.io->power_loss();
  for (const auto &record : f.hot->dump_records()) {
    CHECK(record.tier == StorageTier::Cold);
    auto read = read_manifest(*f.disk, record);
    CHECK(read);
    CHECK(std::get<std::string>(read.value.properties.at("v")) == std::string(100, 'x'));
  }
}

NOVA_TEST(trim_group_failure_preserves_hot_records_and_completed_progress, "tiering", "") {
  for (const auto &fault : {"write", "sync", "directorySync"}) {
    TrimFixture f(70);
    const auto initial = f.hot->memory_usage().hot_payload_bytes;
    GraphEngine engine(f.hot, f.disk, f.config);
    QueryContext context;
    f.io->fail_next = fault;
    auto failed = engine.trim_memory(0, context);
    CHECK(!failed);
    CHECK(failed.status.code == ErrorCode::ioFailure);
    CHECK(failed.value.evicted_count == 0);
    CHECK(f.hot->hot_count() == 70);
    CHECK(f.hot->memory_usage().hot_payload_bytes == initial);
    f.io->power_loss();
    for (const auto &record : f.hot->dump_records())
      CHECK(std::get<std::string>(record.object.properties.at("v")) == std::string(100, 'x'));
  }
  TrimFixture f(70);
  const auto initial = f.hot->memory_usage().hot_payload_bytes;
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  f.io->on_fault_point = [&](std::string_view name) {
    if (name == "payload.group.after_directory_sync")
      f.io->fail_next = "write"; // Fail the next group, after 64 durable evictions.
  };
  auto failed = engine.trim_memory(0, context);
  CHECK(!failed);
  CHECK(failed.value.evicted_count == 64);
  CHECK(f.hot->hot_count() == 6);
  CHECK(failed.value.evicted_bytes == initial - f.hot->memory_usage().hot_payload_bytes);
  f.io->power_loss();
  for (const auto &record : f.hot->dump_records())
    if (record.tier == StorageTier::Cold)
      CHECK(read_manifest(*f.disk, record));
}

NOVA_TEST(trim_target_and_stale_candidate_accounting_remain_exact, "tiering", "") {
  TrimFixture f(6);
  const auto initial = f.hot->memory_usage().hot_payload_bytes;
  const auto one = f.hot->get("n/n0")->payload_bytes;
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  auto no_work = engine.trim_memory(initial, context);
  CHECK(no_work && no_work.value.evicted_count == 0);
  CHECK(f.io->directory_syncs == 0);
  auto one_record = engine.trim_memory(initial - one, context);
  CHECK(one_record && one_record.value.evicted_count == 1);
  CHECK(one_record.value.evicted_bytes == one);
  CHECK(one_record.value.unmet_bytes == 0);
  auto candidate = f.hot->cold_candidates(INT64_MAX, 1).front();
  auto key = object_key(candidate.object.kind, candidate.object.id);
  f.io->on_fault_point = [&](std::string_view name) {
    if (name == "payload.group.after_directory_sync")
      CHECK(f.hot->touch_read(key, 1, 1).ok); // Reject its stale activity version.
  };
  auto rest = engine.trim_memory(0, context);
  CHECK(rest && rest.value.evicted_count == 4);
  CHECK(rest.value.evicted_bytes == one * 4);
  CHECK(rest.value.unmet_bytes == one);
  CHECK(f.hot->memory_usage().hot_payload_bytes == one);
}

NOVA_TEST(trim_cancellation_before_group_publication_keeps_hot_data, "tiering", "") {
  for (auto point : {"payload.after_write", "payload.after_sync", "payload.group.after_directory_sync"}) {
    TrimFixture f(70);
    GraphEngine engine(f.hot, f.disk, f.config);
    QueryContext context;
    f.io->on_fault_point = [&](std::string_view name) {
      if (name == point)
        context.cancel();
    };
    auto cancelled = engine.trim_memory(0, context);
    CHECK(!cancelled);
    CHECK(cancelled.status.code == ErrorCode::cancelled);
    CHECK(cancelled.value.evicted_count == 0);
    CHECK(f.hot->hot_count() == 70);
  }
}

NOVA_TEST(trim_cancellation_after_publication_reports_only_durable_progress, "tiering", "") {
  TrimFixture f(70);
  const auto initial = f.hot->memory_usage().hot_payload_bytes;
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  size_t published = 0;
  f.io->on_fault_point = [&](std::string_view name) {
    if (name == "payload.after_residency_switch" && ++published == 3)
      context.cancel();
  };
  auto cancelled = engine.trim_memory(0, context);
  CHECK(!cancelled && cancelled.status.code == ErrorCode::cancelled);
  CHECK(cancelled.value.evicted_count == 3);
  CHECK(f.hot->hot_count() == 67);
  CHECK(cancelled.value.evicted_bytes == initial - f.hot->memory_usage().hot_payload_bytes);
  f.io->power_loss();
  for (const auto &record : f.hot->dump_records()) {
    if (record.tier == StorageTier::Cold)
      CHECK(read_manifest(*f.disk, record));
    else
      CHECK(std::get<std::string>(record.object.properties.at("v")) == std::string(100, 'x'));
  }
}
NOVA_TEST(forced_trim_obeys_target_and_pins_without_ttl, "tiering", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {{"v", std::string(1000, 'x')}}).ok);
  CHECK(f.engine->upsert_node("N", "b", {{"v", std::string(1000, 'y')}}).ok);
  auto pin = f.engine->pin_node("a");
  CHECK(pin);
  QueryContext context;
  auto trimmed = f.engine->trim_memory(0, context);
  CHECK(trimmed);
  CHECK(trimmed.value.evicted_count == 1);
  CHECK(trimmed.value.evicted_bytes > 0);
  CHECK(trimmed.value.pinned_bytes > 0);
  CHECK(trimmed.value.unmet_bytes > 0);
  pin.value.reset();
  auto rest = f.engine->trim_memory(0, context);
  CHECK(rest);
  CHECK(rest.value.evicted_count == 1);
  CHECK(rest.value.unmet_bytes == 0);
  CHECK(f.engine->get_node("a"));
}
NOVA_TEST(monotonic_inactivity_ignores_wall_jumps_and_resets_on_open, "tiering", "") {
  EngineFixture f;
  f.engine.reset();
  int64_t wall = 100000, mono = 100;
  f.config.wall_clock = [&] { return wall; };
  f.config.monotonic_clock = [&] { return mono; };
  f.config.hot_ttl_ms = 10;
  f.config.checkpoint_on_destroy = false;
  f.engine = std::make_unique<GraphEngine>(f.hot, f.disk, f.config);
  CHECK(f.engine->upsert_node("N", "a", {}).ok);
  wall += 1000000;
  CHECK(f.engine->sweep_once() == 0);
  wall = -10000;
  mono = 111;
  CHECK(f.engine->sweep_once() == 1);
  CHECK(f.engine->get_node("a"));
  CHECK(f.engine->checkpoint().ok);
  f.engine.reset();
  mono = 1;
  f.hot = std::make_shared<MemoryHotStore>();
  f.engine = std::make_unique<GraphEngine>(f.hot, f.disk, f.config);
  CHECK(f.engine->recover().ok);
  CHECK(f.engine->sweep_once() == 0);
  CHECK(f.engine->get_node("a").value.last_modified_ms == 100000);
}
NOVA_TEST(memory_reservations_reject_writes_and_results_before_publication, "tiering", "") {
  EngineFixture f;
  f.engine.reset();
  f.config.memory.metadata_bytes = 1024;
  f.config.memory.hot_payload_bytes = 1024;
  f.config.memory.result_bytes = 128;
  f.config.checkpoint_on_destroy = false;
  f.engine = std::make_unique<GraphEngine>(f.hot, f.disk, f.config);
  auto result = f.engine->upsert_node("N", "a", {{"v", std::string(2000, 'x')}});
  CHECK(result.code == ErrorCode::limitExceeded);
  CHECK(!f.hot->get("n/a"));
}
NOVA_TEST(result_budget_and_failed_trim_return_explicit_errors, "tiering", "") {
  EngineFixture f;
  CHECK(f.engine->upsert_node("N", "a", {{"v", std::string(500, 'x')}}).ok);
  auto config = f.config;
  config.checkpoint_on_destroy = false;
  config.memory.result_bytes = 128;
  GraphEngine limited(f.hot, f.disk, config);
  CHECK(limited.get_node("a").status.code == ErrorCode::limitExceeded);
  GraphEngine broken(f.hot, std::make_shared<FailingDiskStore>(), config);
  QueryContext context;
  auto trim = broken.trim_memory(0, context);
  CHECK(!trim);
  CHECK(f.hot->get("n/a")->tier == StorageTier::Hot);
  CHECK(f.engine->get_node("a"));
}

NOVA_TEST(trim_large_graph_uses_bounded_workspace, "tiering", "") {
  TrimFixture f(1000);
  f.config.memory.query_bytes = 128 * 1024;
  auto usage = f.hot->memory_usage();
  CHECK(usage.metadata_bytes + usage.hot_payload_bytes > f.config.memory.query_bytes);
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryOptions options; options.working_bytes = 128 * 1024;
  QueryContext context(options);
  auto result = engine.trim_memory(0, context);
  CHECK(result);
  CHECK(result.value.evicted_count == 1000);
  CHECK(result.value.evicted_bytes == usage.hot_payload_bytes);
  CHECK(result.value.unmet_bytes == 0);
  CHECK(context.working_available() == options.working_bytes);
}
NOVA_TEST(trim_low_budget_rejects_before_write, "tiering", "") {
  TrimFixture f(1);
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryOptions options; options.working_bytes = 1;
  QueryContext context(options);
  auto result = engine.trim_memory(0, context);
  CHECK(!result && result.status.code == ErrorCode::limitExceeded);
  CHECK(f.hot->hot_count() == 1);
  CHECK(f.io->directory_syncs == 0);
  CHECK(context.working_available() == 1);
}
NOVA_TEST(trim_zero_batch_selects_nothing, "tiering", "") {
  TrimFixture f(3); f.config.sweep_batch_size = 0;
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  auto result = engine.trim_memory(0, context);
  CHECK(result && result.value.evicted_count == 0);
  CHECK(f.hot->hot_count() == 3);
  CHECK(f.io->directory_syncs == 0);
}
NOVA_TEST(trim_metadata_limit_preserves_hot_records, "tiering", "") {
  TrimFixture f(3);
  f.config.memory.metadata_bytes = f.hot->memory_usage().metadata_bytes;
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  auto result = engine.trim_memory(0, context);
  CHECK(!result && result.status.code == ErrorCode::limitExceeded);
  CHECK(result.value.evicted_count == 0);
  CHECK(f.hot->hot_count() == 3);
}
NOVA_TEST(trim_thrown_failure_retains_completed_progress, "tiering", "") {
  TrimFixture f(70);
  GraphEngine engine(f.hot, f.disk, f.config);
  QueryContext context;
  size_t published = 0;
  f.io->on_fault_point = [&](std::string_view point) {
    if (point == "payload.after_residency_switch" && ++published == 64)
      throw QueryFailure(Status::Error("injected limit", ErrorCode::limitExceeded));
  };
  auto result = engine.trim_memory(0, context);
  CHECK(!result && result.status.code == ErrorCode::limitExceeded);
  CHECK(result.value.evicted_count == 64);
  CHECK(f.hot->hot_count() == 6);
}

NOVA_TEST(trim_adapts_to_held_and_native_workspace_limits, "tiering", "") {
  for (bool native : {false, true}) {
    TrimFixture f(130);
    if (native) f.config.memory.query_bytes = 64*1024;
    GraphEngine engine(f.hot,f.disk,f.config);
    QueryOptions options; options.working_bytes = 128*1024;
    QueryContext context(options);
    auto held = context.hold_work(native?0:64*1024);
    auto result = engine.trim_memory(0,context);
    CHECK(result && result.value.evicted_count == 130);
    CHECK(f.io->directory_syncs > 3);
    CHECK(context.working_available() == 64*1024);
  }
}
NOVA_TEST(trim_oversized_next_candidate_preserves_prefix, "tiering", "") {
  TrimFixture f(1);
  GraphObject object; object.id="zz"; object.label_or_type="N";
  object.properties={{"large",std::string(8192,'z')}};
  CHECK(f.hot->put_hot("n/zz",object).ok);
  GraphEngine engine(f.hot,f.disk,f.config);
  QueryOptions options; options.working_bytes=128*1024;
  QueryContext context(options);
  auto result=engine.trim_memory(0,context);
  CHECK(!result && result.status.code==ErrorCode::limitExceeded);
  CHECK(result.value.evicted_count==1);
  CHECK(f.hot->get("n/n0")->tier==StorageTier::Cold);
  CHECK(f.hot->get("n/zz")->tier==StorageTier::Hot);
  CHECK(f.io->directory_syncs==1);
  CHECK(context.working_available()==options.working_bytes);
  QueryContext retry;
  CHECK(engine.trim_memory(0,retry));
  CHECK(f.hot->hot_count()==0);
}
NOVA_TEST(trim_metadata_limit_retains_completed_group, "tiering", "") {
  TrimFixture probe(1);
  auto before=probe.hot->memory_usage().metadata_bytes;
  GraphEngine probe_engine(probe.hot,probe.disk,probe.config);
  QueryContext pc; CHECK(probe_engine.trim_memory(0,pc));
  auto delta=probe.hot->memory_usage().metadata_bytes-before;
  CHECK(delta>0);
  TrimFixture f(70);
  auto initial=f.hot->memory_usage();
  f.config.memory.metadata_bytes=initial.metadata_bytes+64*delta;
  GraphEngine engine(f.hot,f.disk,f.config);
  QueryContext context;
  auto result=engine.trim_memory(0,context);
  CHECK(!result && result.status.code==ErrorCode::limitExceeded);
  CHECK(result.value.evicted_count==64);
  CHECK(result.value.evicted_bytes==initial.hot_payload_bytes-f.hot->memory_usage().hot_payload_bytes);
  CHECK(f.hot->memory_usage().metadata_bytes==f.config.memory.metadata_bytes);
  CHECK(context.working_available()==context.options().working_bytes);
}
NOVA_TEST(trim_long_identity_properties_and_bounded_pages_reopen, "tiering", "") {
  EngineFixture f;
  GraphObject object; object.id=std::string(1024,'i'); object.label_or_type=std::string(500,'L');
  for(int i=0;i<100;++i)object.properties["property"+std::to_string(i)]=std::string(50,'v');
  CHECK(f.engine->upsert_node(object.label_or_type,object.id,object.properties).ok);
  QueryContext context;
  auto trim=f.engine->trim_memory(0,context);
  CHECK(trim && trim.value.evicted_count==1);
  CHECK(context.working_available()==context.options().working_bytes);
  CHECK(f.engine->checkpoint().ok);
  f.engine.reset(); f.hot=std::make_shared<MemoryHotStore>();
  f.engine=std::make_unique<GraphEngine>(f.hot,f.disk,f.config);
  CHECK(f.engine->recover().ok);
  auto read=f.engine->get_node(object.id);
  CHECK(read && read.value.properties==object.properties);
  TrimFixture pages(17); pages.config.sweep_batch_size=3;
  GraphEngine paged(pages.hot,pages.disk,pages.config);
  size_t count=0;
  while(pages.hot->hot_count()) {
    QueryContext page;
    auto result=paged.trim_memory(0,page);
    CHECK(result && result.value.evicted_count>0 && result.value.evicted_count<=3);
    count+=result.value.evicted_count;
  }
  CHECK(count==17);
}
NOVA_TEST(trim_rechecks_pin_and_version_after_group_sync, "tiering", "") {
  for(bool pin_record : {true,false}) {
    TrimFixture f(2);
    GraphEngine engine(f.hot,f.disk,f.config);
    std::shared_ptr<void> pin;
    f.io->on_fault_point=[&](std::string_view point) {
      if(point!="payload.group.after_directory_sync")return;
      if(pin_record)pin=f.hot->pin("n/n0");
      else { auto record=f.hot->get("n/n0");++record->object.version;CHECK(f.hot->put_hot("n/n0",record->object).ok); }
    };
    QueryContext context;
    auto result=engine.trim_memory(0,context);
    CHECK(result && result.value.evicted_count==1);
    CHECK(f.hot->get("n/n0")->tier==StorageTier::Hot);
    CHECK(result.value.unmet_bytes==f.hot->get("n/n0")->payload_bytes);
    CHECK(result.value.pinned_bytes==(pin_record?result.value.unmet_bytes:0));
  }
}
NOVA_TEST(trim_unsupported_disk_backend_rejects_without_writes, "tiering", "") {
  class LegacyDisk : public FailingDiskStore {
  public:
    size_t writes=0;
    Result<std::string> write(const std::string &,const GraphObject &) override {++writes;return {Status::OK(),"unexpected"};}
  };
  TrimFixture f(1);auto legacy=std::make_shared<LegacyDisk>();
  GraphEngine engine(f.hot,legacy,f.config);QueryContext context;
  auto result=engine.trim_memory(0,context);
  CHECK(!result && result.status.code==ErrorCode::limitExceeded);
  CHECK(legacy->writes==0 && f.hot->hot_count()==1);
  CHECK(context.working_available()==context.options().working_bytes);
}

NOVA_TEST(trim_descriptors_use_written_bytes_without_rereads, "tiering", "") {
  TrimFixture f(130);
  GraphEngine engine(f.hot,f.disk,f.config);QueryContext context;
  auto result=engine.trim_memory(0,context);
  CHECK(result && result.value.evicted_count==130);
  CHECK(f.io->reads==0);
  f.io->power_loss();
  for(const auto &record:f.hot->dump_records())CHECK(read_manifest(*f.disk,record));
  CHECK(f.io->reads==130);
}
NOVA_TEST(trim_later_group_sync_errors_preserve_durable_prefix, "tiering", "") {
  for(auto fault:{"sync","directorySync"}) {
    TrimFixture f(70);
    GraphEngine engine(f.hot,f.disk,f.config);QueryContext context;
    size_t published=0;
    f.io->on_fault_point=[&](std::string_view point) {
      if(point=="payload.after_residency_switch" && ++published==64)f.io->fail_next=fault;
    };
    auto result=engine.trim_memory(0,context);
    CHECK(!result && result.status.code==ErrorCode::ioFailure);
    CHECK(result.value.evicted_count==64 && f.hot->hot_count()==6);
    CHECK(context.working_available()==context.options().working_bytes);
    f.io->power_loss();
    for(const auto &record:f.hot->dump_records())
      if(record.tier==StorageTier::Cold)CHECK(read_manifest(*f.disk,record));
    f.io->on_fault_point={};
    QueryContext retry;auto done=engine.trim_memory(0,retry);
    CHECK(done && done.value.evicted_count==6);
  }
}

NOVA_TEST(trim_retries_smaller_group_for_large_first_candidate, "tiering", "") {
  for(size_t cap:{size_t(1),size_t(4096)}) {
    TrimFixture f(0);f.config.sweep_batch_size=cap;
    GraphObject object;object.id="x";object.label_or_type="N";
    object.properties={{"v",std::string(1700,'x')}};
    CHECK(f.hot->put_hot("n/x",object).ok);
    GraphEngine engine(f.hot,f.disk,f.config);
    QueryOptions options;options.working_bytes=128*1024;
    QueryContext context(options);auto result=engine.trim_memory(0,context);
    CHECK(result && result.value.evicted_count==1);
    CHECK(context.working_available()==options.working_bytes);
  }
}
