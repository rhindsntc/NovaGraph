#include "graphdb/Recovery.hpp"
#include <cstring>
namespace graphdb {
namespace {
[[noreturn]] void bad(const char *message){throw QueryFailure(Status::Error(message,ErrorCode::corruptData));}
class Stream {
  SequentialReader &reader_;
  QueryContext &context_;
  std::array<uint8_t,16384> buffer_{};
  size_t available_=0,cursor_=0;
public:
  uint64_t position=0,limit;
  uint32_t crc=0;
  Stream(SequentialReader &r,QueryContext &ctx):reader_(r),context_(ctx),limit(r.size()){}
  void take(uint64_t count,uint8_t *out=nullptr,bool checksum=true) {
    if(position>limit || count>limit-position)bad("catalog field exceeds enclosing extent");
    while(count){
      context_.enforce();
      if(cursor_==available_){auto result=reader_.read(buffer_.data(),buffer_.size());
        if(!result)throw QueryFailure(result.status);
        if(!result.value || result.value>buffer_.size())bad("short catalog stream");
        cursor_=0;available_=result.value;}
      auto n=std::min<uint64_t>(count,available_-cursor_);
      if(checksum)crc=crc32_compute(buffer_.data()+cursor_,n,crc);
      if(out){std::memcpy(out,buffer_.data()+cursor_,n);out+=n;}
      cursor_+=n;position+=n;count-=n;
    }
  }
  uint64_t number(size_t bytes,bool checksum=true){std::array<uint8_t,8> v{};take(bytes,v.data(),checksum);uint64_t n=0;
    for(size_t i=0;i<bytes;++i)n|=uint64_t(v[i])<<(8*i);return n;}
  uint64_t extent(){auto n=number(4);if(n>limit-position)bad("invalid catalog blob extent");return position+n;}
  void string(){auto end=extent();take(end-position);}
};
}
Status scan_retained_catalog(FileIO &io,const std::filesystem::path &path,
    const CatalogDescriptor &descriptor,const std::array<uint8_t,16> &database_id,
    QueryContext &context,const std::function<void(std::string_view)> &mark) {
  try {
    auto reservation=context.hold_work(catalog_scan_workspace+4*path.native().size());
    auto opened=io.open_reader(path,64*1024*1024,16384);if(!opened)return opened.status;
    auto &reader=*opened.value;
    if(reader.size()!=descriptor.bytes || reader.size()<20)bad("retained catalog extent mismatch");
    Stream stream(reader,context);
    if(stream.number(4)!=0x4E434154)bad("invalid catalog magic");
    auto version=stream.number(4);
    if(version!=kFormatVersion || descriptor.version!=kFormatVersion)
      return Status::Error("unsupported catalog version",ErrorCode::unsupportedVersion);
    if(stream.number(8)!=reader.size()-20)bad("invalid catalog envelope extent");
    stream.limit=reader.size()-4;
    std::array<uint8_t,16> identity{};stream.take(identity.size(),identity.data());
    if(identity!=database_id)bad("retained catalog database mismatch");
    if(stream.number(8)!=descriptor.generation || stream.number(8)!=descriptor.committed_lsn)
      bad("retained catalog generation/LSN mismatch");
    auto count=stream.number(4);
    if(count>(stream.limit-stream.position)/4)bad("invalid manifest count");
    for(uint64_t i=0;i<count;++i){
      context.enforce();auto end=stream.extent();auto enclosing=stream.limit;stream.limit=end;
      auto kind=stream.number(1);if(kind!=1 && kind!=2)bad("invalid manifest kind");
      for(int field=0;field<4;++field)stream.string();
      if(!stream.number(8))bad("invalid manifest version");
      auto tier=stream.number(1);
      if(tier==1){
        auto length=stream.number(4);if(!length || length>255)bad("invalid cold filename extent");
        std::array<char,255> name{};stream.take(length,reinterpret_cast<uint8_t*>(name.data()));
        std::string_view filename(name.data(),length);
        if(filename.find('\0')!=std::string_view::npos || filename.find('/')!=std::string_view::npos || filename=="." || filename=="..")bad("invalid cold filename");
        auto bytes=stream.number(8);if(bytes<20 || bytes>kMaxObjectBytes)bad("invalid cold payload extent");
        stream.number(4);mark(filename);
      } else if(!tier){auto payload_end=stream.extent();stream.take(payload_end-stream.position);}
      else bad("invalid manifest tier");
      if(stream.position!=end)bad("trailing manifest bytes");stream.limit=enclosing;
    }
    count=stream.number(4);if(count>(stream.limit-stream.position)/8)bad("invalid declaration count");
    for(uint64_t i=0;i<count;++i){context.enforce();stream.string();stream.string();}
    for(uint64_t i=1;i<=4;++i){
      auto end=stream.extent();auto enclosing=stream.limit;stream.limit=end;
      if(stream.number(1)!=i || stream.number(4)!=1)bad("invalid index section version");
      auto bytes=stream.number(8);
      if(stream.limit-stream.position<4 || bytes!=stream.limit-stream.position-4)bad("invalid index section extent");
      stream.take(bytes);stream.number(4);stream.limit=enclosing;
    }
    if(stream.position!=stream.limit)bad("trailing catalog bytes");
    auto crc=stream.crc;stream.limit=reader.size();auto encoded=stream.number(4,false);
    if(encoded!=crc || crc!=descriptor.crc)bad("retained catalog checksum mismatch");
    context.enforce();return reader.close();
  } catch(const QueryFailure &error){return error.status;}
}
} // namespace graphdb
