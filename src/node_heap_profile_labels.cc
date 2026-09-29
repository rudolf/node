#include "node_heap_profile_labels.h"

#include "node_internals.h"
#include "node_v8_embedder.h"
#include "util-inl.h"
#include "v8-profiler.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace node {
namespace heap_profile_labels {

using v8::Array;
using v8::BigInt;
using v8::Boolean;
using v8::Context;
using v8::DictionaryTemplate;
using v8::EscapableHandleScope;
using v8::HandleScope;
using v8::IntegrityLevel;
using v8::Isolate;
using v8::Local;
using v8::LocalVector;
using v8::MaybeLocal;
using v8::Name;
using v8::NewStringType;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::String;
using v8::Uint32;
using v8::Undefined;
using v8::Value;
using v8::WeakCallbackInfo;
using v8::WeakCallbackType;

namespace {

Mutex& SessionsMutex() {
  static Mutex mutex;
  return mutex;
}

std::unordered_map<Isolate*, std::unique_ptr<HeapProfileSession>>& Sessions() {
  static auto* sessions =
      new std::unordered_map<Isolate*, std::unique_ptr<HeapProfileSession>>();
  return *sessions;
}

std::atomic<uint32_t> next_session_id{0};

constexpr size_t kMaxGroupValueLength = 255;

MaybeLocal<String> NewString(Isolate* isolate, const std::string& value) {
  return String::NewFromUtf8(
      isolate, value.data(), NewStringType::kNormal, value.size());
}

MaybeLocal<Object> NewLabelsObject(Local<Context> context,
                                   const LabelSet::Labels& labels) {
  Isolate* isolate = Isolate::GetCurrent();
  Local<Object> object = Object::New(isolate);
  for (const auto& [name, value] : labels) {
    Local<String> js_name;
    Local<String> js_value;
    if (!NewString(isolate, name).ToLocal(&js_name) ||
        !NewString(isolate, value).ToLocal(&js_value) ||
        object->CreateDataProperty(context, js_name, js_value).IsNothing()) {
      return {};
    }
  }
  if (object->SetIntegrityLevel(context, IntegrityLevel::kFrozen).IsNothing()) {
    return {};
  }
  return object;
}

}  // namespace

void LabelSet::Unref() {
  CHECK_GT(refs_, 0);
  if (--refs_ > 0) return;
  auto& sets = registry_->sets_;
  sets.erase(sets.find(key_));
}

LabelRegistry::~LabelRegistry() {
  HandleScope scope(isolate_);
  for (const auto& entry : sets_) {
    LabelSet* set = entry.second.get();
    if (set->holder_.IsEmpty()) continue;
    set->holder_.Get(isolate_)->SetAlignedPointerInInternalField(
        0, nullptr, EmbedderDataTag::kDefault);
    set->holder_.Reset();
  }
}

MaybeLocal<Object> LabelRegistry::GetHolder(Local<Context> context,
                                            Local<Array> flat) {
  std::string key;
  LabelSet::Labels labels;
  const uint32_t length = flat->Length();
  labels.reserve(length / 2);
  for (uint32_t i = 0; i + 1 < length; i += 2) {
    Local<Value> name;
    Local<Value> value;
    if (!flat->Get(context, i).ToLocal(&name) ||
        !flat->Get(context, i + 1).ToLocal(&value)) {
      return {};
    }
    CHECK(name->IsString());
    CHECK(value->IsString());
    std::string name_utf8 = Utf8Value(isolate_, name).ToString();
    std::string value_utf8 = Utf8Value(isolate_, value).ToString();
    for (const std::string* part : {&name_utf8, &value_utf8}) {
      key += std::to_string(part->size());
      key += ':';
      key += *part;
    }
    labels.emplace_back(std::move(name_utf8), std::move(value_utf8));
  }

  auto it = sets_.find(key);
  if (it != sets_.end() && !it->second->holder_.IsEmpty()) {
    return it->second->holder_.Get(isolate_);
  }

  if (holder_template_.IsEmpty()) {
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate_);
    templ->SetInternalFieldCount(1);
    holder_template_.Reset(isolate_, templ);
  }
  Local<Object> holder;
  if (!holder_template_.Get(isolate_)->NewInstance(context).ToLocal(&holder)) {
    return {};
  }
  if (it == sets_.end()) {
    auto set = std::make_unique<LabelSet>(this, key, std::move(labels));
    it = sets_.emplace(std::move(key), std::move(set)).first;
  }
  LabelSet* set = it->second.get();
  holder->SetAlignedPointerInInternalField(0, set, EmbedderDataTag::kDefault);
  set->holder_.Reset(isolate_, holder);
  set->holder_.SetWeak(set, OnHolderCollected, WeakCallbackType::kParameter);
  set->Ref();
  return holder;
}

