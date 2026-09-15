#include "audio_driver.h"
#include "audio_pcm_tap.h"
#include "audio_resampler_trace.h"
#include "audio_output_headroom.h"
#include "runtime.h"
#include "renderer/engine/engine_performance.h"
#include "ppc_recomp_shared.h"
#include <xaudio2.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t kWorkerStackSize = 0x10000u;
constexpr uint32_t kMaxQueued = kAudioMaxQueuedBuffers;
constexpr int kInitialCredits = int(kAudioPrefillBuffers);
// Retry an empty guest callback at most once per PCM quantum (rounded up).
constexpr DWORD kNoSubmitRetryMs = (1000 * kAudioFramesPerSubmit + kAudioRateHz - 1) / kAudioRateHz;
constexpr uint32_t kTokenBase = 0x44400000u;
constexpr uint32_t kTokenGenMask = 0xFFFFFu;
constexpr uint32_t kBusyHresult = 0x800700AAu;
constexpr uint32_t kBadHandle = 0xC000000Du;
constexpr uint32_t kNoMemory = 0xC0000017u;

struct AudioComScope {
    HRESULT hr = S_OK;
    bool mine = false;
    AudioComScope() {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mine = SUCCEEDED(hr);
    }
    ~AudioComScope() {
        if (mine) CoUninitialize();
    }
};

// The worker owns its MMCSS enrollment and teardown. Dynamic loading avoids a
// new shared build dependency and permits a thread-priority fallback.
struct AudioSchedulingScope {
    HMODULE module = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Register = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using Revert = BOOL(WINAPI*)(HANDLE);
    Revert revert = nullptr;
    HANDLE task = nullptr;
    AudioSchedulingScope() {
        if (module) {
            auto enroll = reinterpret_cast<Register>(GetProcAddress(module, "AvSetMmThreadCharacteristicsW"));
            revert = reinterpret_cast<Revert>(GetProcAddress(module, "AvRevertMmThreadCharacteristics"));
            DWORD taskIndex = 0;
            if (enroll && revert) task = enroll(L"Audio", &taskIndex);
        }
        if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
    ~AudioSchedulingScope() {
        if (task) revert(task);
        if (module) FreeLibrary(module);
    }
};

bool audioGuestSpan(Memory* owner, uint32_t address, uint32_t bytes, bool writable = false) {
    if (!owner || !bytes || uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    uint8_t* base = owner->base();
    if (!base) return false;
    uint64_t cursor = address, end = cursor + bytes;
    // A PCM frame may span regions with different, still readable protections.
    // Validate the complete span before conversion or any guest output write.
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!engineProfileVirtualQuery(EnginePhase::queryAudio, base + cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD protection = info.Protect & 0xFFu;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (writable ? !canWrite : !(canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ))
            return false;
        const uint64_t next = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - base;
        if (next <= cursor) return false;
        cursor = next < end ? next : end;
    }
    return true;
}
bool audioGuestWritable(Memory* owner, uint32_t address, uint32_t bytes) {
    return audioGuestSpan(owner, address, bytes, true);
}
}  // namespace

void audioConvertPlanarBEFloat(const uint8_t* planarBE, float* interleavedOut) {
    for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; ++frame) {
        for (uint32_t ch = 0; ch < kAudioChannels; ++ch) {
            const uint8_t* src = planarBE + (size_t(ch) * kAudioFramesPerSubmit + frame) * 4;
            uint32_t bits = (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) |
                            (uint32_t(src[2]) << 8) | uint32_t(src[3]);
            float value;
            memcpy(&value, &bits, 4);
            interleavedOut[size_t(frame) * kAudioChannels + ch] = value;
        }
    }
}

