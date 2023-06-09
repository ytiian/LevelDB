// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Decodes the blocks generated by block_builder.cc.
//与block_builder的关联：
//block_builder的finish()返回buffer_
//buffer_作为BlockContents的data_传递给block
//2.24✔

#include "table/block.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "leveldb/comparator.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

//从buffer_中得到restart点的数量
inline uint32_t Block::NumRestarts() const {
  assert(size_ >= sizeof(uint32_t));
  //指针移动到记录restart数量开始的地方(data_是const char*)
  return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

//初始化Block，保存实际内容、长度、restart array开始的位置
Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_(contents.heap_allocated) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // Error marker
    //显然错误，因为restart的数量至少占unit32_t
    //size_的单位是byte
  } else {
    //最多允许有多少个restart点
    //除以sizeof(unit32_t)是因为一个restart点的offset占unit32_t
    size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    if (NumRestarts() > max_restarts_allowed) {
      // The size is too small for NumRestarts()
      //一种错误情形，记录的数量大于计算出来的最大值
      size_ = 0;
    } else {
      //得到restart array开始的位置
      //1：restart数量占一个unit32_t
      restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t);
    }
  }
}

Block::~Block() {
  if (owned_) {
    delete[] data_;
  }
}

// Helper routine: decode the next block entry starting at "p",
// storing the number of shared key bytes, non_shared key bytes,
// and the length of the value in "*shared", "*non_shared", and
// "*value_length", respectively.  Will not dereference past "limit".
//
// If any errors are detected, returns nullptr.  Otherwise, returns a
// pointer to the key delta (just past the three decoded values).
//略：参数p指向的是条目的开始位置，
//解码下一条Entry，返回p（猜想：返回的p指向的是non_shared开始的位置）
static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared, uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = reinterpret_cast<const uint8_t*>(p)[0];
  *non_shared = reinterpret_cast<const uint8_t*>(p)[1];
  *value_length = reinterpret_cast<const uint8_t*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}


class Block::Iter : public Iterator {
 private:
  const Comparator* const comparator_;
  const char* const data_;       // underlying block contents
  uint32_t const restarts_;      // Offset of restart array (list of fixed32)
  uint32_t const num_restarts_;  // Number of uint32_t entries in restart array

  // current_ is offset in data_ of current entry.  >= restarts_ if !Valid
  //当前条目在data中的偏移值
  uint32_t current_;
  //当前条目落在的restart的索引
  uint32_t restart_index_;  // Index of restart block in which current_ falls
  std::string key_;//所指向的key
  Slice value_;//所指向的value
  Status status_;

  inline int Compare(const Slice& a, const Slice& b) const {
    return comparator_->Compare(a, b);
  }

  // Return the offset in data_ just past the end of the current entry.
  //返回下一条条目的偏移，（当前value的结尾）
  inline uint32_t NextEntryOffset() const {
    return (value_.data() + value_.size()) - data_;
  }

//先得到存放第index条restart偏移的指针，decode得到实际偏移值（SeekToRestartPoint调用）
  uint32_t GetRestartPoint(uint32_t index) {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

//定位到某个restart点
  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    // current_ will be fixed by ParseNextKey();

    // ParseNextKey() starts at the end of value_, so set value_ accordingly
    uint32_t offset = GetRestartPoint(index);
    //指向restart点
    value_ = Slice(data_ + offset, 0);
  }