void LabelRegistry::OnHolderCollected(const WeakCallbackInfo<LabelSet>& info) {
  LabelSet* set = info.GetParameter();
  set->holder_.Reset();
  set->Unref();
}

bool ExternalMemoryTracker::Enable(Isolate* isolate,
                                   HeapProfileSession* session) {
  if (active()) return false;
  session_ = session;
  isolate_.store(isolate, std::memory_order_release);
  return true;
}

void ExternalMemoryTracker::Disable() {
  isolate_.store(nullptr, std::memory_order_relaxed);
  session_ = nullptr;
  Mutex::ScopedLock lock(mutex_);
  entries_.clear();
  stats_.clear();
}

void ExternalMemoryTracker::TrackAllocate(void* data, size_t size) {
  // The allocator is shared by every thread of the process that allocates
  // backing stores, but only the session's isolate thread may read CPED.
  Isolate* isolate = isolate_.load(std::memory_order_acquire);
  if (data == nullptr || isolate == nullptr ||
      Isolate::TryGetCurrent() != isolate) {
    return;
  }
  auto [set, group] = session_->CaptureCurrent();
  const bool groups = session_->groups();
  if (set == nullptr && !groups) return;
  Mutex::ScopedLock lock(mutex_);
  entries_[data] = {set, size, group};
  if (!groups) return;
  if (group >= stats_.size()) stats_.resize(group + 1);
  GroupStats& stats = stats_[group];
  stats.allocated_bytes += size;
  stats.allocated_count++;
  stats.current_bytes += size;
  stats.window_peak_bytes =
      std::max(stats.window_peak_bytes, stats.current_bytes);
}

void ExternalMemoryTracker::TrackFree(void* data) {
  Mutex::ScopedLock lock(mutex_);
  auto it = entries_.find(data);
  if (it == entries_.end()) return;
  if (it->second.group < stats_.size()) {
    GroupStats& stats = stats_[it->second.group];
    stats.freed_bytes += it->second.size;
    stats.freed_count++;
    stats.current_bytes -= it->second.size;
  }
  entries_.erase(it);
}

std::unordered_map<LabelSet*, uint64_t> ExternalMemoryTracker::LiveBytes()
    const {
  std::unordered_map<LabelSet*, uint64_t> result;
  Mutex::ScopedLock lock(mutex_);
  for (const auto& entry : entries_) {
    if (entry.second.set == nullptr) continue;
    result[entry.second.set] += entry.second.size;
  }
  return result;
}

std::vector<ExternalMemoryTracker::GroupStats> ExternalMemoryTracker::Stats(
    bool advance_peak_window) {
  Mutex::ScopedLock lock(mutex_);
  if (advance_peak_window) {
    for (GroupStats& stats : stats_) {
      stats.published_peak_bytes = stats.window_peak_bytes;
      stats.window_peak_bytes = stats.current_bytes;
    }
  }
  return stats_;
}

HeapProfileSession::HeapProfileSession(Isolate* isolate,
                                       const void* owner,
                                       NodeArrayBufferAllocator* allocator,
                                       Options options)
    : isolate_(isolate),
      owner_(owner),
      id_(++next_session_id),
      sample_interval_(options.sample_interval),
      registry_(options.registry),
      group_by_(std::move(options.group_by)),
      max_groups_(options.max_groups) {
  if (groups()) groups_.resize(kOverflowGroup + 1);
  if (registry_ != nullptr && allocator != nullptr &&
      allocator->heap_profile_tracker()->Enable(isolate, this)) {
    tracker_ = allocator->heap_profile_tracker();
  }
}

HeapProfileSession::~HeapProfileSession() {
  if (tracker_ != nullptr) tracker_->Disable();
  for (const auto& entry : captured_) entry.first->Unref();
}

