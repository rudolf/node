#include "node_heap_profile_labels.h"

#include "node_internals.h"
#include "node_v8_embedder.h"
#include "util-inl.h"
#include "v8-profiler.h"

namespace node {
namespace heap_profile_labels {

using v8::Array;
using v8::Context;
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

MaybeLocal<String> NewString(Isolate* isolate, const std::string& value) {
  return String::NewFromUtf8(
      isolate, value.data(), NewStringType::kNormal, value.size());
}

MaybeLocal<Object> NewLabelsObject(Local<Context> context,
                                   const LabelSet* set) {
  Isolate* isolate = Isolate::GetCurrent();
  Local<Object> object = Object::New(isolate);
  if (set != nullptr) {
    for (const auto& [name, value] : set->labels()) {
      Local<String> js_name;
      Local<String> js_value;
      if (!NewString(isolate, name).ToLocal(&js_name) ||
          !NewString(isolate, value).ToLocal(&js_value) ||
          object->CreateDataProperty(context, js_name, js_value).IsNothing()) {
        return {};
      }
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
}

void ExternalMemoryTracker::TrackAllocate(void* data, size_t size) {
  // The allocator is shared by every thread of the process that allocates
  // backing stores, but only the session's isolate thread may read CPED.
  Isolate* isolate = isolate_.load(std::memory_order_acquire);
  if (data == nullptr || isolate == nullptr ||
      Isolate::TryGetCurrent() != isolate) {
    return;
  }
  LabelSet* set = session_->CaptureCurrent();
  if (set == nullptr) return;
  Mutex::ScopedLock lock(mutex_);
  entries_[data] = {set, size};
}

void ExternalMemoryTracker::TrackFree(void* data) {
  Mutex::ScopedLock lock(mutex_);
  entries_.erase(data);
}

std::unordered_map<LabelSet*, uint64_t> ExternalMemoryTracker::LiveBytes()
    const {
  std::unordered_map<LabelSet*, uint64_t> result;
  Mutex::ScopedLock lock(mutex_);
  for (const auto& entry : entries_) {
    result[entry.second.set] += entry.second.size;
  }
  return result;
}

HeapProfileSession::HeapProfileSession(Isolate* isolate,
                                       const void* owner,
                                       LabelRegistry* registry,
                                       NodeArrayBufferAllocator* allocator)
    : isolate_(isolate),
      owner_(owner),
      id_(++next_session_id),
      registry_(registry) {
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
    LabelRegistry* registry,
    NodeArrayBufferAllocator* allocator) {
  // The previous session must release the external tracker first.
  Destroy(isolate);
  auto session = std::unique_ptr<HeapProfileSession>(
      new HeapProfileSession(isolate, owner, registry, allocator));
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
  return session != nullptr ? session->CaptureCurrent() : nullptr;
}

LabelSet* HeapProfileSession::CaptureCurrent() {
  if (registry_ == nullptr) return nullptr;
  void* context = v8::GetAlignedPointerFromContinuationPreservedEmbedderDataMap(
      isolate_, registry_->als_key());
  if (context == nullptr) return nullptr;
  LabelSet* set = static_cast<LabelSet*>(context);
  auto [it, inserted] = captured_.try_emplace(set, reads_);
  if (inserted) {
    set->Ref();
  } else {
    it->second = reads_;
  }
  return set;
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
  auto labels_object = [&](LabelSet* set) -> MaybeLocal<Object> {
    auto it = labels_objects.find(set);
    if (it != labels_objects.end()) return it->second;
    Local<Object> object;
    if (!NewLabelsObject(context, set).ToLocal(&object)) return {};
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

void HeapProfileSession::Compact(
    uint64_t read, const std::unordered_set<LabelSet*>& referenced) {
  for (auto it = captured_.begin(); it != captured_.end();) {
    if (it->second >= read || referenced.count(it->first) > 0) {
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