struct AudioRenderDriver::Impl {
    struct Client;
    struct ClientVoiceCallback : public IXAudio2VoiceCallback {
        Client* client = nullptr;
        void STDMETHODCALLTYPE OnBufferEnd(void* /*context*/) noexcept override;
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) noexcept override;
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
        void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override;
    };
    struct Client {
        uint32_t token = 0;
        Memory* owner = nullptr;
        uint32_t guestCallback = 0;
        uint32_t argCell = 0;
        uint32_t workerAlloc = 0;
        DWORD workerThreadId = 0;
        HANDLE thread = nullptr;
        HANDLE stopEvent = nullptr;
        HANDLE creditEvent = nullptr;
        HANDLE readyEvent = nullptr;
        HANDLE startEvent = nullptr;
        PTP_TIMER startupTimer = nullptr;
        HRESULT readyStatus = E_FAIL;
        DispatcherCancellation cancellation;
        IXAudio2SourceVoice* voice = nullptr;
        ClientVoiceCallback voiceCb;
        PPCContext workerCtx{};
        std::atomic<bool> active{false};
        std::atomic<HRESULT> voiceError{S_OK};
        std::atomic<uint32_t> volumeChangeMask{0};
        // PCM ownership uses the driver mutex; device callbacks never touch it.
        std::deque<std::vector<float>> queued;
        std::atomic<uint64_t> completedSeq{0};
        uint64_t reclaimed = 0;
        uint64_t submittedSeq = 0;
        uint64_t workerSubmissions = 0; // Only the owning callback thread writes/reads this.
        int credits = kInitialCredits;
        uint64_t creditedCompletions = 0; // Worker-owned, like credits.
        uint32_t startupCallbacks = 0;
        std::atomic<bool> startupCallbacksSpent{false};
        std::atomic<bool> playbackStarted{false};
        std::atomic<bool> workerMmcss{false};
        std::vector<float> lastSubmitted;
        AudioPcmTap pcmTap;
    };

    std::mutex mutex;
    std::condition_variable closingDone;
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    bool muted = false;
    CO_MTA_USAGE_COOKIE mtaCookie = nullptr;
    std::shared_ptr<Client> active;
    std::shared_ptr<Client> closing;
    uint32_t nextGen = 1;

    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> framesSubmitted{0};
    std::atomic<uint64_t> samplesQueued{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> deviceErrors{0};
    std::atomic<uint64_t> starvationPasses{0};
    std::atomic<uint64_t> starvationBytes{0};
    std::atomic<uint64_t> maxCallbackMicros{0};
    std::atomic<uint64_t> callbacksWithoutSubmit{0};
    std::atomic<int32_t> lastHresult{0};
};

namespace {
AudioRenderDriver::Impl gDriver;
std::atomic<HRESULT> nextStartupError{S_OK};

void audioNoteError(HRESULT hr) {
    gDriver.deviceErrors++;
    gDriver.lastHresult = int32_t(hr);
}

void audioReclaimCompleted(AudioRenderDriver::Impl::Client* client) {
    // Caller holds gDriver.mutex. The device publishes completion after it is
    // finished with the PCM; no callback accesses this container.
    uint64_t done = client->completedSeq.load(std::memory_order_acquire);
    while (!client->queued.empty() && client->reclaimed < done) {
        client->queued.pop_front();
        client->reclaimed++;
    }
}

// Caller holds gDriver.mutex. Sparse/deferred clients must still drain even if
// they submit fewer frames than their initial callback budget.
HRESULT audioStartIfReady(AudioRenderDriver::Impl::Client* client, bool deadlineExpired = false) {
    if (client->playbackStarted.load(std::memory_order_acquire) || client->queued.empty()) return S_OK;
    if (!deadlineExpired && client->queued.size() < kAudioPrefillBuffers &&
        !client->startupCallbacksSpent.load(std::memory_order_acquire)) return S_OK;
    client->playbackStarted.store(true, std::memory_order_release);
    const HRESULT hr = client->voice->Start(0);
    if (FAILED(hr)) {
        client->playbackStarted.store(false, std::memory_order_release);
        client->voiceError.store(hr, std::memory_order_release);
        audioNoteError(hr);
    }
    return hr;
}

// Independent of the guest producer: a callback may wait for work that needs
// the first submitted PCM to play. Never leave that sparse prefill stopped.
void CALLBACK audioStartupDeadline(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
    auto* client = static_cast<AudioRenderDriver::Impl::Client*>(context);
    std::lock_guard lock(gDriver.mutex);
    if (gDriver.active.get() != client || !client->active.load(std::memory_order_acquire) ||
        client->voiceError.load(std::memory_order_acquire) != S_OK ||
        client->playbackStarted.load(std::memory_order_acquire)) return;
    const HRESULT hr = audioStartIfReady(client, true);
    fprintf(stderr, "[Audio] startup prefill deadline=%ums queued=%zu started=%u hr=0x%08X\n",
            kAudioPrefillDeadlineMs, client->queued.size(), unsigned(client->playbackStarted.load()), unsigned(hr));
    if (FAILED(hr)) SetEvent(client->creditEvent);
}

// Producer-side diagnostics only, never called on the XAudio2 processing thread.
void audioLogContinuity(AudioRenderDriver::Impl::Client* client) {
    XAUDIO2_VOICE_STATE state{};
    client->voice->GetState(&state);
    XAUDIO2_PERFORMANCE_DATA perf{};
    gDriver.engine->GetPerformanceData(&perf);
    const uint64_t submittedFrames = client->submittedSeq * kAudioFramesPerSubmit;
    const uint64_t queuedFrames = submittedFrames > state.SamplesPlayed ? submittedFrames - state.SamplesPlayed : 0;
    fprintf(stderr, "[AudioContinuity] submitted=%llu completed=%llu queuedBuffers=%u queuedMs=%.3f "
                    "starvationPasses=%llu starvationBytes=%llu engineGlitches=%u maxCallbackUs=%llu noSubmit=%llu "
                    "started=%u mmcss=%u prefillBuffers=%u\n",
            client->submittedSeq, client->completedSeq.load(), state.BuffersQueued,
            double(queuedFrames) * 1000.0 / kAudioRateHz,
            gDriver.starvationPasses.load(), gDriver.starvationBytes.load(), perf.GlitchesSinceEngineStarted,
            gDriver.maxCallbackMicros.load(), gDriver.callbacksWithoutSubmit.load(), unsigned(client->playbackStarted.load()),
            unsigned(client->workerMmcss.load()), kAudioPrefillBuffers);
}

void audioLogOutputLevels(const char* label, uint32_t index, const float* levels, uint32_t count) {
    char values[2048]{};
    size_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int written = snprintf(values + used, sizeof(values) - used, "%s%.9g", i ? "," : "", levels[i]);
        if (written < 0 || size_t(written) >= sizeof(values) - used) break;
        used += size_t(written);
    }
    fprintf(stderr, "[AudioOutput] %s[%u]=%s\n", label, index, values);
}

