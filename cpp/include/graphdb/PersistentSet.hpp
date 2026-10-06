#pragma once
#include "graphdb/PersistentMap.hpp"
#include <vector>
namespace graphdb {
// Immutable bounded posting blocks. Only a changed block and its index paths are
// allocated; retained iterators and transaction snapshots continue to own old data.
class PersistentSet {
  struct Block {
    WorkReservation reservation;
    std::vector<std::string> keys;
    explicit Block(WorkReservation r):reservation(std::move(r)){}
  };
  using Ptr=std::shared_ptr<const Block>;
  using Map=PersistentMap<Ptr>;
  Ptr small_;
  Map blocks_;
  size_t size_=0;
  static constexpr size_t width=32;
  template<class Get> static Ptr block(size_t count,Get get,QueryContext *ctx) {
    size_t charge=sizeof(Block)+32+count*sizeof(std::string);
    for(size_t i=0;i<count;++i)charge+=get(i).size()+24;
    auto out=std::make_shared<Block>(ctx?ctx->hold_work(charge):WorkReservation{});
    out->keys.reserve(count);
    for(size_t i=0;i<count;++i)out->keys.push_back(get(i));
    return out;
  }
  Ptr locate(const std::string &key) const {
    if(small_ || blocks_.empty())return small_;
    auto it=blocks_.floor(key);return it==blocks_.end()?blocks_.begin()->second:it->second;
  }
  void replace(const Ptr &old,const Ptr &next,QueryContext *ctx) {
    if(old)blocks_.erase(old->keys.front(),ctx);
    if(next)blocks_.set(next->keys.front(),next,ctx);
  }
public:
  class Iterator {
    Map::Iterator map_;
    Ptr block_;
    size_t index_=0;
    friend class PersistentSet;
    Iterator(Map::Iterator map,Ptr block,size_t index=0):map_(std::move(map)),block_(std::move(block)),index_(index){}
  public:
    Iterator()=default;
    const std::string &operator*() const{return block_->keys[index_];}
    const std::string *operator->() const{return &**this;}
    Iterator &operator++(){
      if(++index_==block_->keys.size()){
        if(map_!=Map::Iterator{}){++map_;block_=map_==Map::Iterator{}?Ptr{}:map_->second;}
        else block_.reset();
        index_=0;
      }return *this;
    }
    bool operator==(const Iterator &other) const {
      return block_==other.block_ && (!block_ || index_==other.index_);
    }
  };
  Iterator begin() const {
    if(small_)return Iterator({},small_);
    auto it=blocks_.begin();return it==blocks_.end()?end():Iterator(it,it->second);
  }
  Iterator end() const{return {};}
  Iterator upper_bound(const std::string &key) const {
    auto it=blocks_.floor(key);
    if(!small_ && it==blocks_.end())return begin();
    auto b=small_?small_:(it==blocks_.end()?Ptr{}:it->second);
    if(!b)return end();
    auto index=std::upper_bound(b->keys.begin(),b->keys.end(),key)-b->keys.begin();
    if(size_t(index)<b->keys.size())return Iterator(it,b,index);
    if(small_)return end();
    ++it;return it==blocks_.end()?end():Iterator(it,it->second);
  }
  size_t size() const{return size_;}
  bool empty() const{return !size_;}
  size_t count(const std::string &key) const {
    auto b=locate(key);return b && std::binary_search(b->keys.begin(),b->keys.end(),key);
  }
  void insert(const std::string &key,QueryContext *ctx=nullptr) {
    if(ctx)ctx->enforce();
    auto old=locate(key);size_t offset=0,n=old?old->keys.size():0;
    if(old){offset=std::lower_bound(old->keys.begin(),old->keys.end(),key)-old->keys.begin();
      if(offset<n && old->keys[offset]==key)return;}
    auto get=[&](size_t i)->const std::string&{return i==offset?key:old->keys[i-(i>offset)];};
    PersistentSet next=*this;
    if(n<width){auto b=block(n+1,get,ctx);if(size_<=width && blocks_.empty())next.small_=b;else next.replace(old,b,ctx);}
    else {
      auto left=block(16,get,ctx);
      auto right=block(17,[&](size_t i)->const std::string&{return get(i+16);},ctx);
      next.small_.reset();next.replace(old,left,ctx);next.blocks_.set(right->keys.front(),right,ctx);
    }
    ++next.size_;swap(next);
  }
  void erase(const std::string &key,QueryContext *ctx=nullptr) {
    if(ctx)ctx->enforce();
    auto old=locate(key);if(!old)return;
    size_t offset=std::lower_bound(old->keys.begin(),old->keys.end(),key)-old->keys.begin();
    if(offset==old->keys.size() || old->keys[offset]!=key)return;
    PersistentSet next=*this;
    Ptr b;
    if(old->keys.size()>1)b=block(old->keys.size()-1,[&](size_t i)->const std::string&{return old->keys[i+(i>=offset)];},ctx);
    --next.size_;
    if(small_)next.small_=b;
    else {
      next.replace(old,b,ctx);
      if(b){
        auto following=next.blocks_.upper_bound(b->keys.front());
        Ptr neighbor=following==next.blocks_.end()?Ptr{}:following->second;
        if(!neighbor || b->keys.size()+neighbor->keys.size()>width){
          auto prior=next.blocks_.floor(b->keys.front(),false);
          neighbor=prior==next.blocks_.end()?Ptr{}:prior->second;
        }
        if(neighbor && b->keys.size()+neighbor->keys.size()<=width){
          auto a=b,c=neighbor;if(c->keys.front()<a->keys.front())std::swap(a,c);
          auto merged=block(a->keys.size()+c->keys.size(),[&](size_t i)->const std::string&{return i<a->keys.size()?a->keys[i]:c->keys[i-a->keys.size()];},ctx);
          next.replace(b,{},ctx);next.replace(neighbor,merged,ctx);
        }
      }
      if(next.size_<=width){
        if(next.blocks_.size()==1)next.small_=next.blocks_.begin()->second;
        else if(next.size_){
          // At most 32 members remain; no graph-sized temporary vector.
          auto it=next.begin();next.small_=block(next.size_,[&](size_t i)->const std::string&{
            if(i==0)it=next.begin();else ++it;return *it;},ctx);
        }
        next.blocks_.clear();
      }
    }
    swap(next);
  }
  void clear() noexcept{small_.reset();blocks_.clear();size_=0;}
  void swap(PersistentSet &other) noexcept{small_.swap(other.small_);blocks_.swap(other.blocks_);std::swap(size_,other.size_);}
  size_t memory_usage() const {
    auto bytes=[](const Ptr &b){size_t n=sizeof(Block)+32+b->keys.capacity()*sizeof(std::string);
      for(const auto &key:b->keys)n+=key.capacity()+1;return n;};
    if(small_)return bytes(small_);
    size_t n=0;for(const auto &[key,b]:blocks_)n+=Map::entry_overhead_bytes()+key.capacity()+1+bytes(b);
    return n;
  }
};
} // namespace graphdb