HeapProfileSession* HeapProfileSession::Start(
    Isolate* isolate,
    const void* owner,
    NodeArrayBufferAllocator* allocator,
    Options options) {
  // The previous session must release the external tracker first.
  Destroy(isolate);
  auto session = std::unique_ptr<HeapProfileSession>(
      new HeapProfileSession(isolate, owner, allocator, std::move(options)));
  HeapProfileSession* result = session.get();
  Mutex::ScopedLock lock(SessionsMutex());
  Sessions()[isolate] = std::move(session);
  return result;
}

HeapProfileSession* HeapProfileSession::Get(Isolate* isolate) {
  Mutex::ScopedLock lock(SessionsMutex());
  auto it = Sessions().find(isolate);
  return it != Sessions().end() ? it->second.get() : nullptr;
}

bool HeapProfileSession::Destroy(Isolate* isolate, const void* owner) {
  std::unique_ptr<HeapProfileSession> session;
  {
    Mutex::ScopedLock lock(SessionsMutex());
    auto it = Sessions().find(isolate);
    if (it == Sessions().end() ||
        (owner != nullptr && it->second->owner_ != owner)) {
      return false;
    }
    session = std::move(it->second);
    Sessions().erase(it);
  }
  return true;
}

void* HeapProfileSession::ExtractSampleContext(Isolate* isolate) {
  HeapProfileSession* session = Get(isolate);
  if (session == nullptr) return nullptr;
  Capture capture = session->CaptureCurrent();
  if (session->groups()) session->groups_[capture.group].allocated_samples++;
  return capture.set;
}

HeapProfileSession::Capture HeapProfileSession::CaptureCurrent() {
  if (registry_ == nullptr) return {nullptr, kUnknownGroup};
  void* context = v8::GetAlignedPointerFromContinuationPreservedEmbedderDataMap(
      isolate_, registry_->als_key());
  if (context == nullptr) return {nullptr, kUnknownGroup};
  LabelSet* set = static_cast<LabelSet*>(context);
  auto [it, inserted] = captured_.try_emplace(set, Captured{reads_, 0});
  if (inserted) {
    set->Ref();
    it->second.group = groups() ? ResolveGroup(set) : kUnknownGroup;
  } else {
    it->second.read = reads_;
  }
  return {set, it->second.group};
}

uint32_t HeapProfileSession::ResolveGroup(const LabelSet* set) {
  std::string key;
  LabelSet::Labels labels;
  for (size_t i = 0; i < group_by_.size(); i++) {
    const std::string& name = group_by_[i];
    auto it = std::find_if(
        set->labels().begin(), set->labels().end(), [&](const auto& label) {
          return label.first == name;
        });
    if (it == set->labels().end() || it->second.empty()) continue;
    const std::string& value = it->second;
    if (value.size() > kMaxGroupValueLength) {
      overflow_count_++;
      return kOverflowGroup;
    }
    key += std::to_string(i) + ':' + std::to_string(value.size()) + ':';
    key += value;
    labels.emplace_back(name, value);
  }
  if (labels.empty()) return kUnknownGroup;
  auto it = group_index_.find(key);
  if (it != group_index_.end()) return it->second;
  if (groups_.size() - (kOverflowGroup + 1) >= max_groups_) {
    overflow_count_++;
    return kOverflowGroup;
  }
  const uint32_t group = groups_.size();
  groups_.push_back({std::move(labels)});
  group_index_.emplace(std::move(key), group);
  return group;
}

