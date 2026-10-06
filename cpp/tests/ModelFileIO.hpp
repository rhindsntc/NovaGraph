#pragma once
#include "graphdb/FileIO.hpp"
#include <cerrno>
#include <map>
namespace nova_test {
// File contents and directory entries persist independently. Process exit does
// not discard volatile data; power_loss does. Used only by storage fault tests.
class ModelFileIO : public graphdb::FileIO {
  struct Inode {
    std::vector<uint8_t> live, durable;
  };
  using Names = std::map<std::filesystem::path, std::shared_ptr<Inode>>;
  Names live_, durable_;
  graphdb::Status fault(const char *operation, const std::filesystem::path &path) {
    if (fail_next != operation)
      return graphdb::Status::OK();
    fail_next.clear();
    return graphdb::Status::Error("injected " + std::string(operation),
                                  graphdb::ErrorCode::ioFailure, {path.string(), EIO});
  }

public:
  std::string fail_next;
  graphdb::Result<std::vector<std::string>> list(const std::filesystem::path& path,size_t offset=0,size_t limit=4096)override {std::vector<std::string> names;size_t index=0;for(const auto& [name,inode]:live_)if(name.parent_path()==path&&index++>=offset&&names.size()<limit)names.push_back(name.filename().string());return {graphdb::Status::OK(),std::move(names)};}

  void power_loss() {
    live_ = durable_;
    for (auto &[path, inode] : live_)
      inode->live = inode->durable;
  }
  graphdb::Result<std::vector<uint8_t>> read(const std::filesystem::path &path,
                                             size_t max) override {
    auto f = fault("read", path);
    if (!f.ok)
      return {f, {}};
    auto it = live_.find(path);
    if (it == live_.end())
      return {graphdb::Status::Error("missing", graphdb::ErrorCode::notFound), {}};
    if (it->second->live.size() > max)
      return {graphdb::Status::Error("bound", graphdb::ErrorCode::limitExceeded), {}};
    return {graphdb::Status::OK(), it->second->live};
  }
  graphdb::Result<std::unique_ptr<graphdb::SequentialReader>> open_reader(
      const std::filesystem::path &path,size_t maximum,size_t) override {
    auto status=fault("openReader",path);if(!status.ok)return {status,{}};
    auto it=live_.find(path);if(it==live_.end())return {graphdb::Status::Error("missing",graphdb::ErrorCode::notFound),{}};
    if(it->second->live.size()>maximum)return {graphdb::Status::Error("bound",graphdb::ErrorCode::limitExceeded),{}};
    struct Reader:graphdb::SequentialReader {
      ModelFileIO *owner;std::shared_ptr<Inode> inode;std::filesystem::path path;size_t extent,offset=0;
      Reader(ModelFileIO *o,std::shared_ptr<Inode> i,std::filesystem::path p):owner(o),inode(std::move(i)),path(std::move(p)),extent(inode->live.size()){}
      uint64_t size() const override{return extent;}
      graphdb::Result<size_t> read(uint8_t *out,size_t count) override {
        auto status=owner->fault("read",path);if(!status.ok)return {status,0};
        if(inode->live.size()!=extent)return {graphdb::Status::Error("stream extent changed",graphdb::ErrorCode::corruptData),0};
        count=std::min(count,extent-offset);std::copy_n(inode->live.data()+offset,count,out);offset+=count;
        return {graphdb::Status::OK(),count};
      }
      graphdb::Status close() override{return owner->fault("closeReader",path);}
    };
    return {graphdb::Status::OK(),std::make_unique<Reader>(this,it->second,path)};
  }
  graphdb::Status write_all(const std::filesystem::path &path, const std::vector<uint8_t> &bytes,
                            graphdb::WriteMode mode) override {
    auto f = fault("write", path);
    if (!f.ok)
      return f;
    if (mode == graphdb::WriteMode::exclusive && live_.count(path))
      return graphdb::Status::Error("exists", graphdb::ErrorCode::conflict);
    auto &inode = live_[path];
    if (!inode)
      inode = std::make_shared<Inode>();
    if (mode != graphdb::WriteMode::append)
      inode->live.clear();
    inode->live.insert(inode->live.end(), bytes.begin(), bytes.end());
    return graphdb::Status::OK();
  }
  graphdb::Status sync_file(const std::filesystem::path &path) override {
    auto f = fault("sync", path);
    if (!f.ok)
      return f;
    auto it = live_.find(path);
    if (it == live_.end())
      return graphdb::Status::Error("missing", graphdb::ErrorCode::notFound);
    it->second->durable = it->second->live;
    return graphdb::Status::OK();
  }
  graphdb::Status sync_directory(const std::filesystem::path &path) override {
    auto f = fault("directorySync", path);
    if (!f.ok)
      return f;
    for (auto it = durable_.begin(); it != durable_.end();)
      if (it->first.parent_path() == path)
        it = durable_.erase(it);
      else
        ++it;
    for (const auto &[name, inode] : live_)
      if (name.parent_path() == path)
        durable_[name] = inode;
    return graphdb::Status::OK();
  }
  graphdb::Status rename(const std::filesystem::path &from,
                         const std::filesystem::path &to) override {
    auto f = fault("rename", to);
    if (!f.ok)
      return f;
    auto it = live_.find(from);
    if (it == live_.end())
      return graphdb::Status::Error("missing", graphdb::ErrorCode::notFound);
    live_[to] = it->second;
    live_.erase(it);
    return graphdb::Status::OK();
  }
  graphdb::Status truncate(const std::filesystem::path &path, uint64_t size) override {
    auto status=fault("truncate",path);if(!status.ok)return status;
    auto it = live_.find(path);
    if (it == live_.end())
      return graphdb::Status::Error("missing", graphdb::ErrorCode::notFound);
    it->second->live.resize(size);
    return graphdb::Status::OK();
  }
  graphdb::Status remove(const std::filesystem::path &path) override {
    auto status=fault("remove",path);if(!status.ok)return status;
    live_.erase(path);
    return graphdb::Status::OK();
  }
};
} // namespace nova_test
