// libFuzzer target: see fuzz/README.md
//
// miniaudio's own MP3, FLAC and WAV decoders read every local file inside the player (the waveform, the gapless playback), so a damaged
// file reaches them directly. The input is decoded the way waveform.cpp does it (float, 2 channels, 44100 Hz), read to the end, and
// seeked in a few times. The first byte only picks where the seeks go.
#include <cstddef>
#include <cstdint>

#include "miniaudio.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    const uint8_t pick = data[0];
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
    ma_decoder dec;
    if (ma_decoder_init_memory(data + 1, size - 1, &cfg, &dec) != MA_SUCCESS) return 0;
    float buf[1024 * 2];
    ma_uint64 got = 0;
    for (int i = 0; i < 64; ++i) {   // a bounded read: a file can claim hours
        if (ma_decoder_read_pcm_frames(&dec, buf, 1024, &got) != MA_SUCCESS || got == 0) break;
    }
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total) != MA_SUCCESS) total = 0;
    const ma_uint64 targets[] = {0, 1, total / 3, total / 2, total ? total - 1 : 0, static_cast<ma_uint64>(pick) * 4410};
    for (ma_uint64 t : targets) {
        if (ma_decoder_seek_to_pcm_frame(&dec, t) != MA_SUCCESS) continue;
        (void)ma_decoder_read_pcm_frames(&dec, buf, 1024, &got);
    }
    ma_decoder_uninit(&dec);
    return 0;
}