MaybeLocal<Value> HeapProfileSession::GetAllocationProfile(
    Local<Context> context) {
  Isolate* isolate = isolate_;
  EscapableHandleScope scope(isolate);
  const uint64_t read = ++reads_;
  std::unique_ptr<v8::AllocationProfile> profile(
      isolate->GetHeapProfiler()->GetAllocationProfile());
  if (!profile) return scope.Escape(Undefined(isolate));

  std::unordered_set<LabelSet*> referenced;
  std::unordered_map<LabelSet*, Local<Object>> labels_objects;
  const LabelSet::Labels no_labels;
  auto labels_object = [&](LabelSet* set) -> MaybeLocal<Object> {
    auto it = labels_objects.find(set);
    if (it != labels_objects.end()) return it->second;
    Local<Object> object;
    if (!NewLabelsObject(context, set != nullptr ? set->labels() : no_labels)
             .ToLocal(&object)) {
      return {};
    }
    labels_objects.emplace(set, object);
    return object;
  };

  Local<Name> node_id_string = FIXED_ONE_BYTE_STRING(isolate, "nodeId");
  Local<Name> size_string = FIXED_ONE_BYTE_STRING(isolate, "size");
  Local<Name> count_string = FIXED_ONE_BYTE_STRING(isolate, "count");
  Local<Name> sample_id_string = FIXED_ONE_BYTE_STRING(isolate, "sampleId");
  Local<Name> labels_string = FIXED_ONE_BYTE_STRING(isolate, "labels");

  const auto& samples = profile->GetSamples();
  LocalVector<Value> js_samples(isolate);
  js_samples.reserve(samples.size());
  for (size_t i = 0; i < samples.size(); i++) {
    const v8::AllocationProfile::Sample& sample = samples[i];
    LabelSet* set = static_cast<LabelSet*>(profile->GetSampleContext(i));
    if (set != nullptr) referenced.insert(set);
    Local<Object> labels;
    if (!labels_object(set).ToLocal(&labels)) return {};
    Local<Object> js_sample = Object::New(isolate);
    if (js_sample
            ->CreateDataProperty(
                context,
                node_id_string,
                Uint32::NewFromUnsigned(isolate, sample.node_id))
            .IsNothing() ||
        js_sample
            ->CreateDataProperty(
                context,
                size_string,
                Number::New(isolate, static_cast<double>(sample.size)))
            .IsNothing() ||
        js_sample
            ->CreateDataProperty(context,
                                 count_string,
                                 Uint32::NewFromUnsigned(isolate, sample.count))
            .IsNothing() ||
        js_sample
            ->CreateDataProperty(
                context,
                sample_id_string,
                Number::New(isolate, static_cast<double>(sample.sample_id)))
            .IsNothing() ||
        js_sample->CreateDataProperty(context, labels_string, labels)
            .IsNothing()) {
      return {};
    }
    js_samples.push_back(js_sample);
  }
  profile.reset();

  Local<Object> result = Object::New(isolate);
  if (result
          ->CreateDataProperty(
              context,
              FIXED_ONE_BYTE_STRING(isolate, "samples"),
              Array::New(isolate, js_samples.data(), js_samples.size()))
          .IsNothing()) {
    return {};
  }

  if (tracker_ != nullptr) {
    std::unordered_map<LabelSet*, uint64_t> live = tracker_->LiveBytes();
    if (!live.empty()) {
      Local<Name> bytes_string = FIXED_ONE_BYTE_STRING(isolate, "bytes");
      LocalVector<Value> entries(isolate);
      entries.reserve(live.size());
      for (const auto& [set, bytes] : live) {
        referenced.insert(set);
        Local<Object> labels;
        if (!labels_object(set).ToLocal(&labels)) return {};
        Local<Object> entry = Object::New(isolate);
        if (entry->CreateDataProperty(context, labels_string, labels)
                .IsNothing() ||
            entry
                ->CreateDataProperty(
                    context,
                    bytes_string,
                    Number::New(isolate, static_cast<double>(bytes)))
                .IsNothing()) {
          return {};
        }
        entries.push_back(entry);
      }
      if (result
              ->CreateDataProperty(
                  context,
                  FIXED_ONE_BYTE_STRING(isolate, "externalBytes"),
                  Array::New(isolate, entries.data(), entries.size()))
              .IsNothing()) {
        return {};
      }
    }
  }

  Compact(read, referenced);
  return scope.Escape(result);
}

