#include "disc_stream.h"

#include <algorithm>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

namespace {
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

struct DiscStream {
    IRandomAccessStream stream{nullptr};
    uint64_t size{};
};

struct Apartment {
    bool initialized{};
    Apartment() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        } catch (const winrt::hresult_error& e) {
            if (e.code() != winrt::hresult{static_cast<int32_t>(0x80010106u)}) throw;
        }
    }
    ~Apartment() { if (initialized) winrt::uninit_apartment(); }
};

int64_t read_at(void* opaque, uint64_t offset, void* output, size_t length) {
    auto* disc = static_cast<DiscStream*>(opaque);
    if (!disc || !output || offset > disc->size) return -1;
    try {
        thread_local Apartment apartment;
        auto* bytes = static_cast<uint8_t*>(output);
        size_t done = 0;
        while (done < length && offset + done < disc->size) {
            const auto count = static_cast<uint32_t>(std::min<uint64_t>(
                {length - done, disc->size - offset - done, uint64_t{1024 * 1024}}));
            auto input = disc->stream.GetInputStreamAt(offset + done);
            auto buffer = Buffer(count);
            auto read_buffer = input.ReadAsync(buffer, count, InputStreamOptions::None).get();
            const auto received = read_buffer.Length();
            if (!received) break;
            DataReader::FromBuffer(read_buffer).ReadBytes({bytes + done, received});
            done += received;
        }
        return static_cast<int64_t>(done);
    } catch (...) {
        return -1;
    }
}

int64_t stream_len(void* opaque) {
    auto* disc = static_cast<DiscStream*>(opaque);
    if (!disc || disc->size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return -1;
    return static_cast<int64_t>(disc->size);
}

void stream_close(void* opaque) { delete static_cast<DiscStream*>(opaque); }
}

extern "C" int melee_xbox_open_disc_stream(const char* utf8_path, NodDiscStream* stream) {
    if (!utf8_path || !stream) return 0;
    try {
        auto disc = std::async(std::launch::async, [path = std::string(utf8_path)] {
            Apartment apartment;
            auto file = StorageFile::GetFileFromPathAsync(winrt::to_hstring(path)).get();
            auto access = file.OpenAsync(FileAccessMode::Read).get();
            return std::make_unique<DiscStream>(DiscStream{access, access.Size()});
        }).get();
        *stream = NodDiscStream{
            .user_data = disc.release(),
            .read_at = read_at,
            .stream_len = stream_len,
            .close = stream_close,
        };
        return 1;
    } catch (...) {
        return 0;
    }
}
