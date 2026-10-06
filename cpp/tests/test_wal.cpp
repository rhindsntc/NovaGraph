#include "ModelFileIO.hpp"
#include "TestSupport.hpp"
#include "graphdb/Transaction.hpp"
using namespace graphdb;
using namespace nova_test;
static TransactionBatch sample_batch() {
  return {new_transaction_id(),
          {UpsertNode{"N", "a", {{"v", int64_t(1)}}}, UpsertNode{"N", "b", {{"v", int64_t(2)}}}}};
}
NOVA_TEST(wal_replays_complete_transaction_groups, "wal", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager wal(path, io);
  CHECK(wal.open().ok);
  auto batch = sample_batch();
  auto receipt = wal.append_transaction(batch);
  CHECK(receipt);
  CHECK(receipt.value.transaction_id == batch.transaction_id);
  CHECK(receipt.value.committed_lsn == 4);
  auto groups = wal.recover_transactions();
  CHECK(groups);
  CHECK(groups.value.size() == 1);
  CHECK(groups.value[0].batch.mutations.size() == 2);
  CHECK(groups.value[0].committed_lsn == 4);
}
NOVA_TEST(wal_transaction_every_byte_boundary, "wal", "") {
  TempDirectory dir;
  auto source = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager writer(path, source);
  CHECK(writer.open().ok);
  CHECK(writer.append_transaction(sample_batch()));
  auto prefix = source->read(path, 10000).value;
  auto next = sample_batch();
  CHECK(writer.append_transaction(next));
  auto complete = source->read(path, 10000).value;
  for (size_t cut = prefix.size(); cut <= complete.size(); ++cut) {
    auto io = std::make_shared<ModelFileIO>();
    CHECK(io->write_all(path, {complete.begin(), complete.begin() + cut}, WriteMode::replace).ok);
    WalManager reader(path, io);
    CHECK(reader.open().ok);
    auto groups = reader.recover_transactions();
    CHECK(groups);
    CHECK(groups.value.size() == (cut == complete.size() ? 2 : 1));
    CHECK(reader.repair_tail().ok);
    CHECK(reader.append_transaction(sample_batch()));
    auto after = reader.recover_transactions();
    CHECK(after);
    CHECK(after.value.size() == (cut == complete.size() ? 3 : 2));
  }
}
NOVA_TEST(wal_bad_complete_frame_is_corruption_and_unchanged, "wal", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager writer(path, io);
  CHECK(writer.open().ok);
  CHECK(writer.append_transaction(sample_batch()));
  auto bytes = io->read(path, 10000).value;
  bytes.back() ^= 1;
  CHECK(io->write_all(path, bytes, WriteMode::replace).ok);
  WalManager reader(path, io);
  CHECK(reader.open().ok);
  auto recovered = reader.recover_transactions();
  CHECK(!recovered);
  CHECK(recovered.status.code == ErrorCode::corruptData);
  CHECK(io->read(path, 10000).value == bytes);
  CHECK(!reader.writable());
}
NOVA_TEST(wal_sync_failure_returns_transaction_identity_and_fences, "wal", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  WalManager wal(dir.path() / "wal", io);
  CHECK(wal.open().ok);
  auto batch = sample_batch();
  io->fail_next = "sync";
  auto receipt = wal.append_transaction(batch);
  CHECK(!receipt);
  CHECK(receipt.status.code == ErrorCode::commitOutcomeUnknown);
  CHECK(receipt.status.context.transaction_id == batch.transaction_id);
  CHECK(!wal.writable());
  io->power_loss();
  WalManager reopened(dir.path() / "wal", io);
  CHECK(reopened.open().ok);
  auto recovered = reopened.recover_transactions();
  CHECK(recovered);
  CHECK(recovered.value.empty());
}
NOVA_TEST(wal_rejects_empty_and_oversized_batches_before_writing, "wal", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager wal(path, io);
  CHECK(wal.open().ok);
  auto before = io->read(path, 10000).value;
  TransactionBatch empty{new_transaction_id(), {}};
  CHECK(!wal.append_transaction(empty));
  auto many = sample_batch();
  many.mutations.resize(1001, DeleteNode{"a"});
  CHECK(!wal.append_transaction(many));
  CHECK(io->read(path, 10000).value == before);
  CHECK(wal.writable());
}
NOVA_TEST(wal_large_batch_frame_boundaries_and_rotation, "wal", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager wal(path, io);
  CHECK(wal.open().ok);
  TransactionBatch batch{new_transaction_id(), {}};
  for (int i = 0; i < 100; ++i)
    batch.mutations.push_back(UpsertNode{"N", std::to_string(i), {}});
  auto receipt = wal.append_transaction(batch);
  CHECK(receipt);
  auto bytes = io->read(path, 100000).value;
  ByteReader frames(bytes.data(), bytes.size());
  frames.skip(WalManager::header_bytes);
  while (frames.remaining()) {
    uint32_t size;
    CHECK(frames.read_u32(size));
    CHECK(frames.skip(size + 4));
    auto cut = frames.position();
    auto target = std::make_shared<ModelFileIO>();
    CHECK(target->write_all(path, {bytes.begin(), bytes.begin() + cut}, WriteMode::replace).ok);
    WalManager reader(path, target);
    CHECK(reader.open().ok);
    auto groups = reader.recover_transactions();
    CHECK(groups);
    CHECK(groups.value.size() == (cut == bytes.size() ? 1 : 0));
    CHECK(reader.repair_tail().ok);
    CHECK(reader.append_transaction(sample_batch()));
  }
  CHECK(wal.truncate_after_checkpoint(receipt.value.committed_lsn).ok);
  auto next = wal.append_transaction(sample_batch());
  CHECK(next);
  CHECK(next.value.committed_lsn == receipt.value.committed_lsn + 4);
}
NOVA_TEST(wal_header_and_structural_corruption_do_not_repair, "wal", "") {
  TempDirectory dir;
  auto source = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager wal(path, source);
  CHECK(wal.open().ok);
  CHECK(wal.append_transaction(sample_batch()));
  auto bytes = source->read(path, 10000).value;
  for (size_t cut = 0; cut < WalManager::header_bytes; ++cut) {
    auto io = std::make_shared<ModelFileIO>();
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + cut);
    CHECK(io->write_all(path, truncated, WriteMode::replace).ok);
    WalManager reader(path, io);
    CHECK(!reader.open().ok);
    CHECK(io->read(path, 10000).value == truncated);
  }
  for (size_t offset : {size_t(4), size_t(12), size_t(29)}) {
    auto changed = bytes;
    size_t begin = WalManager::header_bytes;
    changed[begin + offset] = 255;
    ByteReader length(changed.data() + begin, 4);
    uint32_t size;
    CHECK(length.read_u32(size));
    auto crc = crc32_compute(changed.data() + begin, 4 + size);
    for (int i = 0; i < 4; ++i)
      changed[begin + 4 + size + i] = uint8_t(crc >> (8 * i));
    auto io = std::make_shared<ModelFileIO>();
    CHECK(io->write_all(path, changed, WriteMode::replace).ok);
    WalManager reader(path, io);
    CHECK(reader.open().ok);
    CHECK(reader.recover_transactions().status.code == ErrorCode::corruptData);
    CHECK(io->read(path, 10000).value == changed);
  }
}
NOVA_TEST(wal_tail_repair_sync_failure_fences_append, "wal", "") {
  TempDirectory dir;
  auto io = std::make_shared<ModelFileIO>();
  auto path = dir.path() / "wal";
  WalManager wal(path, io);
  CHECK(wal.open().ok);
  CHECK(wal.append_transaction(sample_batch()));
  auto prefix = io->read(path, 10000).value;
  CHECK(io->write_all(path, {1, 2}, WriteMode::append).ok);
  WalManager reader(path, io);
  CHECK(reader.open().ok);
  CHECK(reader.recover_transactions());
  CHECK(!reader.writable());
  io->fail_next = "sync";
  CHECK(!reader.repair_tail().ok);
  CHECK(!reader.append_transaction(sample_batch()));
  CHECK(io->read(path, 10000).value == prefix);
}