// Tap-only startup readback, before the guest producer runs. No graph changes.
void audioLogOutputConfiguration(AudioRenderDriver::Impl::Client* client) {
    XAUDIO2_VOICE_DETAILS source{}, master{};
    client->voice->GetVoiceDetails(&source);
    gDriver.master->GetVoiceDetails(&master);
    DWORD mask = 0;
    const HRESULT maskHr = gDriver.master->GetChannelMask(&mask);
    float sourceGain = 0, masterGain = 0, frequencyRatio = 0;
    client->voice->GetVolume(&sourceGain);
    gDriver.master->GetVolume(&masterGain);
    client->voice->GetFrequencyRatio(&frequencyRatio);
    fprintf(stderr, "[AudioOutput] sourceChannels=%u sourceRate=%u sourceMask=0x%08X "
                    "masterChannels=%u masterRate=%u masterMask=0x%08X maskHr=0x%08X "
                    "sourceGain=%.9g masterGain=%.9g frequencyRatio=%.9g\n",
            source.InputChannels, source.InputSampleRate, 0x3Fu,
            master.InputChannels, master.InputSampleRate, unsigned(mask), unsigned(maskHr),
            sourceGain, masterGain, frequencyRatio);
    constexpr uint32_t kMaxDiagnosticChannels = 64;
    if (source.InputChannels != kAudioChannels || !master.InputChannels ||
        master.InputChannels > kMaxDiagnosticChannels) {
        fprintf(stderr, "[AudioOutput] matrix readback skipped: unsupported diagnostic channel count\n");
        return;
    }
    float sourceLevels[kAudioChannels]{};
    float matrix[kAudioChannels * kMaxDiagnosticChannels]{};
    client->voice->GetChannelVolumes(kAudioChannels, sourceLevels);
    client->voice->GetOutputMatrix(gDriver.master, kAudioChannels, master.InputChannels, matrix);
    audioLogOutputLevels("sourceChannelGains", 0, sourceLevels, kAudioChannels);
    // Mastering voices do not expose per-channel volume. Keep matrix readback
    // in its exact returned order; offsets below count raw coefficients, so
    // diagnostics do not rely on conflicting Get/Set layout documentation.
    const uint32_t count=kAudioChannels*master.InputChannels;
    for(uint32_t offset=0;offset<count;offset+=kMaxDiagnosticChannels)
        audioLogOutputLevels("matrixRaw",offset,matrix+offset,(std::min)(kMaxDiagnosticChannels,count-offset));
}

void audioInvokeGuest(AudioRenderDriver::Impl::Client* client) {
    __try {
        client->workerCtx.r3.u64 = client->argCell;
        PPCSafeIndirect(client->workerCtx, client->owner->base(), client->guestCallback);
        gDriver.callbacks++;
    } __except (GetExceptionCode() == 0xe06d7363 ? EXCEPTION_CONTINUE_SEARCH
                                               : exceptionFilter(GetExceptionInformation())) {
        fflush(stderr);
        ExitProcess(3);
    }
}

