/* Copyright 2019 The DeepRec Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/


#ifndef TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_EMBEDDING_VAR_H_
#define TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_EMBEDDING_VAR_H_

#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"

#include "tensorflow/core/framework/embedding/cache.h"
#include "tensorflow/core/framework/embedding/embedding_var_context.h"
#include "tensorflow/core/framework/embedding/embedding_var_restore.h"
#include "tensorflow/core/framework/embedding/value_ptr.h"
#include "tensorflow/core/framework/embedding/filter_factory.h"
#include "tensorflow/core/framework/embedding/gpu_hash_map_kv.h"
#include "tensorflow/core/framework/embedding/embedding_config.h"
#include "tensorflow/core/framework/embedding/storage.h"
#include "tensorflow/core/framework/embedding/storage_factory.h"
#include "tensorflow/core/framework/typed_allocator.h"

namespace tensorflow {
using CPUDevice = Eigen::ThreadPoolDevice;
using GPUDevice = Eigen::GpuDevice;

#if GOOGLE_CUDA
  void SyncWithEventMgr(se::Stream* stream,
      EventMgr* event_mgr);
#endif //GOOGLE_CUDA


template <class K, class V>
class GPUHashTable;

template <class K, class V>
class EmbeddingVar : public ResourceBase {
 public:
  EmbeddingVar(const string& name,
               embedding::Storage<K, V>* storage,
               EmbeddingConfig emb_cfg,
               Allocator* alloc):
      name_(name),
      storage_(storage),
      default_value_(nullptr),
      default_value_no_permission_(nullptr),
      value_len_(0),
      alloc_(alloc),
      default_value_alloc_(alloc),
      emb_config_(emb_cfg) {
    if (IsMultiLevel() || emb_config_.record_freq) {
      add_freq_fn_ = [](ValuePtr<V>* value_ptr, int64 freq, int64 filter_freq) {
        value_ptr->AddFreq(freq);
      };
    } else if (emb_config_.is_counter_filter()) {
      add_freq_fn_ = [](ValuePtr<V>* value_ptr, int64 freq, int64 filter_freq) {
        if (value_ptr->GetFreq() < filter_freq)
          value_ptr->AddFreq(freq);
      };
    } else {
      add_freq_fn_ = [](ValuePtr<V>* value_ptr, int64 freq, int64 filter_freq) {};
    }
    if (emb_config_.steps_to_live != 0 || emb_config_.record_version) {
      update_version_fn_ = [](ValuePtr<V>* value_ptr, int64 gs) {
        value_ptr->SetStep(gs);
      };
    } else {
      update_version_fn_ = [](ValuePtr<V>* value_ptr, int64 gs) {};
    }
  }

  Status Init(const Tensor& default_tensor, int64 default_value_dim) {
    if (storage_ == nullptr) {
      return errors::InvalidArgument(
          "Invalid ht_type to construct EmbeddingVar");
    }

    storage_type_ = storage_->GetStorageType();
    filter_ = FilterFactory::CreateFilter<K, V, EmbeddingVar<K, V>>(
        emb_config_, this, storage_);
    emb_config_.default_value_dim = default_value_dim;
    value_len_ =
        default_tensor.NumElements() / emb_config_.default_value_dim;

    if (LayoutType::NORMAL_CONTIGUOUS == storage_->GetLayoutType() ||
        LayoutType::NORMAL_CONTIGUOUS_GPU == storage_->GetLayoutType() ||
        LayoutType::COMPACT == storage_->GetLayoutType()) {
      storage_->SetAllocLen(value_len_, emb_config_.slot_num + 1);
    }

    if (storage_->IsUseHbm()) {
#if GOOGLE_CUDA
      default_value_ = TypedAllocator::Allocate<V>(alloc_,
          default_tensor.NumElements(), AllocationAttributes());
      auto default_tensor_flat = default_tensor.flat<V>();
      dev_addr_buffer_ = nullptr;
      dev_addr_buffer_size_ = 0;
      cudaMemcpy(default_value_, &default_tensor_flat(0),
          default_tensor.TotalBytes(), cudaMemcpyDeviceToDevice);
      storage_->
          CreateEmbeddingMemoryPool(
              alloc_,
              emb_config_.total_num(
                  storage_->GetAllocLen()),
              1024 * 1024 * 64);
#endif  // GOOGLE_CUDA
    } else if (storage_->IsSingleHbm()) {
#if GOOGLE_CUDA
      storage_->SetValueLen(value_len_);
      default_value_ = TypedAllocator::Allocate<V>(
          alloc_, default_tensor.NumElements(), AllocationAttributes());
      auto default_tensor_flat = default_tensor.flat<V>();
      cudaMemcpy(default_value_, &default_tensor_flat(0),
          default_tensor.TotalBytes(), cudaMemcpyDeviceToDevice);
#endif  // GOOGLE_CUDA
    } else {
      alloc_ = ev_allocator();
      default_value_ = TypedAllocator::Allocate<V>(default_value_alloc_,
          default_tensor.NumElements(), AllocationAttributes());

      auto default_tensor_flat = default_tensor.flat<V>();
      memcpy(default_value_, &default_tensor_flat(0),
          default_tensor.TotalBytes());

      default_value_no_permission_ = TypedAllocator::Allocate<V>(
          default_value_alloc_, value_len_, AllocationAttributes());
      for (int i = 0; i < value_len_; ++i) {
        default_value_no_permission_[i] = static_cast<V>(
            emb_config_.default_value_no_permission);
      }
    }

    return Status::OK();
  }

  void SetInitialized() {
    is_initialized_ = true;
  }

  bool IsInitialized() const {
    return is_initialized_;
  }

  Status LookupKey(K key, ValuePtr<V>** value_ptr) {
    return storage_->Get(key, value_ptr);
  }

  void BatchLookupKey(const EmbeddingVarContext<GPUDevice>& ctx,
                      const K* keys,
                      ValuePtr<V>** value_ptr_list,
                      int64 num_of_keys) {
    storage_->BatchGet(ctx, keys, value_ptr_list, num_of_keys,
                       emb_config_.total_num(storage_->GetAllocLen()));
  }

  Status LookupOrCreateKey(K key, ValuePtr<V>** value_ptr,
                           bool* is_filter, bool indices_as_pointer,
                           int64 count = 1) {
    if (indices_as_pointer) {
      *value_ptr = (ValuePtr<V>*)key;
      *is_filter = (*value_ptr != nullptr);
      return Status::OK();
    } else {
      Status s = filter_->LookupOrCreateKey(key, value_ptr, is_filter, count);
      add_freq_fn_(*value_ptr, count, emb_config_.filter_freq);
      return s;
    }
  }

  Status Insert(K key, V* value) {
    ValuePtr<V>* value_ptr = nullptr;
    CreateKey(key, &value_ptr, true);
    LookupOrCreateEmb(value_ptr, value);
    return Status::OK();
  }

  Status LookupOrCreateKey(K key, ValuePtr<V>** value_ptr) {
    Status s = storage_->GetOrCreate(key, value_ptr,
        emb_config_.total_num(storage_->GetAllocLen()));
    TF_CHECK_OK(s);
    return s;
  }

  void CreateKey(K key, ValuePtr<V>** value_ptr, bool to_dram) {
    storage_->Insert(key, value_ptr,
        emb_config_.total_num(storage_->GetAllocLen()), to_dram);
  }

  void UpdateVersion(ValuePtr<V>* value_ptr, int64 gs) {
    update_version_fn_(value_ptr, gs);
  }

  void BatchCommit(const std::vector<K>& keys,
                   const std::vector<ValuePtr<V>*>& value_ptrs) {
    TF_CHECK_OK(storage_->BatchCommit(keys, value_ptrs));
  }

  void Eviction(K* evict_ids, int64 evict_size) {
    TF_CHECK_OK(storage_->Eviction(evict_ids, evict_size));
  }

  int64 GetVersion(K key) {
    ValuePtr<V>* value_ptr = nullptr;
    TF_CHECK_OK(LookupOrCreateKey(key, &value_ptr));
    return value_ptr->GetStep();
  }

  int64 GetFreq(K key) {
    return filter_->GetFreq(key);
  }

  Status Lookup(K key, V* val, V* default_v)  {
    const V* default_value_ptr =
      (default_v == nullptr) ? default_value_ : default_v;
    return filter_->Lookup(key, val, default_value_ptr,
                           default_value_no_permission_);
  }

  void GetEmbeddings(const EmbeddingVarContext<CPUDevice>& context,
                     const K* keys, V* output,
                     int64 num_of_keys) {
    auto do_work = [this, keys, output] (int64 start, int64 limit) {
      for (int64 i = start; i < limit; ++i) {
        V* default_v =
            default_value_ +
                (keys[i] % emb_config_.default_value_dim) * value_len_;
        filter_->Lookup(keys[i],
            output + i * value_len_, default_v,
            default_value_no_permission_);
      }
    };
    auto worker_threads = context.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers, num_of_keys,
          value_len_ * sizeof(V), do_work);
  }

//Used for CPU Adaptive Embedding
  void GetEmbeddings(const EmbeddingVarContext<CPUDevice>& context,
                     const K* keys, V* output,
                     int64 num_of_keys, V* default_value) {
    auto do_work = [this, keys, output, default_value]
        (int64 start, int64 limit) {
      for (int64 i = start; i < limit; ++i) {
        V* default_v = default_value + i * value_len_;
        ValuePtr<V>* value_ptr = nullptr;
        filter_->LookupOrCreate(
            keys[i], output + i * value_len_, default_v, &value_ptr, 1,
            default_value_no_permission_);
        add_freq_fn_(value_ptr, 1, emb_config_.filter_freq);
      }
    };
    auto worker_threads = context.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers, num_of_keys,
          value_len_ * sizeof(V), do_work);
  }

  void GetOrCreateKey(const EmbeddingVarContext<CPUDevice>& context,
                      const Tensor& keys_tensor,
                      ValuePtr<V>** value_ptrs,
                      int64 num_of_keys) {
    const K* keys = (K*)keys_tensor.data();
    auto do_work = [this, keys, value_ptrs] (int64 start, int64 limit) {
      for (int64 i = start; i < limit; ++i) {
        bool is_filter = false;
        filter_->LookupOrCreateKey(keys[i], &value_ptrs[i], &is_filter, 1);
      }
    };
    auto worker_threads = context.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers,
          num_of_keys, value_len_ * sizeof(V), do_work);

    storage_->AddToCachePrefetchList(keys_tensor);
  }

  void GatherEmbeddings(const EmbeddingVarContext<CPUDevice>& context,
                        const Tensor& keys_tensor,
                        ValuePtr<V>** value_ptrs,
                        V* output,
                        int64 num_of_keys) {
    const K* keys = (K*)keys_tensor.data();
    auto do_work = [this, keys, value_ptrs, output]
        (int64 start, int64 limit) {
      for (int64 i = start; i < limit; ++i) {
        bool is_admit = filter_->is_admit(keys[i], value_ptrs[i]);
        add_freq_fn_(value_ptrs[i], 1, emb_config_.filter_freq);
        V* value = nullptr;
        if (is_admit) {
          V* default_v =
              default_value_ +
                  (keys[i] % emb_config_.default_value_dim) * value_len_;
          value = LookupOrCreateEmb(value_ptrs[i], default_v);
        } else {
          value = default_value_no_permission_;
        }
        memcpy(output + i * value_len_, value, sizeof(V) * value_len_);
      }
    };
    auto worker_threads = context.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers, num_of_keys,
          value_len_ * sizeof(V), do_work);

    storage_->AddToCache(keys_tensor);
  }

#if GOOGLE_CUDA
  void GetEmbeddings(const EmbeddingVarContext<GPUDevice>& context,
                     const K* keys,
                     V* output,
                     int64 num_of_keys) {
    if (IsSingleHbm()) {
      storage_->BatchLookup(context.gpu_device, keys, 
		            output, num_of_keys, default_value_);
    } else {
      filter_->BatchLookup(context, keys, output,
                           num_of_keys, default_value_,
                           default_value_no_permission_);
    }
  }

  void GetOrCreateKey(const EmbeddingVarContext<GPUDevice>& context,
                      const Tensor& keys_tensor,
                      ValuePtr<V>** value_ptrs,
                      int64 num_of_keys) {
    const K* keys = (K*)keys_tensor.data();
    filter_->BatchLookupOrCreateKey(context, keys, value_ptrs, num_of_keys);
    storage_->AddToCachePrefetchList(keys_tensor);
  }

  void BatchLookupOrCreateKey(
      const EmbeddingVarContext<GPUDevice>& context,
      const K* keys,
      ValuePtr<V>** value_ptrs,
      int64 num_of_keys,
      std::vector<std::list<int64>>& not_found_cursor_list) {
    storage_->BatchGetOrCreate(context, keys, value_ptrs, num_of_keys,
                               emb_config_.total_num(storage_->GetAllocLen()),
                               not_found_cursor_list);
  }

  void GatherEmbeddings(const EmbeddingVarContext<GPUDevice>& context,
                        const Tensor& keys_tensor,
                        ValuePtr<V>** value_ptrs,
                        V* output,
                        int64 num_of_keys) {
    std::vector<V*> embedding_ptr(num_of_keys);
    const K* keys = (K*)keys_tensor.data();
    auto do_work = [this, keys, value_ptrs, output, &embedding_ptr]
        (int64 start, int64 limit) {
      for (int64 i = start; i < limit; ++i) {
        bool is_admit = filter_->is_admit(keys[i], value_ptrs[i]);
        add_freq_fn_(value_ptrs[i], 1, emb_config_.filter_freq);
        if (is_admit) {
          V* default_v =
              default_value_ +
                  (keys[i] % emb_config_.default_value_dim) * value_len_;
          embedding_ptr[i] = LookupOrCreateEmb(value_ptrs[i], default_v);
        } else {
          embedding_ptr[i] = default_value_no_permission_;
        }
      }
    };
    auto worker_threads = context.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers, num_of_keys,
          value_len_ * sizeof(V), do_work);

    auto stream = context.compute_stream;
    auto event_mgr = context.event_mgr;
    CopyEmbeddingsToBuffer(
        output, num_of_keys, embedding_ptr.data(),
        stream, event_mgr, context.gpu_device);

    storage_->AddToCache(keys_tensor);
  }

  void BatchLookupOrCreateEmb(
      const EmbeddingVarContext<GPUDevice>& ctx,
      V** var_ptr,
      ValuePtr<V>** value_ptrs,
      const K* indices,
      int64 num_of_keys,
      IntraThreadCopyIdAllocator* thread_copy_id_alloc) {
    int num_worker_threads = ctx.worker_threads->num_threads;
    std::vector<std::list<int64>> init_cursor_list(
        num_worker_threads + 1);
    uint64 main_thread_id = Env::Default()->GetCurrentThreadId();

    auto do_work_get_ptrs = [this, value_ptrs, &init_cursor_list,
        &thread_copy_id_alloc, main_thread_id, var_ptr] (int64 start, int64 limit) {
      int copy_id =
          thread_copy_id_alloc->GetCopyIdOfThread(main_thread_id);
      for (int i = start; i < limit; i++) {
        bool is_need_set_default_value = false;
        var_ptr[i] = LookupOrCreateEmb(
            value_ptrs[i], is_need_set_default_value);
        if (is_need_set_default_value) {
          init_cursor_list[copy_id].emplace_back(i);
        }
      }
    };
    const int64 unit_cost = 1000;
    auto worker_threads = ctx.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers,
          num_of_keys, unit_cost, do_work_get_ptrs);

    // Merge copies of init_cursor_list
    for (int i = 1; i < (worker_threads->num_threads + 1); i++) {
      if (init_cursor_list[i].size() > 0) {
        init_cursor_list[0].splice(init_cursor_list[0].end(),
                                   init_cursor_list[i]);
      }
    }

    auto stream = ctx.compute_stream;
    auto event_mgr = ctx.event_mgr;

    SetDefaultValueOfNewFeatures(
        indices, num_of_keys,
        init_cursor_list[0],
        var_ptr, stream, event_mgr,
        ctx.gpu_device);
  }
#endif

  void LookupOrCreate(K key, V* val, V* default_v, int count = 1)  {
    const V* default_value_ptr =
      (default_v == nullptr) ? default_value_ : default_v;
    ValuePtr<V>* value_ptr = nullptr;
    filter_->LookupOrCreate(key, val, default_value_ptr, &value_ptr, count,
                            default_value_no_permission_);
    add_freq_fn_(value_ptr, count, emb_config_.filter_freq);
  }

  void BatchInitEmb(int64 size, V** memcpy_address, V* default_value,
      bool* init_flags, int64 value_len) {
    filter_->BatchInitEmb(size, memcpy_address, default_value,
        init_flags, value_len);
  }

#if GOOGLE_CUDA
  void CopyEmbeddingsToBuffer(
      V* val_base, int64 size,
      V** memcpy_address,
      se::Stream* compute_stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device);

  void SetDefaultValueOfNewFeatures(
      const K* keys, int64 size,
      const std::list<int64>& init_cursor,
      V** memcpy_address,
      se::Stream* compute_stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device);

  void CopyEmbeddingsFromCPUToGPU(
      const K* keys,
      const std::list<int64>& copyback_cursor,
      V** memcpy_address,
      se::Stream* compute_stream,
      EventMgr* event_mgr,
      const Eigen::GpuDevice& gpu_device,
      const DeviceBase::CpuWorkerThreads* worker_threads,
      int64* output_value_ptrs = nullptr);

  void AllocateMemoryForNewFeatures(
      V** memcpy_address,
      const std::list<int64>& init_cursor) {
    std::vector<ValuePtr<V>*> value_ptr_list;
    for (auto it = init_cursor.cbegin();
      it != init_cursor.cend(); ++it) {
      ValuePtr<V>* value_ptr =
          reinterpret_cast<ValuePtr<V>*>(memcpy_address[*it]);
      value_ptr_list.emplace_back(value_ptr);
    }
    storage_->AllocateMemoryForNewFeatures(value_ptr_list);
  }
#endif  // GOOGLE_CUDA

  V* LookupOrCreateEmb(ValuePtr<V>* value_ptr, const V* default_v) {
    return value_ptr->GetOrAllocate(alloc_, value_len_, default_v,
        emb_config_.emb_index, storage_->GetOffset(
          emb_config_.emb_index));
  }

  V* LookupOrCreateEmb(ValuePtr<V>* value_ptr, const V* default_v,
                       Allocator* alloc) {
    return value_ptr->GetOrAllocate(alloc, value_len_, default_v,
        emb_config_.emb_index, storage_->GetOffset(
            emb_config_.emb_index));
  }

  V* LookupOrCreateEmb(ValuePtr<V>* value_ptr, bool &need_initialize) {
    return value_ptr->GetOrAllocate(alloc_, value_len_, nullptr,
        emb_config_.emb_index,
        storage_->GetOffset(emb_config_.emb_index),
        need_initialize);
  }

  V* LookupPrimaryEmb(ValuePtr<V>* value_ptr) {
    V* primary_val = value_ptr->GetValue(emb_config_.primary_emb_index,
        storage_->GetOffset(emb_config_.primary_emb_index));
    return primary_val;
  }

  typename TTypes<V>::Flat flat(ValuePtr<V>* value_ptr, int64 index) {
    V* default_v =
        default_value_ + (index % emb_config_.default_value_dim) * value_len_;
    V* val = LookupOrCreateEmb(value_ptr, default_v);
    Eigen::array<Eigen::DenseIndex, 1> dims({value_len_});
    return typename TTypes<V>::Flat(val, dims);
  }

  int64 ValueLen() const {
    return value_len_;
  }

  int64 Size() const {
    return storage_->Size();
  }

  int64 CacheSize() const {
    return storage_->CacheSize();
  }

  int64 MinFreq() {
    return emb_config_.filter_freq;
  }

  int64 StepsToLive() const {
    return emb_config_.steps_to_live;
  }

  bool IsMultiLevel() {
    return storage_->IsMultiLevel();
  }

  bool IsUseHbm() {
    return storage_->IsUseHbm();
  }

  bool IsSingleHbm() {
    return storage_->IsSingleHbm();
  }

  bool IsUsePersistentStorage() {
    return storage_->IsUsePersistentStorage();
  }

  void InitCache(embedding::CacheStrategy cache_strategy) {
    storage_->InitCache(cache_strategy);
  }

  std::string DebugString() const {
    return emb_config_.DebugString();
  }

  void Restore(const std::string& name_string,
               const std::string& file_name_string, int64 partition_id,
               int64 partition_num, bool is_incr, BundleReader* reader,
               bool reset_version = false,
               const Eigen::GpuDevice* device = nullptr) {
    return storage_->Restore(name_string, file_name_string, partition_id,
                             partition_num, value_len_, is_incr, reset_version,
                             emb_config_, device, reader, this, filter_);
  }

  Status Save(const string& tensor_name,
              const string& prefix,
              BundleWriter* writer,
              embedding::ShrinkArgs& shrink_args) {
    return storage_->Save(tensor_name, prefix,
                          writer, emb_config_,
                          shrink_args, value_len_,
                          default_value_);
  }

  void GetSnapshot(std::vector<K>* key_list,
                   std::vector<V*>* value_list,
                   std::vector<int64>* version_list,
                   std::vector<int64>* freq_list) {
    std::vector<ValuePtr<V>*> value_ptr_list;
    storage_->GetSnapshot(key_list, &value_ptr_list);
    bool is_save_freq = emb_config_.is_save_freq();
    bool is_save_version = emb_config_.is_save_version();
    for (int64 i = 0; i < key_list->size(); i++) {
      V* val = value_ptr_list[i]->GetValue(emb_config_.emb_index, 0);
      if (val != nullptr) {
        value_list->emplace_back(val);
      } else {
        value_list->emplace_back(default_value_);
      }

      if(is_save_version) {
        int64 dump_version = value_ptr_list[i]->GetStep();
        version_list->emplace_back(dump_version);
      }

      if(is_save_freq) {
        int64 dump_freq = value_ptr_list[i]->GetFreq();
        freq_list->emplace_back(dump_freq);
      }
    }
  }

  mutex* mu() {
    return &mu_;
  }

  embedding::Storage<K, V>* storage() {
    return storage_;
  }

  Status Shrink(embedding::ShrinkArgs& shrink_args) {
    if (emb_config_.is_primary()) {
      shrink_args.value_len = value_len_;
      return storage_->Shrink(shrink_args);
    } else {
      return Status::OK();
    }
  }

  V* GetDefaultValuePtr() {
    return default_value_;
  }

  int64 GetDefaultValueDim() {
    return emb_config_.default_value_dim;
  }

  V* GetDefaultValue(int64 key) {
    return default_value_ + (key % emb_config_.default_value_dim) * value_len_;
  }

  embedding::BatchCache<K>* Cache() {
    return storage_->Cache();
  }

  int64 GetEmbeddingIndex() {
    return emb_config_.emb_index;
  }

  int64 GetEmbeddingSlotNum() {
    return emb_config_.slot_num;
  }
  
  Allocator* GetAllocator() {
    return alloc_;
  }

  int64 GetAllocLen() {
    return emb_config_.total_num(storage_->GetAllocLen());
  }

  V** GetBuffer(int64 size) {
    if (dev_addr_buffer_size_ >= size) {
      return dev_addr_buffer_;
    } else {
      if (dev_addr_buffer_size_ != 0) {
        alloc_->DeallocateRaw(dev_addr_buffer_);
      }
      dev_addr_buffer_ =
          (V**)alloc_->AllocateRaw(
              Allocator::kAllocatorAlignment,
              size * sizeof(V*));
      dev_addr_buffer_size_ = size;
      return dev_addr_buffer_;
    }
  }

  void UpdateCache(const Tensor& indices,
                   const Tensor& indices_counts,
                   bool is_called_by_gather = false) {
    if (!is_called_by_gather ||
        (is_called_by_gather && emb_config_.is_inference)) {
      storage_->UpdateCache(indices, indices_counts);
    }
  }

  void UpdateCache(const Tensor& indices,
                   bool is_called_by_gather = false) {
    if (!is_called_by_gather ||
        (is_called_by_gather && emb_config_.is_inference)) {
      storage_->UpdateCache(indices);
    }
  }

  void UpdateCache(const K* key_buff, int64 key_num,
      const int64* version_buff, const int64* freq_buff) {
    auto cache = Cache();
    if (cache) {
      cache->update(key_buff, key_num, version_buff, freq_buff);
      auto cache_size = CacheSize();
      if (cache->size() > cache_size) {
        int64 evict_size = cache->size() - cache_size;
        K* evict_ids = new K[evict_size];
        size_t true_size = cache->get_evic_ids(evict_ids, evict_size);
        if (!IsUseHbm()) {
          Eviction(evict_ids, true_size);
        }
        delete []evict_ids;
      }
    }
  }

  void LookupOrCreate(const K* key, V* val, V* default_v,
      int32 default_v_num, size_t n, const Eigen::GpuDevice& device) {
    storage_->BatchLookupOrCreate(key, val, default_v, default_v_num,
        n, device);
  }

  void LookupOrCreateKey(const K* key, int32* item_idxs, size_t n,
      const Eigen::GpuDevice& device, int64 update_version = -1) {
    storage_->BatchLookupOrCreateKeys(key, item_idxs, n, device);
  }

  void Lookup(const K* key, V* val, V* default_v,
      int32 default_v_num,
      size_t n, const Eigen::GpuDevice& device) {
    storage_->BatchLookup(key, val, default_v, default_v_num,
        n, device);
  }
  
  int32 SlotNum() {
    return (emb_config_.block_num * (1 + emb_config_.slot_num));
  }

  int32 EmbIdx() {
    return emb_config_.emb_index;
  }

  GPUHashTable<K, V>* HashTable() {
    return storage_->HashTable();
  }

 protected:
  FilterPolicy<K, V, EmbeddingVar<K, V>>* GetFilter() const {
    return filter_;
  }

  ~EmbeddingVar() override {
    // When dynamic dimension embedding is used,
    // there will be more than one primary slot
    if (emb_config_.is_primary() && emb_config_.primary_emb_index == 0) {
      delete storage_;
    }
    if (embedding::StorageType::HBM_DRAM == storage_type_) {
      alloc_->DeallocateRaw(dev_addr_buffer_);
    }
    TypedAllocator::Deallocate(default_value_alloc_, default_value_,
        value_len_ * emb_config_.default_value_dim);
    if (default_value_no_permission_) {
      TypedAllocator::Deallocate(default_value_alloc_,
          default_value_no_permission_,
          value_len_);
    }
    if (filter_) {
      delete filter_;
    }
  }

 private:
  void LookupThroughFilter(
      const EmbeddingVarContext<CPUDevice>& context,
      const Tensor& indices, V* output,
      int64 num_of_keys) {
    const K* keys = (K*)indices.data();
    auto do_work = [this, keys, output] (int64 start, int64 limit) {
      for (int64 i = start; i < limit; ++i) {
        V* default_v =
            default_value_ +
                (keys[i] % emb_config_.default_value_dim) * value_len_;
        filter_->Lookup(keys[i],
            output + i * value_len_, default_v,
            default_value_no_permission_);
      }
    };
    auto worker_threads = context.worker_threads;
    Shard(worker_threads->num_threads,
          worker_threads->workers, num_of_keys,
          value_len_ * sizeof(V), do_work);
  }

  V* GetAddressOfGpuValuePtr(ValuePtr<V>* value_ptr,
      int64 index,
      bool copyback_flag,
      std::list<int64>& init_cursor,
      std::list<int64>& copyback_cursor) {
    V* mem_addr = nullptr;
    bool init_flag = false;
    if (!copyback_flag) {
      mem_addr = LookupOrCreateEmb(value_ptr, init_flag);
    } else {
      mem_addr = value_ptr->GetValue(0,0);
      if (copyback_flag ==
          embedding::CopyBackFlag::COPYBACK_AND_DESTROY) {
        delete value_ptr;
        // If the 64th bit of cursor is set to 1,
        // the corresponding valueptr need to be deleted later.
        int64 tmp = 1;
        tmp = tmp << 63;
        copyback_cursor.emplace_back(index | tmp);
      } else {
        copyback_cursor.emplace_back(index);
      }
    }
    if (init_flag) {
      init_cursor.emplace_back(index);
    }
    return mem_addr;
  }

  std::string name_;
  bool is_initialized_ = false;

  mutex mu_;

  V* default_value_;
  V* default_value_no_permission_;
  V** dev_addr_buffer_;
  int64 dev_addr_buffer_size_;
  int64 value_len_;
  Allocator* alloc_;
  Allocator* default_value_alloc_;
  embedding::Storage<K, V>* storage_;
  embedding::StorageType storage_type_;
  EmbeddingConfig emb_config_;
  FilterPolicy<K, V, EmbeddingVar<K, V>>* filter_;
  std::function<void(ValuePtr<V>*, int64, int64)> add_freq_fn_;
  std::function<void(ValuePtr<V>*, int64)> update_version_fn_;

  TF_DISALLOW_COPY_AND_ASSIGN(EmbeddingVar);
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_EMBEDDING_VAR_H_
