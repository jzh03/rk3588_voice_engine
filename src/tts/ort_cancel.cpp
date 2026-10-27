#include "onnxruntime_c_api.h"
#include "ort_cancel.hpp"
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <stdexcept>

static_assert(ORT_API_VERSION == 17, "Review the TTS cancellation bridge on any ORT ABI change");
namespace {
thread_local voice_engine::TtsOrtCancellationScope* active_scope = nullptr;
const OrtApiBase* original_base() noexcept {
    static const auto get = reinterpret_cast<const OrtApiBase* (ORT_API_CALL*)() noexcept>(
        dlsym(RTLD_NEXT, "OrtGetApiBase"));
    if (!get) {
        std::fputs("TTS: cannot resolve the frozen ONNX Runtime API\n", stderr);
        std::abort();
    }
    return get();
}
const OrtApi& original_api() noexcept { return *original_base()->GetApi(ORT_API_VERSION); }
OrtStatus* ORT_API_CALL cancellable_run(OrtSession* session, const OrtRunOptions* options,
    const char* const* input_names, const OrtValue* const* inputs, size_t input_count,
    const char* const* output_names, size_t output_count, OrtValue** outputs) noexcept {
    const auto& api = original_api();
    auto* scope = active_scope;
    if (!scope || !scope->token)
        return api.Run(session, options, input_names, inputs, input_count,
                       output_names, output_count, outputs);
    ++scope->runs;
    if (scope->token->requested())
        return api.CreateStatus(ORT_FAIL, "TTS generation cancelled before ONNX Run");
    // Sherpa supplies a fresh RunOptions per Run. Preserve its options exactly.
    // Also support the C API's null/default option without sharing it globally.
    OrtRunOptions* owned = nullptr;
    if (!options) {
        if (auto* status = api.CreateRunOptions(&owned)) return status;
        options = owned;
    }
    auto* mutable_options = const_cast<OrtRunOptions*>(options);
    OrtStatus* result = nullptr;
    bool terminated = false;
    try {
        {
            voice_engine::TtsCancellation::Binding binding(*scope->token, [&] {
                auto* status = api.RunOptionsSetTerminate(mutable_options);
                if (status) api.ReleaseStatus(status);
                else terminated = true;
            });
            scope->token->native_run_started_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            ++scope->token->native_runs;
            result = api.Run(session, options, input_names, inputs, input_count,
                             output_names, output_count, outputs);
            scope->token->native_run_started_ns = 0;
        } // A racing cancel must finish before options are reset/released.
        if (terminated) {
            auto* status = api.RunOptionsUnsetTerminate(mutable_options);
            if (status) api.ReleaseStatus(status);
        }
    } catch (...) {
        scope->token->native_run_started_ns = 0;
        result = api.CreateStatus(ORT_FAIL, "TTS cancellation binding failed");
    }
    if (owned) api.ReleaseRunOptions(owned);
    return result;
}
const OrtApi* ORT_API_CALL bridge_api(uint32_t version) noexcept {
    const auto* original = original_base()->GetApi(version);
    if (!original || version != ORT_API_VERSION) return original;
    static const OrtApi wrapped = [] {
        OrtApi api = original_api();
        api.Run = cancellable_run;
        return api;
    }();
    return &wrapped;
}
const char* ORT_API_CALL bridge_version() noexcept { return original_base()->GetVersionString(); }
} // namespace

// Exported by the executable so the pinned, dynamically linked Sherpa uses the
// same table. Libraries and model assets themselves are not patched.
extern "C" const OrtApiBase* ORT_API_CALL OrtGetApiBase() noexcept {
    static const OrtApiBase base{bridge_api, bridge_version};
    return &base;
}
namespace voice_engine {
TtsOrtCancellationScope::TtsOrtCancellationScope(std::shared_ptr<TtsCancellation> value)
    : token(std::move(value)), previous_(active_scope) { active_scope = this; }
TtsOrtCancellationScope::~TtsOrtCancellationScope() { active_scope = previous_; }
void TtsOrtCancellationScope::verify_observed() const {
    if (token && !runs) throw std::runtime_error("TTS: native cancellation bridge was not reached");
}
} // namespace voice_engine