DWORD WINAPI audioDriverWorkerMain(void* raw) {
    auto* held = static_cast<std::shared_ptr<AudioRenderDriver::Impl::Client>*>(raw);
    std::shared_ptr<AudioRenderDriver::Impl::Client> client = *held;
    delete held;
    AudioComScope com;
    HRESULT injected = nextStartupError.exchange(S_OK);
    client->readyStatus = FAILED(com.hr) ? com.hr : injected;
    SetEvent(client->readyEvent);
    if (FAILED(client->readyStatus)) return 1;
    HANDLE startupHandles[] = {client->stopEvent, client->startEvent};
    if (WaitForMultipleObjects(2, startupHandles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
        return 0;
    AudioSchedulingScope scheduling;
    client->workerMmcss.store(scheduling.task != nullptr, std::memory_order_release);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    currentContext = &client->workerCtx;
    client->workerCtx.fpscr.loadFromHost();
    setDispatcherCancellation(&client->cancellation);
    for (;;) {
        if (WaitForSingleObject(client->stopEvent, 0) == WAIT_OBJECT_0) break;
        if (client->voiceError.load(std::memory_order_acquire) != S_OK) {
            HANDLE handles[2] = {client->stopEvent, client->creditEvent};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            continue;
        }
        // Auto-reset events coalesce signals. Harvest the complete sequence so
        // a batch of completions retains every credit without device-side locks.
        const uint64_t completed = client->completedSeq.load(std::memory_order_acquire);
        const uint64_t replenished = uint64_t(client->credits) + completed - client->creditedCompletions;
        client->credits = int(replenished < kMaxQueued ? replenished : kMaxQueued);
        client->creditedCompletions = completed;
        const bool haveCredit = client->credits > 0;
        if (haveCredit) client->credits--;
        if (!haveCredit) {
            client->startupCallbacksSpent.store(true, std::memory_order_release);
            {
                std::lock_guard lock(gDriver.mutex);
                if (client->active.load(std::memory_order_acquire)) audioStartIfReady(client.get());
            }
            HANDLE handles[2] = {client->stopEvent, client->creditEvent};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            continue;
        }
        if (!client->active.load(std::memory_order_acquire)) break;
        LARGE_INTEGER begin{}, end{};
        const uint64_t submittedBefore = client->workerSubmissions;
        QueryPerformanceCounter(&begin);
        try { audioInvokeGuest(client.get()); }
        catch (const DispatcherWaitCancelled&) { break; }
        QueryPerformanceCounter(&end);
        const uint64_t micros = uint64_t(end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart;
        if (micros > gDriver.maxCallbackMicros.load()) gDriver.maxCallbackMicros.store(micros);
        if (client->startupCallbacks < kAudioPrefillBuffers &&
            ++client->startupCallbacks == kAudioPrefillBuffers) {
            client->startupCallbacksSpent.store(true, std::memory_order_release);
            std::lock_guard lock(gDriver.mutex);
            if (client->active.load(std::memory_order_acquire) &&
                client->voiceError.load(std::memory_order_acquire) == S_OK) audioStartIfReady(client.get());
        }
        if (client->workerSubmissions == submittedBefore) {
            // The guest may return early after its wait/stop event without PCM.
            // Consuming that credit forever progressively shrinks the queue and
            // eventually leaves no completion capable of waking the producer.
            ++client->credits;
            gDriver.callbacksWithoutSubmit.fetch_add(1, std::memory_order_relaxed);
            // Ignore completion signals for this delay: they must not turn a
            // guest that keeps returning empty into a busy loop. Stop still wakes.
            WaitForSingleObject(client->stopEvent, kNoSubmitRetryMs);
        }
    }
    setDispatcherCancellation(nullptr);
    currentContext = nullptr;
    return 0;
}

void audioTeardownClient(AudioRenderDriver::Impl::Client* client) {
    if (!client) return;
    if (client->voice) {
        client->voice->Stop(0);
        client->voice->FlushSourceBuffers();
        client->voice->DestroyVoice();
        client->voice = nullptr;
    }
    client->queued.clear();
    client->lastSubmitted.clear();
    if (client->creditEvent) {
        CloseHandle(client->creditEvent);
        client->creditEvent = nullptr;
    }
    if (client->stopEvent) {
        CloseHandle(client->stopEvent);
        client->stopEvent = nullptr;
    }
    if (client->readyEvent) {
        CloseHandle(client->readyEvent);
        client->readyEvent = nullptr;
    }
    if (client->startEvent) {
        CloseHandle(client->startEvent);
        client->startEvent = nullptr;
    }
    if (client->owner) {
        if (client->argCell) client->owner->release(client->argCell);
        if (client->workerAlloc) client->owner->release(client->workerAlloc);
    }
    client->argCell = 0;
    client->workerAlloc = 0;
}

void audioJoinAndTeardown(const std::shared_ptr<AudioRenderDriver::Impl::Client>& client) {
    if (!client) return;
    if (client->stopEvent) SetEvent(client->stopEvent);
    cancelDispatcherWaits(client->cancellation);
    // active is already false (or startup was never published). No submit can
    // rearm the timer. Join it before releasing the client/source voice.
    if (client->startupTimer) {
        SetThreadpoolTimer(client->startupTimer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(client->startupTimer, TRUE);
        CloseThreadpoolTimer(client->startupTimer);
        client->startupTimer = nullptr;
    }
    if (client->thread) {
        WaitForSingleObject(client->thread, INFINITE);
        CloseHandle(client->thread);
        client->thread = nullptr;
    }
    // The producer is joined, so a partial capture can now be finalized safely.
    client->pcmTap.finish();
    audioResamplerTrace.finish();
    audioTeardownClient(client.get());
}

// Called with the driver mutex held, only after all source voices are gone.
void audioReleaseEngine() {
    if (gDriver.master) { gDriver.master->DestroyVoice(); gDriver.master = nullptr; }
    if (gDriver.engine) { gDriver.engine->Release(); gDriver.engine = nullptr; }
    if (gDriver.mtaCookie) {
        HRESULT hr = CoDecrementMTAUsage(gDriver.mtaCookie);
        gDriver.mtaCookie = nullptr;
        if (FAILED(hr)) audioNoteError(hr);
    }
}

void audioFatalSelfTeardown(const char* where) {
    fprintf(stderr, "[Audio] FATAL: %s on owning worker thread; cannot free executing stack\n",
            where);
    fflush(stderr);
    ExitProcess(3);
}
}  // namespace

void AudioRenderDriver::Impl::ClientVoiceCallback::OnBufferEnd(void* /*context*/) noexcept {
    if (!client) return;
    client->completedSeq.fetch_add(1, std::memory_order_release);
    gDriver.completed++;
    if (client->creditEvent) SetEvent(client->creditEvent);
}
void AudioRenderDriver::Impl::ClientVoiceCallback::OnVoiceProcessingPassStart(UINT32 bytesRequired) noexcept {
    // For PCM, this is data missing from the upcoming processing pass. It also
    // counts intentional drains by idle/deferred clients, not only game glitches.
    if (!client || !client->active.load(std::memory_order_acquire) ||
        !client->playbackStarted.load(std::memory_order_acquire) || !bytesRequired) return;
    gDriver.starvationPasses.fetch_add(1, std::memory_order_relaxed);
    gDriver.starvationBytes.fetch_add(bytesRequired, std::memory_order_relaxed);
}
void AudioRenderDriver::Impl::ClientVoiceCallback::OnVoiceError(void* /*context*/, HRESULT error) noexcept {
    if (!client) return;
    HRESULT expected = S_OK;
    client->voiceError.compare_exchange_strong(expected, error);
    gDriver.deviceErrors++;
    gDriver.lastHresult = int32_t(error);
    if (client->creditEvent) SetEvent(client->creditEvent);
}

AudioRenderDriver::Impl& AudioRenderDriver::impl() { return gDriver; }
AudioRenderDriver& AudioRenderDriver::instance() {
    static AudioRenderDriver driver;
    return driver;
}

bool audioDriverPeekLastSubmitted(std::vector<float>& out) {
    std::lock_guard lock(gDriver.mutex);
    if (!gDriver.active || gDriver.active->lastSubmitted.empty()) return false;
    out = gDriver.active->lastSubmitted;
    return true;
}

uint32_t audioDriverInjectVoiceError(int32_t hr) {
    std::lock_guard lock(gDriver.mutex);
    if (!gDriver.active) return kBadHandle;
    gDriver.active->voiceCb.OnVoiceError(nullptr, HRESULT(hr));
    return 0;
}

void audioDriverInjectStartupError(int32_t hr) {
    nextStartupError.store(HRESULT(hr));
}

uint32_t audioDriverVoiceCategoryVolumeChangeMask(uint32_t token, uint32_t outMask) {
    std::lock_guard lock(gDriver.mutex);
    AudioRenderDriver::Impl::Client* client = gDriver.active.get();
    if (!client || !client->active.load(std::memory_order_acquire) || token != client->token)
        return kBadHandle;
    Memory* owner = client->owner;
    if (!audioGuestWritable(owner, outMask, 4)) return kBadHandle;
    owner->write32(outMask, client->volumeChangeMask.load(std::memory_order_acquire));
    return 0;
}

bool AudioRenderDriver::setMuted(bool muted) {
    std::lock_guard lock(gDriver.mutex);
    if (gDriver.master) {
        const HRESULT hr = gDriver.master->SetVolume(muted ? 0.0f : 1.0f);
        if (FAILED(hr)) { audioNoteError(hr); return false; }
    }
    gDriver.muted = muted;
    return true;
}

uint32_t AudioRenderDriver::registerClient(uint32_t guestCallback, uint32_t rawArgument,
                                           uint32_t driverOut) {
    AudioComScope com;
    if (FAILED(com.hr)) { audioNoteError(com.hr); return uint32_t(com.hr); }
    Memory* owner = memory;
    if (!guestCallback || !owner || !audioGuestWritable(owner, driverOut, 4)) return kBadHandle;
    std::unique_lock lock(gDriver.mutex);
    if (gDriver.active || gDriver.closing) return kBusyHresult;
    if (gDriver.nextGen > kTokenGenMask) return kNoMemory;
    std::shared_ptr<Impl::Client> client;
    auto fail = [&](HRESULT hr) -> uint32_t {
        audioNoteError(hr);
        // The worker cannot enter guest code until startEvent is signaled.
        audioJoinAndTeardown(client);
        audioReleaseEngine();
        return uint32_t(hr);
    };
    try {
        client = std::make_shared<Impl::Client>();
        client->owner = owner;
        client->guestCallback = guestCallback;
        if (!gDriver.engine) {
            HRESULT hr = CoIncrementMTAUsage(&gDriver.mtaCookie);
            if (FAILED(hr)) return fail(hr);
            hr = XAudio2Create(&gDriver.engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
            if (FAILED(hr)) return fail(hr);
            hr = gDriver.engine->CreateMasteringVoice(&gDriver.master);
            if (FAILED(hr)) return fail(hr);
            hr = gDriver.master->SetVolume(gDriver.muted ? 0.0f : 1.0f);
            if (FAILED(hr)) return fail(hr);
            float outputVolume = -1.0f;
            gDriver.master->GetVolume(&outputVolume);
            std::fprintf(stderr, "[Audio] Mastering voice volume=%g (%s)\n", outputVolume,
                         gDriver.muted ? "muted" : "audible");
        }
        client->argCell = owner->allocate(4);
        if (!client->argCell) return fail(E_OUTOFMEMORY);
        owner->write32(client->argCell, rawArgument);
        client->workerAlloc = owner->allocate(kWorkerStackSize + 0x2000, 0x1000, 0x71000000, 0x7F000000);
        if (!client->workerAlloc) return fail(E_OUTOFMEMORY);
        client->workerCtx.r13.u64 = client->workerAlloc;
        client->workerCtx.r1.u64 = client->workerAlloc + 0x2000 + kWorkerStackSize - 0x100;
        owner->initThreadStorage(client->workerAlloc, client->workerAlloc + 0x2000, kWorkerStackSize, 0);
        DWORD oldProtection = 0;
        if (!VirtualProtect(owner->base() + client->workerAlloc + 0x1000, 0x1000, PAGE_NOACCESS,
                            &oldProtection)) return fail(HRESULT_FROM_WIN32(GetLastError()));
        WAVEFORMATEXTENSIBLE wfx{};
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = kAudioChannels;
        wfx.Format.nSamplesPerSec = kAudioRateHz;
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = kAudioChannels * 4;
        wfx.Format.nAvgBytesPerSec = kAudioRateHz * kAudioChannels * 4;
        wfx.Format.cbSize = 22;
        wfx.Samples.wValidBitsPerSample = 32;
        wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                            SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        client->voiceCb.client = client.get();
        HRESULT hr = gDriver.engine->CreateSourceVoice(&client->voice, (WAVEFORMATEX*)&wfx, 0,
                                                       1.0f, &client->voiceCb);
        if (FAILED(hr)) return fail(hr);
        XAUDIO2_VOICE_DETAILS destination{};
        gDriver.master->GetVoiceDetails(&destination);
        std::vector<float> matrix(size_t(kAudioChannels) * destination.InputChannels);
        client->voice->GetOutputMatrix(gDriver.master, kAudioChannels, destination.InputChannels, matrix.data());
        const float headroom = audioOutputHeadroom(matrix, kAudioChannels);
        if (!(headroom > 0)) return fail(E_INVALIDARG);
        hr = client->voice->SetVolume(headroom);
        if (FAILED(hr)) return fail(hr);
        std::fprintf(stderr, "[Audio] output channels=%u downmixHeadroom=%.9g\n",
                     destination.InputChannels, headroom);
        client->stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!client->stopEvent) return fail(HRESULT_FROM_WIN32(GetLastError()));
        client->creditEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!client->creditEvent) return fail(HRESULT_FROM_WIN32(GetLastError()));
        client->readyEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!client->readyEvent) return fail(HRESULT_FROM_WIN32(GetLastError()));
        client->startEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!client->startEvent) return fail(HRESULT_FROM_WIN32(GetLastError()));
        client->startupTimer = CreateThreadpoolTimer(audioStartupDeadline, client.get(), nullptr);
        if (!client->startupTimer) return fail(HRESULT_FROM_WIN32(GetLastError()));
        auto* held = new std::shared_ptr<Impl::Client>(client);
        client->thread = CreateThread(nullptr, 0, audioDriverWorkerMain, held, 0, &client->workerThreadId);
        if (!client->thread) {
            DWORD error = GetLastError();
            delete held;
            return fail(HRESULT_FROM_WIN32(error));
        }
        // Keep lifecycle ownership while waiting: teardown cannot close readyEvent.
        DWORD readyWait = WaitForSingleObject(client->readyEvent, 5000);
        if (readyWait != WAIT_OBJECT_0)
            return fail(HRESULT_FROM_WIN32(readyWait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()));
        if (FAILED(client->readyStatus)) return fail(client->readyStatus);
        if (client->pcmTap.initializeFromEnvironment()) audioLogOutputConfiguration(client.get());
        audioResamplerTrace.initialize();
        owner->write32(client->workerAlloc + Memory::threadObjectOffset + 0x14c, client->workerThreadId);
        client->token = kTokenBase | gDriver.nextGen++;
        client->active.store(true, std::memory_order_release);
        gDriver.active = client;
        uint32_t previousOut = owner->read32(driverOut);
        owner->write32(driverOut, client->token);
        if (!SetEvent(client->startEvent)) {
            HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            owner->write32(driverOut, previousOut);
            client->active.store(false, std::memory_order_release);
            gDriver.active.reset();
            return fail(error);
        }
        fprintf(stderr, "[Audio] render client registered callback=0x%08X token=0x%08X driver=0x%08X\n",
                guestCallback, client->token, driverOut);
        return 0;
    } catch (const std::bad_alloc&) {
        return fail(E_OUTOFMEMORY);
    }
}

