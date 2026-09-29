#include <cstdint>
#include <memory>

#include "gtest/gtest.h"
#include "node_test_fixture.h"
#include "v8-profiler.h"
#include "v8.h"

namespace {

uintptr_t sample_context_calls = 0;

void* CountingExtractor(v8::Isolate*) {
  return reinterpret_cast<void*>(++sample_context_calls << 1);
}

v8::Local<v8::Object> NewHolder(v8::Isolate* isolate,
                                v8::Local<v8::Context> context,
                                void* ptr) {
  v8::Local<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetInternalFieldCount(1);
  v8::Local<v8::Object> holder = templ->NewInstance(context).ToLocalChecked();
  holder->SetAlignedPointerInInternalField(
      0, ptr, v8::kEmbedderDataTypeTagDefault);
  return holder;
}

void* LookupCped(v8::Isolate* isolate, const v8::Global<v8::Value>& key) {
  return v8::GetAlignedPointerFromContinuationPreservedEmbedderDataMap(isolate,
                                                                      key);
}

v8::Global<v8::Value>* cped_key = nullptr;
void* cped_target = nullptr;
int cped_hits = 0;
int cped_misses = 0;
int cped_unexpected = 0;

void* CpedMapExtractor(v8::Isolate* isolate) {
  void* result = LookupCped(isolate, *cped_key);
  if (result == cped_target) {
    ++cped_hits;
  } else if (result == nullptr) {
    ++cped_misses;
  } else {
    ++cped_unexpected;
  }
  return result;
}

void SetCped(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetIsolate()->SetContinuationPreservedEmbedderDataV2(info[0]);
}

void RunScript(v8::Local<v8::Context> context, const char* source) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::String> code =
      v8::String::NewFromUtf8(isolate, source).ToLocalChecked();
  v8::Script::Compile(context, code)
      .ToLocalChecked()
      ->Run(context)
      .ToLocalChecked();
}

}  // namespace

class HeapSampleContextTest : public NodeTestFixture {
 protected:
  void SetUp() override {
    NodeTestFixture::SetUp();
    v8::V8::SetFlagsFromString("--sampling-heap-profiler-suppress-randomness");
  }
};

TEST_F(HeapSampleContextTest, ExtractorRunsOncePerSample) {
  const v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = v8::Context::New(isolate_);
  v8::Context::Scope context_scope(context);
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();

  sample_context_calls = 0;
  ASSERT_TRUE(heap_profiler->StartSamplingHeapProfiler(
      64,
      16,
      static_cast<v8::HeapProfiler::SamplingFlags>(
          v8::HeapProfiler::kSamplingForceGC |
          v8::HeapProfiler::kSamplingIncludeObjectsCollectedByMajorGC),
      CountingExtractor));
  RunScript(context,
      "var retained = [];"
      "for (var i = 0; i < 500; i++) retained.push(new Array(10));"
      "for (var i = 0; i < 500; i++) new Array(10);");

  std::unique_ptr<v8::AllocationProfile> profile(
      heap_profiler->GetAllocationProfile());
  ASSERT_TRUE(profile);
  const auto& samples = profile->GetSamples();
  ASSERT_FALSE(samples.empty());
  int dead_samples = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    EXPECT_EQ(reinterpret_cast<void*>(samples[i].sample_id << 1),
              profile->GetSampleContext(i));
    if (!samples[i].is_live) ++dead_samples;
  }
  EXPECT_GT(dead_samples, 0);
  EXPECT_GE(sample_context_calls, samples.size());
  EXPECT_EQ(nullptr, profile->GetSampleContext(samples.size()));
  heap_profiler->StopSamplingHeapProfiler();
}

TEST_F(HeapSampleContextTest, NoExtractorHasNoContext) {
  const v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = v8::Context::New(isolate_);
  v8::Context::Scope context_scope(context);
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();

  ASSERT_TRUE(heap_profiler->StartSamplingHeapProfiler(64));
  RunScript(context,
      "var retained = [];"
      "for (var i = 0; i < 100; i++) retained.push(new Array(10));");
  std::unique_ptr<v8::AllocationProfile> profile(
      heap_profiler->GetAllocationProfile());
  ASSERT_TRUE(profile);
  ASSERT_FALSE(profile->GetSamples().empty());
  for (size_t i = 0; i < profile->GetSamples().size(); ++i) {
    EXPECT_EQ(nullptr, profile->GetSampleContext(i));
  }
  heap_profiler->StopSamplingHeapProfiler();
}

