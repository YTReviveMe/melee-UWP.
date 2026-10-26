#include <xaudio2.h>
#include <objbase.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

extern "C" void pc_log_line(const char* fmt, ...);

namespace {
constexpr size_t kSamples = 160 * 2;
constexpr size_t kInputFrames = 32;
constexpr size_t kOutputFrames = 32;
constexpr UINT32 kMaxQueuedBuffers = 8;
using Frame = std::array<float, kSamples>;

struct Output {
    std::mutex frames_mutex;
    std::array<Frame, kInputFrames> frames{};
    size_t read = 0;
    size_t count = 0;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::atomic<float> gain{1.0f};
    std::thread worker;
};

std::mutex g_output_mutex;
std::shared_ptr<Output> g_output;

void run_output(std::shared_ptr<Output> output) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool release_com = SUCCEEDED(com_result);
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* mastering = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    HRESULT hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = engine->CreateMasteringVoice(&mastering);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 32000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * sizeof(float);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (SUCCEEDED(hr))
        hr = engine->CreateSourceVoice(&source, &format);
    if (SUCCEEDED(hr))
        hr = source->Start();

    if (FAILED(hr)) {
        pc_log_line("audio: XAudio2 output unavailable (HRESULT 0x%08X)",
                    static_cast<unsigned>(hr));
    } else {
        pc_log_line("audio: XAudio2 output ready (32 kHz stereo)");
        std::array<Frame, kOutputFrames> buffers{};
        size_t next_buffer = 0;
        while (!output->stop.load(std::memory_order_acquire)) {
            XAUDIO2_VOICE_STATE state{};
            source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued < kMaxQueuedBuffers) {
                Frame frame;
                bool available = false;
                {
                    std::lock_guard lock(output->frames_mutex);
                    if (output->count != 0) {
                        frame = output->frames[output->read];
                        output->read = (output->read + 1) % kInputFrames;
                        --output->count;
                        available = true;
                    }
                }
                if (available) {
                    buffers[next_buffer] = frame;
                    XAUDIO2_BUFFER buffer{};
                    buffer.AudioBytes = sizeof(Frame);
                    buffer.pAudioData = reinterpret_cast<const BYTE*>(buffers[next_buffer].data());
                    hr = source->SubmitSourceBuffer(&buffer);
                    if (FAILED(hr)) {
                        pc_log_line("audio: XAudio2 submit failed (HRESULT 0x%08X)",
                                    static_cast<unsigned>(hr));
                        break;
                    }
                    next_buffer = (next_buffer + 1) % kOutputFrames;
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (source)
        source->DestroyVoice();
    if (mastering)
        mastering->DestroyVoice();
    if (engine)
        engine->Release();
    if (release_com)
        CoUninitialize();
    output->finished.store(true, std::memory_order_release);
}
}

extern "C" void pc_xbox_audio_start(float gain) {
    std::lock_guard lock(g_output_mutex);
    if (g_output)
        return;
    auto output = std::make_shared<Output>();
    output->gain.store(gain, std::memory_order_relaxed);
    output->worker = std::thread(run_output, output);
    g_output = std::move(output);
}

extern "C" void pc_xbox_audio_push(const float* samples) {
    std::shared_ptr<Output> output;
    {
        std::lock_guard lock(g_output_mutex);
        output = g_output;
    }
    if (!output || output->stop.load(std::memory_order_acquire))
        return;
    std::lock_guard lock(output->frames_mutex);
    if (output->count == kInputFrames) {
        output->read = (output->read + 1) % kInputFrames;
        --output->count;
    }
    const size_t write = (output->read + output->count) % kInputFrames;
    const float gain = output->gain.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kSamples; ++i)
        output->frames[write][i] = samples[i] * gain;
    ++output->count;
}

extern "C" void pc_xbox_audio_set_volume(float gain) {
    std::lock_guard lock(g_output_mutex);
    if (g_output)
        g_output->gain.store(gain, std::memory_order_relaxed);
}

extern "C" void pc_xbox_audio_stop(void) {
    std::shared_ptr<Output> output;
    {
        std::lock_guard lock(g_output_mutex);
        output = std::move(g_output);
    }
    if (!output)
        return;
    output->stop.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!output->finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (output->worker.joinable()) {
        if (output->finished.load(std::memory_order_acquire))
            output->worker.join();
        else
            output->worker.detach();
    }
}
