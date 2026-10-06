#include "ResourceDisk.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
int main(){
 char temp[]="/tmp/nova-resource-disk-XXXXXX";if(!mkdtemp(temp))return 1;
 auto root=std::filesystem::path(temp);
 try {
  std::filesystem::create_directory(root/"cold");
  std::ofstream(root/"cold"/"payload")<<"1234567890";
  std::ofstream(root/"catalog-1.db")<<"123";
  std::ofstream(root/"active.wal")<<"12";
  std::ofstream(root/"CURRENT")<<"1";
  auto r=nova_resource::disk_snapshot(root,"fixture");
  if(r["logicalBytes"]!=16||r["files"]!=4||r["categories"]["payload"]["logicalBytes"]!=10||r["categories"]["catalog"]["logicalBytes"]!=3||r["categories"]["wal"]["logicalBytes"]!=2)throw std::runtime_error("wrong category totals");
  if(!r["allocatedBytes"].is_number_unsigned()||r["allocatedBytes"].get<uint64_t>()<16)throw std::runtime_error("missing allocated disk bytes");
  std::filesystem::create_symlink(root/"CURRENT",root/"alias");bool rejected=false;
  try{nova_resource::disk_snapshot(root,"bad");}catch(const std::exception&){rejected=true;}
  if(!rejected)throw std::runtime_error("symlink accepted");
  rejected=false;try{nova_resource::disk_snapshot(root/"missing","bad");}catch(const std::exception&){rejected=true;}
  if(!rejected)throw std::runtime_error("missing root accepted");
  std::filesystem::remove_all(root);std::cout<<"Resource disk real-file contracts passed\n";return 0;
 }catch(const std::exception& e){std::filesystem::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
