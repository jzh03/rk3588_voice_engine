#include "voice_engine/llm.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace voice_engine {
namespace {
using Clock = std::chrono::steady_clock;
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}
int utf8_length(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}
bool valid_char(const std::string& s, std::size_t p, int n) {
    if (!n || p + n > s.size()) return false;
    for (int k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[p+k]) & 0xc0) != 0x80) return false;
    const auto a = static_cast<unsigned char>(s[p]);
    const auto b = n > 1 ? static_cast<unsigned char>(s[p+1]) : 0;
    return !(n == 3 && ((a == 0xe0 && b < 0xa0) || (a == 0xed && b >= 0xa0))) &&
           !(n == 4 && ((a == 0xf0 && b < 0x90) || (a == 0xf4 && b >= 0x90)));
}
bool reserved_input(const std::string& s) {
    return s.find("<|") != std::string::npos || s.find("<think") != std::string::npos ||
           s.find("</think") != std::string::npos || s.find('\0') != std::string::npos;
}
struct Filter {
    std::string pending;
    bool thinking = false, invalid = false, markers = false;
    std::string feed(const std::string& bytes, bool final = false) {
        pending += bytes;
        std::string out;
        std::size_t i = 0;
        while (i < pending.size()) {
            if (pending.compare(i, 7, "<think>") == 0) {
                thinking = true; markers = true; i += 7; continue;
            }
            if (pending.compare(i, 8, "</think>") == 0) {
                thinking = false; markers = true; i += 8; continue;
            }
            if (pending.compare(i, 2, "<|") == 0) {
                const auto end = pending.find("|>", i+2);
                if (end == std::string::npos) {
                    if (final) { markers = true; i = pending.size(); }
                    break;
                }
                markers = true; i = end + 2; continue;
            }
            if (pending[i] == '<') {
                const auto tail = pending.substr(i);
                bool prefix = false;
                for (const auto* tag : {"<think>", "</think>", "<|"})
                    if (std::string(tag).rfind(tail, 0) == 0) prefix = true;
                if (prefix) { if (final) { markers = true; i = pending.size(); } break; }
            }
            const int n = utf8_length(static_cast<unsigned char>(pending[i]));
            if (n && i + n > pending.size() && !final) break;
            if (!valid_char(pending, i, n) || pending[i] == '\0') {
                invalid = true; ++i; continue;
            }
            if (!thinking) out.append(pending, i, n);
            i += n;
        }
        pending.erase(0, i);
        if (final) {
            if (!pending.empty()) invalid = true;
            pending.clear();
        }
        // The queue is bounded; a malformed unfinished tag must also be bounded.
        if (pending.size() > 8192) { pending.clear(); invalid = true; }
        return out;
    }
};
} // namespace

