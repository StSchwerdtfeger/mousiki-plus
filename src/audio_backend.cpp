// Platform audio context: picks the miniaudio backend for this platform (WASAPI on Windows, OpenSL ES
// on Android, the default elsewhere). It is used for decoding, not for speaker output (that is the
// player's job).
#include "audio_backend.h"
#include <cstddef>
#include <cstdlib>

namespace muisc {

bool init_platform_audio_context(ma_context& out_context) {
#if defined(MUISC_PLATFORM_ANDROID)
    ma_backend backends[] = { ma_backend_opensl };
#elif defined(MUISC_PLATFORM_WINDOWS)
    ma_backend backends[] = { ma_backend_wasapi };
#elif defined(MUISC_PLATFORM_LINUX)
    ma_backend backends[] = { ma_backend_pulseaudio, ma_backend_alsa };
#elif defined(MUISC_PLATFORM_MACOS)
    ma_backend backends[] = { ma_backend_coreaudio };
#else
    ma_backend backends[] = { ma_backend_null };
#endif

    ma_context_config cfg = ma_context_config_init();
    ma_result result = ma_context_init(backends, sizeof(backends) / sizeof(backends[0]), &cfg, &out_context);
    return result == MA_SUCCESS;
}

const char* platform_backend_name() {
#if defined(MUISC_PLATFORM_ANDROID)
    return "OpenSL ES (Android)";
#elif defined(MUISC_PLATFORM_WINDOWS)
    return "WASAPI (Windows)";
#elif defined(MUISC_PLATFORM_LINUX)
    return "PulseAudio/ALSA -> PipeWire (Linux)";
#elif defined(MUISC_PLATFORM_MACOS)
    return "Core Audio (macOS)";
#else
    return "null";
#endif
}

std::vector<AudioOutputDevice> list_output_devices(std::string* err) {
    std::vector<AudioOutputDevice> out;
    ma_context ctx;
    const char* a = std::getenv("MOUSIKI_NULL_AUDIO");
    const char* b = std::getenv("MOUSIKI_RADIO_NULL");
    bool ok;
    if ((a && *a == '1') || (b && *b == '1')) {
        ma_backend nb[] = { ma_backend_null };
        ok = ma_context_init(nb, 1, nullptr, &ctx) == MA_SUCCESS;
    } else {
        ok = init_platform_audio_context(ctx);
    }
    if (!ok) { if (err) *err = "no audio backend available"; return out; }
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) {
        if (err) *err = "the audio devices could not be listed";
    } else {
        for (ma_uint32 i = 0; i < count; ++i) {
            AudioOutputDevice d;
            d.name = infos[i].name;
            d.is_default = infos[i].isDefault != 0;
            if (!d.name.empty()) out.push_back(std::move(d));
        }
    }
    ma_context_uninit(&ctx);
    return out;
}

bool find_output_device(ma_context& ctx, const std::string& name, ma_device_id& out) {
    if (name.empty()) return false;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return false;
    for (ma_uint32 i = 0; i < count; ++i)
        if (name == infos[i].name) { out = infos[i].id; return true; }
    return false;
}

} // namespace muisc
