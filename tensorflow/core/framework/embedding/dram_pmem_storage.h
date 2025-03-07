/* Copyright 2022 The DeepRec Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
======================================================================*/
#ifndef TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_DRAM_PMEM_STORAGE_H_
#define TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_DRAM_PMEM_STORAGE_H_

#include "tensorflow/core/framework/embedding/multi_tier_storage.h"
#include "tensorflow/core/framework/embedding/single_tier_storage.h"
#include "tensorflow/core/framework/embedding/cpu_hash_map_kv.h"

namespace tensorflow {
template <class V>
class ValuePtr;

template <class K, class V>
class EmbeddingVar;

namespace embedding {

template<typename K, typename V>
class DramPmemStorage : public MultiTierStorage<K, V> {
 public:
  DramPmemStorage(const StorageConfig& sc, Allocator* dram_alloc,
      Allocator* pmem_alloc, LayoutCreator<V>* lc,
      const std::string& name)
      : MultiTierStorage<K, V>(sc, name) {
    dram_ = new DramStorage<K, V>(sc, dram_alloc, lc, new LocklessHashMap<K, V>());
    pmem_ = new PmemLibpmemStorage<K, V>(sc, pmem_alloc, lc);
    value_ptr_size_ =
        const_cast<EmbeddingConfig&>(sc.embedding_config).total_num(
            Storage<K, V>::GetAllocLen());
  }

  ~DramPmemStorage() override {
    MultiTierStorage<K, V>::DeleteFromEvictionManager();
    delete dram_;
    delete pmem_;
  }

  TF_DISALLOW_COPY_AND_ASSIGN(DramPmemStorage);

  Status Get(K key, ValuePtr<V>** value_ptr) override {
    Status s = dram_->Get(key, value_ptr);
    if (s.ok()) {
      return s;
    }
    s = pmem_->Get(key, value_ptr);
    if (s.ok()) {
      ValuePtr<V>* new_value_ptr = dram_->CreateValuePtr(value_ptr_size_);
      memcpy(new_value_ptr->GetPtr(), (*value_ptr)->GetPtr(),
             sizeof(FixedLengthHeader) + sizeof(V) * value_ptr_size_);
      *value_ptr = new_value_ptr;
      s = dram_->TryInsert(key, *value_ptr);
      if (s.ok()) {
        return s;
      }
      dram_->DestroyValuePtr(*value_ptr);
      return dram_->Get(key, value_ptr);
    }
    return s;
  }

  void Insert(K key, ValuePtr<V>* value_ptr) override {
    LOG(FATAL)<<"Unsupport Insert(K, ValuePtr<V>*) in DramPmemStorage.";
  }

  void Insert(K key, ValuePtr<V>** value_ptr,
              size_t alloc_len, bool to_dram = false) override {
    dram_->Insert(key, value_ptr, alloc_len);
  }
  Status GetOrCreate(K key, ValuePtr<V>** value_ptr,
      size_t size, CopyBackFlag &need_copyback) override {
     LOG(FATAL)<<"GetOrCreate(K key, ValuePtr<V>** value_ptr, "
              <<"size_t size, CopyBackFlag &need_copyback) "
              <<"in DramPmemStorage can not be called.";
  }

  bool IsUseHbm() override {
    return false;
  }

  bool IsSingleHbm() override {
    return false;
  }

  Status GetOrCreate(K key, ValuePtr<V>** value_ptr,
      size_t size) override {
    Status s = dram_->Get(key, value_ptr);
    if (s.ok()) {
      return s;
    }
    s = pmem_->Get(key, value_ptr);

    ValuePtr<V>* new_value_ptr = dram_->CreateValuePtr(size);
    if (s.ok()) {
      memcpy(new_value_ptr->GetPtr(), (*value_ptr)->GetPtr(),
             sizeof(FixedLengthHeader) + sizeof(V) * size);
    }
    *value_ptr = new_value_ptr;
    
    s = dram_->TryInsert(key, *value_ptr);
    if (s.ok()) {
      return s;
    }
    // Insert Failed, key already exist
    dram_->DestroyValuePtr(*value_ptr);
    return dram_->Get(key, value_ptr);
  }