 public:
 //data：BlockContents的具体内容，restarts是restart array开始的地方
  Iter(const Comparator* comparator, const char* data, uint32_t restarts,
       uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),//初始化为restart array开始的位置
        restart_index_(num_restarts_) {//初始化为最大值的restart编号
    assert(num_restarts_ > 0);
  }

//如果条目在restart数组之前 说明是有效的
  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  Slice key() const override {
    assert(Valid());
    return key_;
  }
  Slice value() const override {
    assert(Valid());
    return value_;
  }

//迭代器向前移一个条目  后->前
  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

//向后移一个条目  后->前
  void Prev() override {
    assert(Valid());

    //向后扫描小于current_的一个restart点 后->前
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {//直到这个restart点小于current_
      if (restart_index_ == 0) {
        // No more entries
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    //调用此函数之后，key清空，value开始于restart点，长度0
    do {
    //调用SeekToRestartPoint后开始调用ParseNextKey，current_=(value_.data() + value_.size()) - data_
    //current_是新条目的偏移量
    //下一条条目的偏移量不能超过所查key，即要保证<key
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  void Seek(const Slice& target) override {
    // Binary search in restart array to find the last restart point
    // with a key < target
    //目标：找到小于target的最后一个restart点
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    int current_key_compare = 0;

    if (Valid()) {
      // If we're already scanning, use the current position as a starting
      // point. This is beneficial if the key we're seeking to is ahead of the
      // current position.
      current_key_compare = Compare(key_, target);
      if (current_key_compare < 0) {//key_<target
        // key_ is smaller than target
        left = restart_index_;
      } else if (current_key_compare > 0) {
        right = restart_index_;
      } else {
        // We're seeking to the key we're already at.
        return;
      }
    }

    while (left < right) {
      uint32_t mid = (left + right + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr =
          DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                      &non_shared, &value_length);
      if (key_ptr == nullptr || (shared != 0)) {
        CorruptionError();
        return;
      }
      Slice mid_key(key_ptr, non_shared);//mid_key的non_shared部分肯定保存的是完整key 
      if (Compare(mid_key, target) < 0) {
        // Key at "mid" is smaller than "target".  Therefore all
        // blocks before "mid" are uninteresting.
        left = mid;
      } else {
        // Key at "mid" is >= "target".  Therefore all blocks at or
        // after "mid" are uninteresting.
        right = mid - 1;
      }
    }

    // We might be able to use our current position within the restart block.
    // This is true if we determined the key we desire is in the current block
    // and is after than the current key.
    assert(current_key_compare == 0 || Valid());
    bool skip_seek = left == restart_index_ && current_key_compare < 0;
    if (!skip_seek) {
      SeekToRestartPoint(left);
    }
    // Linear search (within restart block) for first key >= target
    while (true) {
      if (!ParseNextKey()) {//已经到了最后一条，所有key都<target
      //1，3，5 target=6 iter指向restart array
        return;
      }
      if (Compare(key_, target) >= 0) {//1，3，5   target=4的时候 iter指向5
        return;
      }
    }
  }

//定位到第一个条目
  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

//定位到最后一个条目
  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
      // Keep skipping
    }
  }

 private:
  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_.clear();
  }

  bool ParseNextKey() {
    //在SeekToRestartPoint后，需要一次ParseNextKey，才开始指向真正的条目。否则只是暂时指向restart
    current_ = NextEntryOffset();//获得下一个条目的偏移值
    const char* p = data_ + current_;//下一个条目的位置
    const char* limit = data_ + restarts_;  // Restarts come right after data
    //restart数组只能在数据右边
    if (p >= limit) {
      //p大于limit说明已经没有剩余条目
      // No more entries to return.  Mark as invalid.
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    // Decode next entry
    uint32_t shared, non_shared, value_length;
    //参数p是条目的开始位置，返回值p指向non_shared部分的key
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    } else {
      //获得条目的key和value
      key_.resize(shared);//直接由上一条key得到新key
      key_.append(p, non_shared);
      value_ = Slice(p + non_shared, value_length);
      while (restart_index_ + 1 < num_restarts_ &&
             GetRestartPoint(restart_index_ + 1) < current_) {
        ++restart_index_;
      }//定位到本条目最接近的restart
      return true;
    }
  }
};

//在块上新建一个迭代器(属于Block类的方法)
Iterator* Block::NewIterator(const Comparator* comparator) {
  if (size_ < sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return NewEmptyIterator();
  } else {
    return new Iter(comparator, data_, restart_offset_, num_restarts);
  }
}

}  // namespace leveldb
