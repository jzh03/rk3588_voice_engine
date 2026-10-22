#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace voice_engine {
struct Config;

struct LlmOptions {
    int context_tokens = 2048;
    int max_output_tokens = 128;
    int margin_tokens = 64;
    int first_token_timeout_ms = 10000;
    int total_timeout_ms = 30000;
    int cancel_timeout_ms = 2000;
    std::size_t queue_bytes = 8192;
};
struct LlmPerf {
    int prefill_tokens = 0, generate_tokens = 0;
    double prefill_ms = 0, generate_ms = 0, memory_mb = 0;
};
struct LlmBackendEvent {
    enum class Kind { token, finish, error };
    Kind kind = Kind::token;
    int token_id = -1;
    std::string text;
    LlmPerf perf;
};
// initialize/run/clear/shutdown execute on one persistent worker. abort/running
// may run concurrently on the supervisor, as required by RKLLM's abort API.
// run is synchronous; after run returns AND running()==0, callbacks must be
// finished. Tests may retain callback copies to exercise stale-turn filtering.
class LlmBackend {
public:
    using Callback = std::function<void(const LlmBackendEvent&)>;
    virtual ~LlmBackend() = default;
    virtual void initialize() = 0;
    virtual int run(const std::string& rendered_prompt, int max_tokens, Callback callback) = 0;
    virtual int abort() = 0;
    virtual int running() = 0;
    virtual int clear() = 0;
    virtual int shutdown() = 0;
};

namespace llm_detail { struct Turn; }
struct LlmTurn {
    std::uint64_t session_id = 0, generation_id = 0;
private:
    std::shared_ptr<llm_detail::Turn> state;
    friend class LlmAdapter;
};
struct LlmPacket {
    std::uint64_t session_id = 0, generation_id = 0, seq = 0;
    std::int64_t monotonic_ns = 0;
    int token_id = -1;
    // Complete UTF-8; incomplete bytes/control markers are retained internally.
    // Thinking and control markers never become speakable output.
    std::string text;
};
struct LlmResult {
    std::uint64_t session_id = 0, generation_id = 0;
    std::string status, text, raw_text;
    // Byte-based conservative admission estimate, NOT an exact tokenizer count.
    std::size_t prompt_estimated_tokens = 0, dropped_history_pairs = 0;
    int output_budget = 0, run_rc = -999, clear_rc = -999, abort_rc = -999;
    // token_callbacks counts token events with an ID >= 0, including waiting
    // tokens with empty text and stale IDs; FINISH/error events are separate.
    int token_callbacks = 0, finish_callbacks = 0, callback_errors = 0;
    std::size_t stale_callbacks = 0, queue_peak_bytes = 0;
    // Instantaneous retained callback queue, observed under the adapter mutex.
    std::size_t queue_current_bytes = 0, queue_current_packets = 0;
    bool overflow = false, invalid_utf8 = false, restart_required = false;
    bool history_committed = false, control_markers_seen = false;
    std::int64_t submit_ns = 0, first_token_ns = 0, first_text_ns = 0;
    std::int64_t finish_ns = 0, run_return_ns = 0, cancel_requested_ns = 0, idle_ns = 0;
    LlmPerf perf;
};

// Observational labels, not an SDK-reported internal stop reason. RKLLM's
// generate-stage perf counter is kept separately; its contract is not the total
// emitted token-ID count, and measured cap runs show different count offsets.
struct LlmTermination {
    std::string reason, basis;
    bool observed_budget_exceeded = false;
};
LlmTermination llm_observed_termination(const LlmResult& result,
                                        int observed_token_callbacks,
                                        int last_token_id);

class LlmAdapter {
public:
    LlmAdapter(std::unique_ptr<LlmBackend> backend, LlmOptions options = {});
    ~LlmAdapter();
    LlmAdapter(const LlmAdapter&) = delete;
    LlmAdapter& operator=(const LlmAdapter&) = delete;
    // A new start cancels an unfinished previous turn and waits for SDK idle.
    // Throws on invalid UTF-8/control input, budget rejection, or poisoned handle.
    LlmTurn start(const std::string& text, int max_tokens = 0,
                  int first_token_timeout_ms = 0, int total_timeout_ms = 0);
    // One consumer per turn. Token metadata can carry empty text.
    std::optional<LlmPacket> poll(const LlmTurn& turn, int wait_ms = 0);
    // Drains remaining packets. nullopt means caller wait expired; watchdog
    // deadlines continue independently. Completion includes SDK idle and KV clear.
    std::optional<LlmResult> wait(const LlmTurn& turn, int timeout_ms);
    LlmResult snapshot(const LlmTurn& turn) const;
    void cancel();
    void new_session();
    // Explicit application acknowledgement AFTER successful output/playback.
    // A cancelled/stale/not-complete turn can never enter history.
    bool commit_history(const LlmTurn& turn);
    // Atomic generation check + nonblocking downstream queue commit. The sink
    // must not block, perform I/O, call SDKs, or call back into this adapter.
    bool commit_if_current(const LlmTurn& turn, const std::function<void()>& sink);
    LlmResult warmup(const std::string& text);
    bool idle() const;
    bool restart_required() const;
    std::size_t history_pairs() const;
    double initialization_ms() const;
    // Explicit bounded release; false means active handle was NOT destroyed and
    // its worker retains backend/callback storage until idle, and the caller
    // must restart/terminate its supervised process. All turn submissions and
    // history commits are invalid after close, including completed turns.
    bool close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string llm_system_template();
std::string llm_render_prompt(const std::vector<std::pair<std::string,std::string>>& history,
                              const std::string& text);
bool llm_valid_utf8(const std::string& text);
LlmOptions llm_options_from_config(const Config& config);
struct RkllmAssets {
    std::string model_id;
    std::filesystem::path model, runtime;
};
// Requires the shared lock before hashing; validates only the selected LLM.
RkllmAssets resolve_rkllm_assets(const Config& config);
std::unique_ptr<LlmBackend> make_rkllm_backend(const RkllmAssets& assets,
                                             const LlmOptions& options);
} // namespace voice_engine