uint32_t AudioRenderDriver::submitFrame(uint32_t token, uint32_t samplesGuest) {
    AudioComScope com;
    HRESULT comHr = com.hr;
    if (FAILED(comHr)) {
        audioNoteError(comHr);
        return uint32_t(comHr);
    }
    std::lock_guard lock(gDriver.mutex);
    Impl::Client* client = gDriver.active.get();
    if (!client || !client->active.load(std::memory_order_acquire) || token != client->token)
        return kBadHandle;
    if (HRESULT err = client->voiceError.load(std::memory_order_acquire); err != S_OK)
        return uint32_t(err);
    Memory* owner = client->owner;
    if (!audioGuestSpan(owner, samplesGuest, kAudioSubmitBytes)) return kBadHandle;
    audioReclaimCompleted(client);
    if (client->queued.size() >= kMaxQueued) return kBusyHresult;
    std::vector<float> snapshot;
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = kAudioSubmitBytes;
    try {
        std::vector<float> interleaved(size_t(kAudioFramesPerSubmit) * kAudioChannels);
        audioConvertPlanarBEFloat(owner->base() + samplesGuest, interleaved.data());
        snapshot = interleaved;
        buffer.pAudioData = reinterpret_cast<const BYTE*>(interleaved.data());
        // Establish ownership before XAudio2 can read the buffer. No throwing
        // allocation may occur between acceptance and retaining its storage.
        client->queued.push_back(std::move(interleaved));
    } catch (const std::bad_alloc&) {
        return uint32_t(E_OUTOFMEMORY);
    }
    HRESULT hr = client->voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        client->queued.pop_back(); // Rejected buffers never get OnBufferEnd.
        audioNoteError(hr);
        fprintf(stderr, "[Audio] SubmitSourceBuffer failed hr=0x%08X\n", unsigned(hr));
        return uint32_t(hr);
    }
    client->lastSubmitted = std::move(snapshot);
    client->submittedSeq++;
    // Exact accepted game PCM only. The opt-in tap copies to preallocated memory;
    // its writer performs all capture file I/O away from the producer/device.
    client->pcmTap.submit(client->lastSubmitted.data(), client->submittedSeq, samplesGuest);
    audioResamplerTrace.submitted(client->submittedSeq);
    if (GetCurrentThreadId() == client->workerThreadId) ++client->workerSubmissions;
    gDriver.framesSubmitted++;
    gDriver.samplesQueued += kAudioFramesPerSubmit;
    const HRESULT start = audioStartIfReady(client);
    if (client->submittedSeq == 1 && !client->playbackStarted.load(std::memory_order_acquire)) {
        ULARGE_INTEGER delay{};
        delay.QuadPart = uint64_t(-int64_t(kAudioPrefillDeadlineMs) * 10000);
        FILETIME due{delay.LowPart, delay.HighPart};
        SetThreadpoolTimer(client->startupTimer, &due, 0, 0);
    }
    if (client->submittedSeq == kAudioPrefillBuffers)
        audioLogContinuity(client);
    return uint32_t(start);
}

