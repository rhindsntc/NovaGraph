#pragma once
#include "graphdb/QueryContext.hpp"
#include <array>
#include <string>
#include <utility>

namespace graphdb {
// Immutable AVL nodes make a staged root cheap and publication a noexcept swap.
// Rebalancing copies paths, never unrelated values. Temporary paths release their
// workspace reservations immediately, so the limit bounds live staging memory.
template<class Value> class PersistentMap {
  struct Entry {
    WorkReservation reservation;
    std::pair<std::string, Value> value;
    Entry(WorkReservation r, std::string key, Value item)
        : reservation(std::move(r)), value(std::move(key),std::move(item)) {}
  };
  using Item=std::shared_ptr<const Entry>;
  struct Node;
  using Ptr=std::shared_ptr<const Node>;
  struct Node {
    WorkReservation reservation;
    Item item;
    Ptr left,right;
    int height;
    size_t count;
    Node(WorkReservation r,Item i,Ptr l,Ptr next)
        : reservation(std::move(r)),item(std::move(i)),left(std::move(l)),right(std::move(next)),
          height(1+std::max(left?left->height:0,right?right->height:0)),
          count(1+(left?left->count:0)+(right?right->count:0)) {}
  };
  Ptr root_;
  static int height(const Ptr &p) {return p?p->height:0;}
  static Ptr node(Item item,Ptr left,Ptr right,QueryContext *ctx) {
    return std::make_shared<Node>(ctx?ctx->hold_work(sizeof(Node)+32):WorkReservation{},
                                  std::move(item),std::move(left),std::move(right));
  }
  static Ptr balance(Item item,Ptr left,Ptr right,QueryContext *ctx) {
    if(height(left)>height(right)+1) {
      if(height(left->left)>=height(left->right))
        return node(left->item,left->left,node(item,left->right,right,ctx),ctx);
      auto pivot=left->right;
      return node(pivot->item,node(left->item,left->left,pivot->left,ctx),
                  node(item,pivot->right,right,ctx),ctx);
    }
    if(height(right)>height(left)+1) {
      if(height(right->right)>=height(right->left))
        return node(right->item,node(item,left,right->left,ctx),right->right,ctx);
      auto pivot=right->left;
      return node(pivot->item,node(item,left,pivot->left,ctx),
                  node(right->item,pivot->right,right->right,ctx),ctx);
    }
    return node(std::move(item),std::move(left),std::move(right),ctx);
  }
  static Ptr put(const Ptr &p,const Item &item,QueryContext *ctx) {
    if(!p)return node(item,{},{},ctx);
    if(item->value.first<p->item->value.first)
      return balance(p->item,put(p->left,item,ctx),p->right,ctx);
    if(item->value.first>p->item->value.first)
      return balance(p->item,p->left,put(p->right,item,ctx),ctx);
    return node(item,p->left,p->right,ctx);
  }
  static Ptr remove(const Ptr &p,const std::string &key,QueryContext *ctx) {
    if(!p)return {};
    if(key<p->item->value.first)return balance(p->item,remove(p->left,key,ctx),p->right,ctx);
    if(key>p->item->value.first)return balance(p->item,p->left,remove(p->right,key,ctx),ctx);
    if(!p->left)return p->right;
    if(!p->right)return p->left;
    auto next=p->right;
    while(next->left)next=next->left;
    return balance(next->item,p->left,remove(p->right,next->item->value.first,ctx),ctx);
  }
public:
  class Iterator {
    Ptr owner_;
    // An AVL tree with size_t cardinality cannot reach 128 levels.
    std::array<const Node *,128> path_{};
    size_t depth_=0;
    void left(const Node *p) {while(p){path_[depth_++]=p;p=p->left.get();}}
    friend class PersistentMap;
    Iterator(Ptr owner,const std::string *after=nullptr,bool inclusive=false):owner_(std::move(owner)) {
      if(!after){left(owner_.get());return;}
      auto p=owner_.get();
      while(p) {
        if(p->item->value.first>*after || (inclusive && p->item->value.first==*after)) {
          path_[depth_++]=p;p=p->left.get();
        } else p=p->right.get();
      }
    }
  public:
    Iterator()=default;
    const auto &operator*() const {return path_[depth_-1]->item->value;}
    const auto *operator->() const {return &**this;}
    Iterator &operator++(){auto p=path_[--depth_];left(p->right.get());return *this;}
    bool operator==(const Iterator &other) const {
      return (depth_?path_[depth_-1]:nullptr)==(other.depth_?other.path_[other.depth_-1]:nullptr);
    }
  };
  static constexpr size_t entry_overhead_bytes(){return sizeof(Node)+sizeof(Entry)+64;}
  // One insertion/replacement can retain an old path and rotation intermediates.
  // Values and ancestor keys are shared; only the replacement entry is copied.
  size_t update_path_charge() const {
    return (height(root_)+1)*3*(sizeof(Node)+32)+sizeof(Entry)+32;
  }
  Iterator begin() const{return Iterator(root_);}
  Iterator end() const{return {};}
  Iterator upper_bound(const std::string &key) const{return Iterator(root_,&key);}
  Iterator floor(const std::string &key,bool inclusive=true) const {
    auto p=root_.get();const std::string *candidate=nullptr;
    while(p){if(p->item->value.first<key || (inclusive && p->item->value.first==key)){candidate=&p->item->value.first;p=p->right.get();}
      else p=p->left.get();}
    return candidate?Iterator(root_,candidate,true):end();
  }
  Iterator find(const std::string &key) const {
    auto it=Iterator(root_,&key,true);return it!=end() && it->first==key?it:end();
  }
  const Value &at(const std::string &key) const {
    auto it=find(key);if(it==end())throw std::out_of_range("persistent map key");return it->second;
  }
  size_t size() const{return root_?root_->count:0;}
  bool empty() const{return !root_;}
  size_t count(const std::string &key) const{return find(key)!=end();}
  void set(const std::string &key,Value value,QueryContext *ctx=nullptr,size_t extra=0) {
    auto item=std::make_shared<Entry>(ctx?ctx->hold_work(sizeof(Entry)+32+key.size()+extra):WorkReservation{},key,std::move(value));
    root_=put(root_,item,ctx);
  }
  void erase(const std::string &key,QueryContext *ctx=nullptr) {
    if(count(key))root_=remove(root_,key,ctx);
  }
  void clear() noexcept{root_.reset();}
  void swap(PersistentMap &other) noexcept{root_.swap(other.root_);}
};
} // namespace graphdb
