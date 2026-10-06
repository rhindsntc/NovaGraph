#pragma once
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include "nlohmann/json.hpp"
namespace nova_resource {
inline nlohmann::json disk_snapshot(const std::filesystem::path& root,const std::string& stage){
 using json=nlohmann::json;
 if(std::filesystem::is_symlink(std::filesystem::symlink_status(root))||!std::filesystem::is_directory(root))throw std::runtime_error("invalid disk snapshot root");
 json categories=json::object();
 for(auto name:{"payload","catalog","wal","other"})categories[name]={{"logicalBytes",uint64_t(0)},{"allocatedBytes",uint64_t(0)},{"files",uint64_t(0)}};
 uint64_t logical=0,allocated=0,files=0;
 auto add=[](uint64_t a,uint64_t b){if(b>std::numeric_limits<uint64_t>::max()-a)throw std::runtime_error("disk counter overflow");return a+b;};
 for(const auto& entry:std::filesystem::recursive_directory_iterator(root)){
  auto status=entry.symlink_status();
  if(std::filesystem::is_symlink(status))throw std::runtime_error("symlink in disk snapshot");
  if(std::filesystem::is_directory(status))continue;
  if(!std::filesystem::is_regular_file(status))throw std::runtime_error("nonregular file in disk snapshot");
  struct stat info{};if(lstat(entry.path().c_str(),&info)!=0||!S_ISREG(info.st_mode)||info.st_size<0)throw std::runtime_error("disk snapshot stat failed");
  uint64_t bytes=static_cast<uint64_t>(info.st_size),blocks=0;
#if defined(__APPLE__) || defined(__linux__)
  if(info.st_blocks<0||static_cast<uint64_t>(info.st_blocks)>std::numeric_limits<uint64_t>::max()/512)throw std::runtime_error("invalid disk allocation count");
  blocks=static_cast<uint64_t>(info.st_blocks)*512;
#endif
  auto rel=entry.path().lexically_relative(root);auto name=entry.path().filename().string();
  std::string category=*rel.begin()=="cold"?"payload":name.starts_with("catalog-")?"catalog":(name.starts_with("wal-")||name.ends_with(".wal"))?"wal":"other";
  auto& c=categories[category];c["logicalBytes"]=add(c["logicalBytes"].get<uint64_t>(),bytes);c["allocatedBytes"]=add(c["allocatedBytes"].get<uint64_t>(),blocks);c["files"]=add(c["files"].get<uint64_t>(),1);
  logical=add(logical,bytes);allocated=add(allocated,blocks);files=add(files,1);
 }
 json out={{"stage",stage},{"logicalBytes",logical},{"allocatedBytes",allocated},{"files",files},{"categories",categories},{"allocationSupport","st_blocks_512"}};
#if !defined(__APPLE__) && !defined(__linux__)
 out["allocatedBytes"]=nullptr;out["allocationSupport"]="unavailable";for(auto& c:out["categories"].items())c.value()["allocatedBytes"]=nullptr;
#endif
 return out;
}
}