uint32_t AudioRenderDriver::unregisterClient(uint32_t token) {
    std::shared_ptr<Impl::Client> gone;
    {
        std::lock_guard lock(gDriver.mutex);
        if (!gDriver.active || token != gDriver.active->token) return kBadHandle;
        if (GetCurrentThreadId() == gDriver.active->workerThreadId) return kBusyHresult;
        gDriver.active->active.store(false, std::memory_order_release);
        SetEvent(gDriver.active->stopEvent);
        gone = std::move(gDriver.active);
        gDriver.closing = gone;
    }
    audioJoinAndTeardown(gone);
    {
        std::lock_guard lock(gDriver.mutex);
        if (gDriver.closing == gone) {
            gDriver.closing.reset();
            gDriver.closingDone.notify_all();
        }
    }
    fprintf(stderr, "[Audio] render client unregistered token=0x%08X\n", gone->token);
    return 0;
}

void AudioRenderDriver::shutdown() { shutdownForMemory(nullptr); }

void AudioRenderDriver::shutdownForMemory(Memory* owner) {
    // The engine's MTA cookie survives all callers. Cleanup must also work on an
    // STA caller, where CoInitializeEx(MTA) would fail with RPC_E_CHANGED_MODE.
    std::shared_ptr<Impl::Client> gone;
    {
        std::unique_lock lock(gDriver.mutex);
        auto matches = [&](const std::shared_ptr<Impl::Client>& c) {
            return c && (!owner || c->owner == owner);
        };
        if (matches(gDriver.active)) {
            if (GetCurrentThreadId() == gDriver.active->workerThreadId)
                audioFatalSelfTeardown("owner shutdown");
            gDriver.active->active.store(false, std::memory_order_release);
            SetEvent(gDriver.active->stopEvent);
            gone = std::move(gDriver.active);
            gDriver.closing = gone;
        } else if (matches(gDriver.closing)) {
            auto closing = gDriver.closing;
            if (GetCurrentThreadId() == closing->workerThreadId)
                audioFatalSelfTeardown("owner shutdown during unregister");
            gDriver.closingDone.wait(lock, [&] { return gDriver.closing != closing; });
        }
    }
    audioJoinAndTeardown(gone);
    std::lock_guard lock(gDriver.mutex);
    if (gone && gDriver.closing == gone) {
        gDriver.closing.reset();
        gDriver.closingDone.notify_all();
    }
    if (!gDriver.active && !gDriver.closing) audioReleaseEngine();
}