  Status Remove(K key) override {
    dram_->Remove(key);
    pmem_->Remove(key);
    return Status::OK();
  }

  int64 Size() const override {
    int64 total_size = dram_->Size();
    total_size += pmem_->Size();
    return total_size;
  }

  int64 Size(int level) const override {
    if (level == 0) {
      return dram_->Size();
    } else if (level == 1) {
      return pmem_->Size();
    } else {
      return -1;
    }
  }

  int LookupTier(K key) const override {
    Status s = dram_->Contains(key);
    if (s.ok())
      return 0;
    s = pmem_->Contains(key);
    if (s.ok())
      return 1;
    return -1;
  }

  Status Save(
      const string& tensor_name,
      const string& prefix,
      BundleWriter* writer,
      const EmbeddingConfig& emb_config,
      ShrinkArgs& shrink_args,
      int64 value_len,
      V* default_value) override {
    std::vector<K> key_list, tmp_pmem_key_list;
    std::vector<ValuePtr<V>*> value_ptr_list, tmp_pmem_value_list;

    TF_CHECK_OK(dram_->GetSnapshot(&key_list, &value_ptr_list));
    dram_->Shrink(key_list, value_ptr_list, shrink_args, value_len);

    TF_CHECK_OK(pmem_->GetSnapshot(&tmp_pmem_key_list,
                                   &tmp_pmem_value_list));
    pmem_->Shrink(tmp_pmem_key_list, tmp_pmem_value_list,
                  shrink_args, value_len);

    for (int64 i = 0; i < tmp_pmem_key_list.size(); i++) {
      Status s = dram_->Contains(tmp_pmem_key_list[i]);
      if (!s.ok()) {
        key_list.emplace_back(tmp_pmem_key_list[i]);
        value_ptr_list.emplace_back(tmp_pmem_value_list[i]);
      }
    }

    TF_CHECK_OK((Storage<K, V>::SaveToCheckpoint(
        tensor_name, writer,
        emb_config,
        value_len, default_value,
        key_list,
        value_ptr_list)));

    return Status::OK();
  }

  Status Eviction(K* evict_ids, int64 evict_size) override {
    ValuePtr<V>* value_ptr;
    for (int64 i = 0; i < evict_size; ++i) {
      if (dram_->Get(evict_ids[i], &value_ptr).ok()) {
        TF_CHECK_OK(pmem_->Commit(evict_ids[i], value_ptr));
        TF_CHECK_OK(dram_->Remove(evict_ids[i]));
        dram_->DestroyValuePtr(value_ptr);
      }
    }
    return Status::OK();
  }

  Status EvictionWithDelayedDestroy(K* evict_ids, int64 evict_size) override {
    mutex_lock l(*(dram_->get_mutex()));
    mutex_lock l1(*(pmem_->get_mutex()));
    MultiTierStorage<K, V>::ReleaseInvalidValuePtr(dram_->alloc_);
    ValuePtr<V>* value_ptr = nullptr;
    for (int64 i = 0; i < evict_size; ++i) {
      if (dram_->Get(evict_ids[i], &value_ptr).ok()) {
        TF_CHECK_OK(pmem_->Commit(evict_ids[i], value_ptr));
        TF_CHECK_OK(dram_->Remove(evict_ids[i]));
        MultiTierStorage<K, V>::KeepInvalidValuePtr(value_ptr);
      }
    }
    return Status::OK();
  }

 protected:
  void SetTotalDims(int64 total_dims) override {}

 private:
  DramStorage<K, V>* dram_;
  PmemLibpmemStorage<K, V>* pmem_;
  int64 value_ptr_size_;
};
} // embedding
} // tensorflow

#endif // TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_DRAM_PMEM_STORAGE_H_