TEST_F(HeapSampleContextTest, CpedMapLookup) {
  const v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = v8::Context::New(isolate_);
  v8::Context::Scope context_scope(context);
  int target;

  v8::Local<v8::Object> key = v8::Object::New(isolate_);
  v8::Global<v8::Value> global_key(isolate_, key);
  v8::Local<v8::Map> map = v8::Map::New(isolate_);
  map->Set(context,
           v8::Object::New(isolate_),
           NewHolder(isolate_, context, &target))
      .ToLocalChecked();

  EXPECT_EQ(nullptr, LookupCped(isolate_, v8::Global<v8::Value>()));
  isolate_->SetContinuationPreservedEmbedderDataV2(v8::Object::New(isolate_));
  EXPECT_EQ(nullptr, LookupCped(isolate_, global_key));

  isolate_->SetContinuationPreservedEmbedderDataV2(map);
  EXPECT_EQ(nullptr, LookupCped(isolate_, global_key));
  key->GetIdentityHash();
  EXPECT_EQ(nullptr, LookupCped(isolate_, global_key));

  map->Set(context, key, NewHolder(isolate_, context, &target))
      .ToLocalChecked();
  EXPECT_EQ(&target, LookupCped(isolate_, global_key));

  map->Set(context, key, v8::Object::New(isolate_)).ToLocalChecked();
  EXPECT_EQ(nullptr, LookupCped(isolate_, global_key));
  map->Set(context, key, v8::Number::New(isolate_, 1)).ToLocalChecked();
  EXPECT_EQ(nullptr, LookupCped(isolate_, global_key));

  isolate_->SetContinuationPreservedEmbedderDataV2(v8::Local<v8::Data>());
}

TEST_F(HeapSampleContextTest, ExtractorReadsCpedMapWhileJsSwitchesFrames) {
  const v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = v8::Context::New(isolate_);
  v8::Context::Scope context_scope(context);
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  int target;

  v8::Local<v8::Object> key = v8::Object::New(isolate_);
  v8::Global<v8::Value> global_key(isolate_, key);
  cped_key = &global_key;
  cped_target = &target;
  cped_hits = cped_misses = cped_unexpected = 0;
  v8::Local<v8::Object> global = context->Global();
  auto name = [&](const char* s) {
    return v8::String::NewFromUtf8(isolate_, s).ToLocalChecked();
  };
  ASSERT_TRUE(global->Set(context, name("key"), key).FromJust());
  ASSERT_TRUE(global
                  ->Set(context,
                        name("holder"),
                        NewHolder(isolate_, context, &target))
                  .FromJust());
  ASSERT_TRUE(global
                  ->Set(context,
                        name("setCped"),
                        v8::Function::New(context, SetCped).ToLocalChecked())
                  .FromJust());

  ASSERT_TRUE(heap_profiler->StartSamplingHeapProfiler(
      64, 16, v8::HeapProfiler::kSamplingNoFlags, CpedMapExtractor));
  // Each frame is a new Map, and it grows while it is the current CPED.
  RunScript(context,
      "for (var i = 0; i < 200; i++) {"
      "  var frame = new Map();"
      "  setCped(frame);"
      "  frame.set(key, holder);"
      "  for (var j = 0; j < 20; j++) frame.set({}, j);"
      "  for (var j = 0; j < 20; j++) new Array(10);"
      "  setCped(undefined);"
      "  for (var j = 0; j < 20; j++) new Array(10);"
      "}");
  heap_profiler->StopSamplingHeapProfiler();
  isolate_->SetContinuationPreservedEmbedderDataV2(v8::Local<v8::Data>());
  cped_key = nullptr;

  EXPECT_GT(cped_hits, 0);
  EXPECT_GT(cped_misses, 0);
  EXPECT_EQ(0, cped_unexpected);
}