bool llm_valid_utf8(const std::string& s) {
    for (std::size_t p = 0; p < s.size();) {
        const int n = utf8_length(static_cast<unsigned char>(s[p]));
        if (!valid_char(s, p, n)) return false;
        p += n;
    }
    return true;
}
LlmTermination llm_observed_termination(const LlmResult& r,int observed,int last_token_id) {
    if (observed < 0 || r.output_budget < 1)
        throw std::invalid_argument("invalid LLM token observation or output budget");
    LlmTermination value;
    value.observed_budget_exceeded = observed > r.output_budget;
    if (r.status != "completed") {
        value.reason = r.status;
        value.basis = "adapter_status";
    } else if (last_token_id == 248046 || last_token_id == 248044) {
        value.reason = "observed_eos";
        value.basis = "observed_selected_qwen_eos_token_id";
    } else if (value.observed_budget_exceeded) {
        value.reason = "observed_token_budget_exceeded";
        value.basis = "observed_valid_token_ids_exceed_requested_output_limit";
    } else if (observed == r.output_budget) {
        value.reason = "token_limit";
        value.basis = "observed_valid_token_ids_equal_requested_output_limit_inference_not_sdk_stop_reason";
    } else {
        value.reason = "sdk_finish_without_visible_eos";
        value.basis = "sdk_finish_without_observed_eos_or_requested_token_limit";
    }
    // Do not add one to perf.generate_tokens: the 1.3.0 header describes
    // generate-stage processing, and observed 1-token/4-token limits have
    // different offsets. Count generated IDs in the delivered trace instead.
    return value;
}
std::string llm_system_template() {
    return "<|im_start|>system\n你是本地中文问答助手，只能生成文字，没有联网、设备控制、文件操作或定时任务能力。用户要求你代为执行现实或软件操作时，先明确说明无法执行，不得声称已经完成或承诺稍后执行，也不要将请求复述成执行确认。翻译、改写、解释等纯文字任务应直接完成。缺少必要上下文或实时信息时，说明不知道或请用户补充，不猜测具体事实。用一句简短完整的话回答，不展开解释。<|im_end|>\n";
}
std::string llm_render_prompt(const std::vector<std::pair<std::string,std::string>>& history,
                              const std::string& text) {
    std::string p = llm_system_template();
    for (const auto& h : history)
        p += "<|im_start|>user\n" + h.first + "<|im_end|>\n<|im_start|>assistant\n" +
             h.second + "<|im_end|>\n";
    return p + "<|im_start|>user\n" + text +
           "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
}
namespace llm_detail {
struct Turn {
    LlmResult result;
    std::weak_ptr<void> owner;
    std::string input, prompt;
    int first_timeout_ms = 0, total_timeout_ms = 0;
    std::deque<LlmPacket> queue;
    std::size_t queue_bytes = 0;
    std::uint64_t next_seq = 1;
    bool valid = true, done = false, in_backend = false, abort_sent = false;
    bool consumed_final = false, warmup = false, supervisor_inflight = false;
    Filter filter;
};
} // namespace llm_detail
struct LlmAdapter::Impl {
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_ptr<LlmBackend> backend;
        LlmOptions options;
        std::shared_ptr<llm_detail::Turn> current;
        std::vector<std::pair<std::string,std::string>> history;
        std::uint64_t session = 1, generation = 0;
        bool ready = false, poisoned = false, stopping = false;
        bool worker_done = false, supervisor_done = false, job = false;
        std::string init_error;
        double init_ms = 0;
        int shutdown_rc = -999;
    };
    std::shared_ptr<Shared> s;
    std::thread worker, supervisor;
    static void invalidate(Shared& s, const std::shared_ptr<llm_detail::Turn>& t,
                           const std::string& reason) {
        if (!t || !t->valid) return;
        t->valid = false;
        if (s.current == t && t->result.session_id == s.session &&
            t->result.generation_id == s.generation) ++s.generation;
        t->queue.clear(); t->queue_bytes = 0;
        t->result.cancel_requested_ns = now_ns();
        t->result.status = reason;
        s.cv.notify_all();
    }
    static bool current(const Shared& s, const std::shared_ptr<llm_detail::Turn>& t) {
        return t && t->owner.lock().get() == &s && t->valid && s.current == t && t->result.session_id == s.session &&
               t->result.generation_id == s.generation && !s.poisoned && !s.stopping;
    }
    explicit Impl(std::unique_ptr<LlmBackend> b, LlmOptions o) : s(std::make_shared<Shared>()) {
        if (!b || o.context_tokens != 2048 || o.max_output_tokens < 1 ||
            o.max_output_tokens > 128 || o.margin_tokens < 64 ||
            o.max_output_tokens + o.margin_tokens >= o.context_tokens ||
            o.queue_bytes < 1 || o.queue_bytes > 1024*1024 ||
            o.first_token_timeout_ms < 1 || o.total_timeout_ms < o.first_token_timeout_ms ||
            o.cancel_timeout_ms < 1)
            throw std::invalid_argument("invalid fixed LLM options");
        s->backend = std::move(b); s->options = o;
        worker = std::thread([state=s] { work(state); });
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->cv.wait(lock, [&]{ return s->ready; });
            if (!s->init_error.empty()) {
                lock.unlock(); worker.join();
                throw std::runtime_error(s->init_error);
            }
        }
        supervisor = std::thread([state=s] { supervise(state); });
    }
    static void callback(const std::weak_ptr<Shared>& weak,
                         const std::shared_ptr<llm_detail::Turn>& t,
                         const LlmBackendEvent& e) {
        auto state = weak.lock();
        if (!state) return;
        std::lock_guard<std::mutex> lock(state->mutex);
        auto& r = t->result;
        const auto ts = now_ns();
        if (e.kind == LlmBackendEvent::Kind::finish) {
            ++r.finish_callbacks; r.finish_ns = ts; r.perf = e.perf;
            state->cv.notify_all(); return;
        }
        if (e.kind == LlmBackendEvent::Kind::error) {
            ++r.callback_errors;
            if (current(*state,t) && !t->done) invalidate(*state, t, "model_error");
            else ++r.stale_callbacks;
            return;
        }
        if (e.token_id >= 0) {
            ++r.token_callbacks;
            if (!r.first_token_ns) r.first_token_ns = ts;
        }
        if (!current(*state,t) || t->done) {
            ++r.stale_callbacks; return;
        }
        if (e.text.size() > state->options.queue_bytes - t->queue_bytes ||
            t->queue.size() >= 1024) {
            r.overflow = true;
            invalidate(*state, t, "queue_overflow");
            return;
        }
        t->queue.push_back({r.session_id,r.generation_id,t->next_seq++,ts,e.token_id,e.text});
        t->queue_bytes += e.text.size();
        r.queue_peak_bytes = std::max(r.queue_peak_bytes,t->queue_bytes);
        state->cv.notify_all();
    }
    static void work(const std::shared_ptr<Shared>& state) {
        const auto begin = Clock::now();
        try {
            state->backend->initialize();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->init_error = e.what(); state->ready = true; state->worker_done = true;
            state->cv.notify_all(); return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->init_ms = std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
            state->ready = true; state->cv.notify_all();
        }
        while (true) {
            std::shared_ptr<llm_detail::Turn> t;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait(lock,[&]{return state->job || state->stopping;});
                if (!state->job && state->stopping) break;
                t = state->current; state->job = false;
            }
            int clear_rc = state->backend->clear(), run_rc = -999;
            bool execute = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                t->result.clear_rc = clear_rc;
                execute = current(*state,t) && clear_rc == 0;
                t->in_backend = execute;
            }
            if (execute) {
                try {
                    const std::weak_ptr<Shared> weak = state;
                    run_rc = state->backend->run(t->prompt,t->result.output_budget,
                        [weak,t](const LlmBackendEvent& e){callback(weak,t,e);});
                } catch (...) { run_rc = -1; }
            }
            const auto returned = now_ns();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                t->result.run_rc = run_rc; t->result.run_return_ns = returned;
                state->cv.notify_all();
            }
            int running = state->backend->running();
            // A timed-out handle remains owned by this worker until SDK idle.
            // close() may detach this worker after its bounded wait, but its
            // captured Shared keeps backend and callback userdata alive. Never
            // turn a cancellation timeout into destruction of an active SDK.
            while (running != 0) {
                bool poisoned;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    poisoned = state->poisoned;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(poisoned ? 20 : 2));
                running = state->backend->running();
            }
            const auto idle_time = running == 0 ? now_ns() : 0;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                if (running == 0) t->in_backend = false;
                state->cv.wait(lock,[&]{return !t->supervisor_inflight;});
            }
            // Never destroy/reset or clear an active runtime.
            int post_clear = running == 0 ? state->backend->clear() : -999;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                t->in_backend = running != 0; t->result.run_rc = run_rc;
                t->result.run_return_ns = returned; t->result.idle_ns = idle_time;
                if (post_clear != 0 || clear_rc != 0 || running != 0) {
                    state->poisoned = true; t->result.restart_required = true;
                    if (t->valid) invalidate(*state,t,"model_error");
                }
                if (t->valid) {
                    if (run_rc == 0 && t->result.finish_callbacks == 1 &&
                        t->result.callback_errors == 0)
                        t->result.status = "completed";
                    else invalidate(*state,t,"model_error");
                }
                t->done = true; state->cv.notify_all();
            }
        }
        // Avoid racing shutdown with a supervisor that may still be in abort().
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock,[&]{return state->supervisor_done;});
        }
        while (state->backend->running() != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int rc = state->backend->shutdown();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->shutdown_rc = rc; state->worker_done = true; state->cv.notify_all();
        }
    }
    static void supervise(const std::shared_ptr<Shared>& state) {
        while (true) {
            std::shared_ptr<llm_detail::Turn> t;
            bool try_abort = false;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait_for(lock,std::chrono::milliseconds(2));
                if (state->stopping && (!state->current || state->current->done)) break;
                t = state->current;
                if (!t || t->done) continue;
                const auto now = now_ns();
                const auto elapsed = (now-t->result.submit_ns)/1000000;
                if (t->valid && !t->result.first_token_ns &&
                    elapsed >= t->first_timeout_ms)
                    invalidate(*state,t,"first_token_timeout");
                if (t->valid && elapsed >= t->total_timeout_ms)
                    invalidate(*state,t,"total_timeout");
                if (!t->valid && t->result.cancel_requested_ns &&
                    now-t->result.cancel_requested_ns >=
                        static_cast<std::int64_t>(state->options.cancel_timeout_ms)*1000000) {
                    state->poisoned = true; t->result.restart_required = true;
                    t->result.status = "cancel_timeout"; t->done = true;
                    state->cv.notify_all();
                    break;
                }
                try_abort = !t->valid && t->in_backend && !t->abort_sent;
                if (try_abort) t->supervisor_inflight = true;
            }
            // No generation/state lock is held during any SDK call.
            if (try_abort) {
                int rc = -999;
                const bool active = state->backend->running() == 1;
                if (active) rc = state->backend->abort();
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (active) { t->abort_sent = true; t->result.abort_rc = rc; }
                    t->supervisor_inflight = false; state->cv.notify_all();
                }
            }
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->supervisor_done = true; state->cv.notify_all();
    }
    static std::optional<LlmPacket> drain(Shared& state, const std::shared_ptr<llm_detail::Turn>& t) {
        if (!t->queue.empty()) {
            LlmPacket p = std::move(t->queue.front()); t->queue.pop_front();
            t->queue_bytes -= p.text.size();
            t->result.raw_text += p.text;
            if (t->result.raw_text.size() > 65536) {
                invalidate(state,t,"output_byte_limit"); return std::nullopt;
            }
            p.text = t->filter.feed(p.text);
            t->result.text += p.text;
            if (!p.text.empty() && !t->result.first_text_ns)
                t->result.first_text_ns = now_ns();
            t->result.invalid_utf8 = t->filter.invalid;
            t->result.control_markers_seen = t->filter.markers;
            return p;
        }
        if (t->done && !t->consumed_final) {
            t->consumed_final = true;
            const auto last = t->filter.feed("",true);
            t->result.text += last;
            t->result.invalid_utf8 = t->filter.invalid;
            t->result.control_markers_seen = t->filter.markers;
            if (!last.empty() && current(state,t))
                return LlmPacket{t->result.session_id,t->result.generation_id,
                                 t->next_seq++,now_ns(),-1,last};
        }
        return std::nullopt;
    }
    bool close() {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            invalidate(*s,s->current,"closed");
            s->history.clear(); s->stopping = true; s->cv.notify_all();
        }
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->cv.wait_for(lock,std::chrono::milliseconds(s->options.cancel_timeout_ms+100),
                [&]{return s->worker_done && s->supervisor_done;});
        }
        bool wd,sd;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            wd = s->worker_done; sd = s->supervisor_done;
        }
        if (worker.joinable()) { if(wd) worker.join(); else worker.detach(); }
        if (supervisor.joinable()) { if(sd) supervisor.join(); else supervisor.detach(); }
        return wd && sd && s->shutdown_rc == 0;
    }
};

