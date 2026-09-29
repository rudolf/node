#ifndef SRC_NODE_HEAP_PROFILE_LABELS_H_
#define SRC_NODE_HEAP_PROFILE_LABELS_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "node_mutex.h"
#include "v8.h"

namespace node {

class NodeArrayBufferAllocator;

namespace heap_profile_labels {

class HeapProfileSession;
class LabelRegistry;

// Interned, immutable label content. Referenced by its holder object while
// that is alive and by the session that captured it. Isolate thread only.
class LabelSet {
 public:
  using Labels = std::vector<std::pair<std::string, std::string>>;

  LabelSet(LabelRegistry* registry, std::string key, Labels labels)
      : registry_(registry), key_(std::move(key)), labels_(std::move(labels)) {}
  LabelSet(const LabelSet&) = delete;
  LabelSet& operator=(const LabelSet&) = delete;

  const Labels& labels() const { return labels_; }
  void Ref() { refs_++; }
  void Unref();

 private:
  friend class LabelRegistry;

  LabelRegistry* const registry_;
  const std::string key_;
  const Labels labels_;
  v8::Global<v8::Object> holder_;
  uint32_t refs_ = 0;
};

// Label sets of one realm, keyed by content.
class LabelRegistry {
 public:
  explicit LabelRegistry(v8::Isolate* isolate) : isolate_(isolate) {}
  ~LabelRegistry();
  LabelRegistry(const LabelRegistry&) = delete;
  LabelRegistry& operator=(const LabelRegistry&) = delete;

  // Returns the holder for `flat`, an array of [key, value, ...] strings.
  v8::MaybeLocal<v8::Object> GetHolder(v8::Local<v8::Context> context,
                                       v8::Local<v8::Array> flat);

  void set_als_key(v8::Local<v8::Value> key) { als_key_.Reset(isolate_, key); }
  const v8::Global<v8::Value>& als_key() const { return als_key_; }

 private:
  friend class LabelSet;
  static void OnHolderCollected(const v8::WeakCallbackInfo<LabelSet>& info);

  v8::Isolate* const isolate_;
  v8::Global<v8::Value> als_key_;
  v8::Global<v8::ObjectTemplate> holder_template_;
  std::unordered_map<std::string, std::unique_ptr<LabelSet>> sets_;
};

// Live Buffer and ArrayBuffer backing stores allocated under labels while a
// labelled session runs. Allocations are tracked on the session's isolate
// thread; frees arrive on any thread, including V8's array buffer sweeper.
class ExternalMemoryTracker {
 public:
  bool active() const {
    return isolate_.load(std::memory_order_relaxed) != nullptr;
  }
  bool Enable(v8::Isolate* isolate, HeapProfileSession* session);
  void Disable();

  void TrackAllocate(void* data, size_t size);
  void TrackFree(void* data);

  std::unordered_map<LabelSet*, uint64_t> LiveBytes() const;

 private:
  struct Entry {
    LabelSet* set;
    size_t size;
  };

  std::atomic<v8::Isolate*> isolate_{nullptr};
  HeapProfileSession* session_ = nullptr;
  mutable Mutex mutex_;
  std::unordered_map<void*, Entry> entries_;
};

// Node's state for a sampling heap profiler started by v8.startHeapProfile().
// There is at most one per isolate. Created and destroyed on the isolate
// thread, which is also the only thread that uses it.
class HeapProfileSession {
 public:
  ~HeapProfileSession();
  HeapProfileSession(const HeapProfileSession&) = delete;
  HeapProfileSession& operator=(const HeapProfileSession&) = delete;

  // Creates the session for a sampler just started on `isolate`, replacing
  // any previous session. `registry` is null without labels.
  static HeapProfileSession* Start(v8::Isolate* isolate,
                                   const void* owner,
                                   LabelRegistry* registry,
                                   NodeArrayBufferAllocator* allocator);
  static HeapProfileSession* Get(v8::Isolate* isolate);
  // Destroys the session of `isolate` if it was started by `owner`, or any
  // session if `owner` is null. Returns whether a session was destroyed.
  static bool Destroy(v8::Isolate* isolate, const void* owner = nullptr);

  // The v8::SampleContextExtractor for labelled sessions.
  static void* ExtractSampleContext(v8::Isolate* isolate);

  uint32_t id() const { return id_; }
  bool labels() const { return registry_ != nullptr; }

  // Returns the label set of the current async context, or nullptr.
  LabelSet* CaptureCurrent();

  // Returns the current profile, or undefined if the sampler is stopped.
  v8::MaybeLocal<v8::Value> GetAllocationProfile(
      v8::Local<v8::Context> context);

 private:
  HeapProfileSession(v8::Isolate* isolate,
                     const void* owner,
                     LabelRegistry* registry,
                     NodeArrayBufferAllocator* allocator);

  // Releases label sets that are not in `referenced` and were not captured
  // since read number `read` began.
  void Compact(uint64_t read, const std::unordered_set<LabelSet*>& referenced);

  v8::Isolate* const isolate_;
  const void* const owner_;
  const uint32_t id_;
  LabelRegistry* const registry_;
  ExternalMemoryTracker* tracker_ = nullptr;
  // Captured label sets, each holding a reference, and the read that was in
  // progress when each was last captured.
  std::unordered_map<LabelSet*, uint64_t> captured_;
  uint64_t reads_ = 0;
};

}  // namespace heap_profile_labels
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_HEAP_PROFILE_LABELS_H_
