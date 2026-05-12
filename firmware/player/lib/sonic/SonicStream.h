#pragma once
/**
 * SonicStream.h
 * Thin AudioTools-compatible wrapper around Bill Cox's Sonic library.
 *
 * Sonic uses WSOLA (Waveform-Similarity Overlap-Add) to change playback
 * speed WITHOUT changing pitch — exactly what we need for the barrel organ.
 *
 * Pipeline usage:
 *   AudioPlayer → SonicStream → I2SStream
 *
 * SonicStream is an AudioOutput: it accepts write() calls from upstream
 * (AudioPlayer / WAVDecoder), feeds them through the Sonic engine, and
 * immediately forwards the processed samples to the next Print* stream.
 */

extern "C" {
#include "sonic.h"
}
#include "AudioTools.h"

class SonicStream : public audio_tools::AudioOutput {
public:
    explicit SonicStream(Print &out) : p_out(&out) {}

    ~SonicStream() { destroySonic(); }

    /// Call after the WAV format is known (sample_rate, channels, bits).
    bool begin(audio_tools::AudioInfo info) {
        cfg = info;
        setAudioInfo(info);
        return restartSonic();
    }

    /// Change playback speed at any time (1.0 = normal, 1.5 = 50% faster, …).
    /// Pitch is always held at 1.0 — Sonic handles that internally.
    void setSpeed(float s) {
        speed = constrain(s, 0.2f, 4.0f);
        if (stream) sonicSetSpeed(stream, speed);
    }

    float getSpeed() const { return speed; }

    // AudioOutput interface ──────────────────────────────────────────────────

    bool begin() override { return restartSonic(); }

    void end() override {
        if (stream) {
            sonicFlushStream(stream);
            drainToOutput();
        }
        destroySonic();
    }

    /// Accepts interleaved int16_t samples, forwards time-stretched audio.
    size_t write(const uint8_t *data, size_t len) override {
        if (!stream || len == 0) return 0;

        const int16_t *samples   = reinterpret_cast<const int16_t *>(data);
        int            numFrames = static_cast<int>(len / sizeof(int16_t)) / cfg.channels;

        if (numFrames <= 0) return len;

        // Sonic's write takes frame count; it multiplies by numChannels internally.
        if (!sonicWriteShortToStream(stream,
                                     const_cast<short *>(samples),
                                     numFrames)) {
            // realloc failed — reset and keep going rather than hard crash
            restartSonic();
            return len;
        }

        drainToOutput();
        return len;
    }

private:
    Print                    *p_out  = nullptr;
    sonicStream               stream = nullptr;
    audio_tools::AudioInfo    cfg;
    float                     speed  = 1.0f;

    // Read-back buffer; allocated once and grown as needed.
    static constexpr int kReadFrames = 1024;
    int16_t readBuf[kReadFrames * 2];   // up to stereo

    bool restartSonic() {
        destroySonic();
        stream = sonicCreateStream(
            cfg.sample_rate  ? cfg.sample_rate  : 44100,
            cfg.channels     ? cfg.channels     : 2);
        if (!stream) return false;
        sonicSetSpeed(stream, speed);
        sonicSetPitch(stream, 1.0f);   // never change pitch
        // Quality 1: disables AMDF down-sampling (skip=1 at 44100 Hz).
        // Pitch detection runs at full sample rate → much more accurate
        // period finding, especially for high-pitched treble pipes.
        sonicSetQuality(stream, 0);
        return true;
    }

    void destroySonic() {
        if (stream) {
            sonicDestroyStream(stream);
            stream = nullptr;
        }
    }

    /// Drain all available output frames from Sonic into the downstream stream.
    void drainToOutput() {
        if (!stream || !p_out) return;
        int framesAvail;
        while ((framesAvail = sonicSamplesAvailable(stream)) > 0) {
            int toRead = min(framesAvail, kReadFrames);
            int got    = sonicReadShortFromStream(stream, readBuf, toRead);
            if (got <= 0) break;
            p_out->write(reinterpret_cast<uint8_t *>(readBuf),
                         got * cfg.channels * sizeof(int16_t));
        }
    }
};