LlmAdapter::LlmAdapter(std::unique_ptr<LlmBackend> b,LlmOptions o)
    : impl_(std::make_unique<Impl>(std::move(b),o)) {}
LlmAdapter::~LlmAdapter() { impl_->close(); }
LlmTurn LlmAdapter::start(const std::string& text,int max_tokens,int first_timeout,int total_timeout) {
    if (text.empty() || !llm_valid_utf8(text) || reserved_input(text))
        throw std::invalid_argument("LLM input must be nonempty UTF-8 without template control markers");
    auto s = impl_->s;
    std::unique_lock<std::mutex> lock(s->mutex);
    if (s->poisoned || s->stopping) throw std::runtime_error("LLM handle requires process restart");
    if (s->current && !s->current->done) {
        Impl::invalidate(*s,s->current,"superseded");
        s->cv.wait_for(lock,std::chrono::milliseconds(s->options.cancel_timeout_ms+100),
                      [&]{return s->current->done;});
        if (!s->current->done || s->poisoned)
            throw std::runtime_error("LLM previous turn did not become idle");
    }
    if (max_tokens == 0) max_tokens = s->options.max_output_tokens;
    if (max_tokens < 1 || max_tokens > s->options.max_output_tokens)
        throw std::invalid_argument("LLM output budget exceeds configured maximum");
    if (first_timeout == 0) first_timeout = s->options.first_token_timeout_ms;
    if (total_timeout == 0) total_timeout = s->options.total_timeout_ms;
    if (first_timeout < 1 || total_timeout < first_timeout ||
        first_timeout > s->options.first_token_timeout_ms || total_timeout > s->options.total_timeout_ms)
        throw std::invalid_argument("invalid LLM request deadline");
    auto history = s->history;
    std::size_t dropped = 0;
    std::string prompt = llm_render_prompt(history,text);
    while (prompt.size()+max_tokens+s->options.margin_tokens >
           static_cast<std::size_t>(s->options.context_tokens) && !history.empty()) {
        history.erase(history.begin()); ++dropped; prompt = llm_render_prompt(history,text);
    }
    if (prompt.size()+max_tokens+s->options.margin_tokens >
        static_cast<std::size_t>(s->options.context_tokens))
        throw std::length_error("LLM input exceeds conservative UTF-8 byte budget; shorten input");
    s->history = std::move(history);
    auto t = std::make_shared<llm_detail::Turn>();
    t->owner = s;
    t->input = text; t->prompt = std::move(prompt);
    t->first_timeout_ms = first_timeout; t->total_timeout_ms = total_timeout;
    t->result.session_id = s->session; t->result.generation_id = ++s->generation;
    t->result.status = "running"; t->result.submit_ns = now_ns();
    t->result.prompt_estimated_tokens = t->prompt.size();
    t->result.dropped_history_pairs = dropped; t->result.output_budget = max_tokens;
    s->current = t; s->job = true; s->cv.notify_all();
    LlmTurn turn; turn.session_id = t->result.session_id;
    turn.generation_id = t->result.generation_id; turn.state = t;
    return turn;
}
std::optional<LlmPacket> LlmAdapter::poll(const LlmTurn& turn,int wait_ms) {
    if (!turn.state || turn.state->owner.lock().get() != impl_->s.get())
        throw std::invalid_argument("empty or foreign LLM turn");
    auto s = impl_->s; std::unique_lock<std::mutex> lock(s->mutex);
    s->cv.wait_for(lock,std::chrono::milliseconds(std::max(0,wait_ms)),
        [&]{return !turn.state->queue.empty() || turn.state->done;});
    if (!Impl::current(*s,turn.state)) return std::nullopt;
    return Impl::drain(*s,turn.state);
}
std::optional<LlmResult> LlmAdapter::wait(const LlmTurn& turn,int timeout_ms) {
    if (!turn.state || turn.state->owner.lock().get() != impl_->s.get())
        throw std::invalid_argument("empty or foreign LLM turn");
    auto s = impl_->s;
    const auto end = Clock::now()+std::chrono::milliseconds(std::max(0,timeout_ms));
    std::unique_lock<std::mutex> lock(s->mutex);
    while (true) {
        while (!turn.state->queue.empty()) Impl::drain(*s,turn.state);
        if (turn.state->done) { Impl::drain(*s,turn.state); return turn.state->result; }
        if (s->cv.wait_until(lock,end) == std::cv_status::timeout) return std::nullopt;
    }
}
LlmResult LlmAdapter::snapshot(const LlmTurn& turn) const {
    if (!turn.state || turn.state->owner.lock().get() != impl_->s.get())
        throw std::invalid_argument("empty or foreign LLM turn");
    std::lock_guard<std::mutex> lock(impl_->s->mutex);
    auto result = turn.state->result;
    result.queue_current_bytes = turn.state->queue_bytes;
    result.queue_current_packets = turn.state->queue.size();
    return result;
}
void LlmAdapter::cancel() {
    auto s = impl_->s; std::lock_guard<std::mutex> lock(s->mutex);
    Impl::invalidate(*s,s->current,"cancelled");
}
void LlmAdapter::new_session() {
    auto s = impl_->s; std::lock_guard<std::mutex> lock(s->mutex);
    Impl::invalidate(*s,s->current,"new_session"); ++s->session; s->history.clear();
}
bool LlmAdapter::commit_history(const LlmTurn& turn) {
    auto s = impl_->s; std::lock_guard<std::mutex> lock(s->mutex);
    auto t = turn.state;
    if (!Impl::current(*s,t) || !t->done || !t->consumed_final || t->warmup ||
        t->result.status != "completed" || t->result.history_committed ||
        t->result.invalid_utf8 || t->result.text.empty()) return false;
    s->history.emplace_back(t->input,t->result.text);
    // Even empty/small responses cannot make history unbounded.
    if (s->history.size() > 32) s->history.erase(s->history.begin());
    t->result.history_committed = true; return true;
}
bool LlmAdapter::commit_if_current(const LlmTurn& turn,const std::function<void()>& sink) {
    auto s = impl_->s; std::lock_guard<std::mutex> lock(s->mutex);
    if (!Impl::current(*s,turn.state)) return false;
    sink(); return true;
}
LlmResult LlmAdapter::warmup(const std::string& text) {
    auto turn = start(text,std::min(32,impl_->s->options.max_output_tokens));
    {
        std::lock_guard<std::mutex> lock(impl_->s->mutex); turn.state->warmup = true;
    }
    const auto r = wait(turn,impl_->s->options.total_timeout_ms+impl_->s->options.cancel_timeout_ms+500);
    if (!r) throw std::runtime_error("LLM warmup did not terminate");
    return *r;
}
bool LlmAdapter::idle() const {
    std::lock_guard<std::mutex> lock(impl_->s->mutex);
    return !impl_->s->current || (impl_->s->current->done && !impl_->s->current->in_backend);
}
bool LlmAdapter::restart_required() const {
    std::lock_guard<std::mutex> lock(impl_->s->mutex); return impl_->s->poisoned;
}
std::size_t LlmAdapter::history_pairs() const {
    std::lock_guard<std::mutex> lock(impl_->s->mutex); return impl_->s->history.size();
}
double LlmAdapter::initialization_ms() const { return impl_->s->init_ms; }
bool LlmAdapter::close() { return impl_->close(); }
} // namespace voice_engine