AudioDriverCounters AudioRenderDriver::counters() {
    AudioDriverCounters out{};
    out.callbacks = gDriver.callbacks.load();
    out.framesSubmitted = gDriver.framesSubmitted.load();
    out.samplesQueued = gDriver.samplesQueued.load();
    out.buffersCompleted = gDriver.completed.load();
    out.deviceErrors = gDriver.deviceErrors.load();
    out.lastHresult = gDriver.lastHresult.load();
    out.starvationPasses = gDriver.starvationPasses.load();
    out.starvationBytes = gDriver.starvationBytes.load();
    out.maxCallbackMicros = gDriver.maxCallbackMicros.load();
    out.callbacksWithoutSubmit = gDriver.callbacksWithoutSubmit.load();
    std::lock_guard lock(gDriver.mutex);
    out.workerRunning = gDriver.active && gDriver.active->active.load();
    out.deviceReady = gDriver.engine != nullptr;
    if (gDriver.engine) {
        XAUDIO2_PERFORMANCE_DATA perf{};
        gDriver.engine->GetPerformanceData(&perf);
        out.engineGlitches = perf.GlitchesSinceEngineStarted;
    }
    if (gDriver.active) {
        auto* client = gDriver.active.get();
        XAUDIO2_VOICE_STATE state{};
        client->voice->GetState(&state);
        out.queuedBuffers = state.BuffersQueued;
        const uint64_t submittedFrames = client->submittedSeq * kAudioFramesPerSubmit;
        out.queuedFrames = submittedFrames > state.SamplesPlayed ? submittedFrames - state.SamplesPlayed : 0;
        out.playbackStarted = client->playbackStarted.load();
        out.workerMmcss = client->workerMmcss.load();
    }
    return out;
}

void audioDriverShutdownForMemory(Memory* owner) { AudioRenderDriver::instance().shutdownForMemory(owner); }
}