MaybeLocal<Value> HeapProfileSession::GetHeapStats(Local<Context> context,
                                                   bool advance_peak_window) {
  Isolate* isolate = isolate_;
  EscapableHandleScope scope(isolate);
  if (!groups()) return scope.Escape(Undefined(isolate));
  const uint64_t read = ++reads_;
  std::unique_ptr<v8::AllocationProfile> profile(
      isolate->GetHeapProfiler()->GetAllocationProfile());
  if (!profile) return scope.Escape(Undefined(isolate));

  struct HeapStats {
    uint64_t samples = 0;
    double bytes = 0;
  };
  std::vector<HeapStats> heap(groups_.size());
  std::unordered_set<LabelSet*> referenced;
  const auto& samples = profile->GetSamples();
  for (size_t i = 0; i < samples.size(); i++) {
    const v8::AllocationProfile::Sample& sample = samples[i];
    LabelSet* set = static_cast<LabelSet*>(profile->GetSampleContext(i));
    uint32_t group = kUnknownGroup;
    if (set != nullptr) {
      referenced.insert(set);
      auto it = captured_.find(set);
      CHECK(it != captured_.end());
      group = it->second.group;
    }
    if (!sample.is_live) continue;
    // The scaling V8 applies to allocation profile nodes.
    const double size = static_cast<double>(sample.size);
    heap[group].samples++;
    heap[group].bytes +=
        size / -std::expm1(-size / static_cast<double>(sample_interval_));
  }
  profile.reset();

  std::vector<ExternalMemoryTracker::GroupStats> external;
  if (tracker_ != nullptr) {
    external = tracker_->Stats(advance_peak_window);
    for (const auto& entry : tracker_->LiveBytes()) {
      referenced.insert(entry.first);
    }
  }
  if (advance_peak_window) peak_window_id_++;
  external.resize(heap.size());

  static constexpr std::string_view group_names[] = {
      "labels",
      "isUnknown",
      "isOverflow",
      "allocatedHeapSampleCount",
      "currentHeapSampleCount",
      "currentHeapBytes",
      "allocatedExternalBytes",
      "allocatedExternalCount",
      "freedExternalBytes",
      "freedExternalCount",
      "currentExternalBytes",
      "publishedPeakExternalBytes",
  };
  Local<DictionaryTemplate> group_template =
      DictionaryTemplate::New(isolate, group_names);
  auto big = [&](uint64_t value) -> Local<Value> {
    return BigInt::NewFromUnsigned(isolate, value);
  };
  LocalVector<Value> js_groups(isolate);
  for (uint32_t i = 0; i < heap.size(); i++) {
    const Group& group = groups_[i];
    const ExternalMemoryTracker::GroupStats& ext = external[i];
    if (group.allocated_samples == 0 && heap[i].samples == 0 &&
        ext.allocated_count == 0) {
      continue;
    }
    Local<Object> labels;
    if (!NewLabelsObject(context, group.labels).ToLocal(&labels)) return {};
    MaybeLocal<Value> values[] = {
        labels,
        Boolean::New(isolate, i == kUnknownGroup),
        Boolean::New(isolate, i == kOverflowGroup),
        big(group.allocated_samples),
        big(heap[i].samples),
        big(static_cast<uint64_t>(heap[i].bytes + 0.5)),
        big(ext.allocated_bytes),
        big(ext.allocated_count),
        big(ext.freed_bytes),
        big(ext.freed_count),
        big(ext.current_bytes),
        big(ext.published_peak_bytes),
    };
    Local<Object> js_group;
    if (!NewDictionaryInstance(context, group_template, values)
             .ToLocal(&js_group)) {
      return {};
    }
    js_groups.push_back(js_group);
  }

  static constexpr std::string_view names[] = {
      "sampleInterval",
      "peakWindowId",
      "overflowCount",
      "groups",
  };
  MaybeLocal<Value> values[] = {
      Number::New(isolate, static_cast<double>(sample_interval_)),
      big(peak_window_id_),
      big(overflow_count_),
      Array::New(isolate, js_groups.data(), js_groups.size()),
  };
  Local<Object> result;
  if (!NewDictionaryInstance(
           context, DictionaryTemplate::New(isolate, names), values)
           .ToLocal(&result)) {
    return {};
  }
  Compact(read, referenced);
  return scope.Escape(result);
}

void HeapProfileSession::Compact(
    uint64_t read, const std::unordered_set<LabelSet*>& referenced) {
  for (auto it = captured_.begin(); it != captured_.end();) {
    if (it->second.read >= read || referenced.count(it->first) > 0) {
      ++it;
      continue;
    }
    LabelSet* set = it->first;
    it = captured_.erase(it);
    set->Unref();
  }
}

}  // namespace heap_profile_labels
}  // namespace node
