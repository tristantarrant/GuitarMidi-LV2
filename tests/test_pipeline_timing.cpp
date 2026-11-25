/*
 * End-to-end timing tests for the detection pipeline.
 *
 * These need the shipped model (python/neuralnetmodelling/guitarmidi.tflite)
 * and exercise the contract the timestamps introduced:
 *
 *  - notes are detected at every host block size, including sizes that do not
 *    divide the analysis hop (100, 300), because the model now always sees a
 *    contiguous frame,
 *  - every emitted event is stamped inside the block that produced it and
 *    never before the audio that caused it,
 *  - event times are monotone,
 *  - with a model slower than real time the bounded queues refuse frames
 *    (instead of overwriting them), the pipeline reports the resulting holes,
 *    and no note is left hanging.
 */
#include <guitarmidi/fretboard.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const std::string &what, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)
#define CHECK_MSG(condition, message) check((condition), (message), __LINE__)

constexpr int kSampleRate = 48000;

struct CaptureOutput : GuitarMidiOutput
{
    struct Event
    {
        int64_t frame;
        int64_t blockStart;
        int blockSize;
        uint8_t note;
        uint8_t velocity;
        bool on;
    };

    std::vector<Event> events;
    int64_t blockStart = 0;
    int blockSize = 0;

    void initializeSequence() override {}
    void finalizeSequence() override {}
    void sendMidiMessage(uint8_t midinote[3], int64_t frames) override
    {
        events.push_back({blockStart + frames, blockStart, blockSize,
                          midinote[1], midinote[2], midinote[2] > 0});
    }
};

double noteFrequency(int midi)
{
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

void addPluck(std::vector<float> &buffer, int start, int midi, double amplitude)
{
    const double f0 = noteFrequency(midi);
    const int length = std::min<int>(static_cast<int>(kSampleRate * 0.4),
                                     static_cast<int>(buffer.size()) - start);
    for (int i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        const double attack = std::min(1.0, t / 0.002);
        const double envelope = attack * std::exp(-t / 0.18);
        double sample = 0.0;
        for (int h = 1; h <= 10; ++h)
        {
            const double frequency = f0 * h;
            if (frequency > kSampleRate * 0.45)
                break;
            sample += std::sin(2.0 * M_PI * frequency * t) / h;
        }
        buffer[start + i] += static_cast<float>(amplitude * envelope * sample * 0.3);
    }
}

struct Controls
{
    float gain = 20.0f;
    float expressivity = 7.0f;
    float smoothing = 0.3f;
    float smoothingOffset = 0.3f;
    float onsetThreshold = 0.80f;
    float offsetThreshold = 0.10f;
    float onsetEnergy = -66.0f;
    float offsetEnergy = -66.0f;
};

struct RunResult
{
    std::vector<double> latenciesMs;
    int detected = 0;
    int falsePositives = 0;
    int outOfOrder = 0;
    GuitarMidi::NoteInferenceStats stats;
};

RunResult runCase(int blockSize, const std::vector<int> &pitches, double spacing,
                  bool realtime, int inferenceDelayMs, int backpressureMs = 0)
{
    Controls controls;
    auto *output = new CaptureOutput();
    output->blockSize = blockSize;
    FretBoard board(output);
    board.setGain(&controls.gain);
    board.setExpressivity(&controls.expressivity);
    board.setSmoothing(&controls.smoothing);
    board.setSmoothingOffset(&controls.smoothingOffset);
    board.setOnsetThreshold(&controls.onsetThreshold);
    board.setOffsetThreshold(&controls.offsetThreshold);
    board.setOnsetEnergyThreshold(&controls.onsetEnergy);
    board.setOffsetEnergyThreshold(&controls.offsetEnergy);
    board.setInferenceDelayMs(inferenceDelayMs);
    board.setBackpressureMs(backpressureMs);

    if (!board.initialize(GUITARMIDI_TEST_MODEL_DIR, kSampleRate, 2048))
    {
        std::fprintf(stderr, "FAIL: could not initialize the model from %s\n", GUITARMIDI_TEST_MODEL_DIR);
        std::exit(1);
    }

    const double leadIn = 0.35;
    const int totalSamples = static_cast<int>((leadIn + pitches.size() * spacing + 0.5) * kSampleRate);
    std::vector<float> audio(totalSamples, 0.0f);
    std::vector<int> onsets;
    for (size_t i = 0; i < pitches.size(); ++i)
    {
        const int start = static_cast<int>((leadIn + i * spacing) * kSampleRate);
        onsets.push_back(start);
        addPluck(audio, start, pitches[i], 0.25);
    }

    std::vector<float> block(blockSize, 0.0f);
    const int blocks = (totalSamples + blockSize - 1) / blockSize;
    const auto startTime = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        const int offset = b * blockSize;
        const int n = std::min(blockSize, totalSamples - offset);
        std::fill(block.begin(), block.end(), 0.0f);
        std::memcpy(block.data(), audio.data() + offset, n * sizeof(float));
        if (realtime)
        {
            const auto due = startTime +
                             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double>(static_cast<double>(offset) / kSampleRate));
            std::this_thread::sleep_until(due);
        }
        output->blockStart = offset;
        board.setAudioInput(block.data());
        board.process(n);
    }

    RunResult result;
    result.stats = board.getInferenceStats();

    // Every event must be stamped inside the block that emitted it.
    for (const auto &event : output->events)
        CHECK_MSG(event.frame >= event.blockStart && event.frame - event.blockStart < event.blockSize,
                  "event outside its block");

    for (size_t e = 1; e < output->events.size(); ++e)
        if (output->events[e].frame < output->events[e - 1].frame)
            ++result.outOfOrder;

    int matched[16];
    std::fill(std::begin(matched), std::end(matched), -1);
    for (size_t i = 0; i < pitches.size(); ++i)
    {
        const int windowStart = onsets[i];
        const int windowEnd = (i + 1 < pitches.size())
                                  ? onsets[i + 1]
                                  : onsets[i] + static_cast<int>(0.45 * kSampleRate);
        for (size_t e = 0; e < output->events.size(); ++e)
        {
            const auto &event = output->events[e];
            if (event.on && event.note == pitches[i] && event.frame >= windowStart && event.frame < windowEnd)
            {
                matched[i] = static_cast<int>(e);
                result.latenciesMs.push_back(
                    static_cast<double>(event.frame - windowStart) * 1000.0 / kSampleRate);
                ++result.detected;
                break;
            }
        }
    }
    for (size_t e = 0; e < output->events.size(); ++e)
    {
        bool expected = false;
        for (size_t i = 0; i < pitches.size(); ++i)
            if (matched[i] == static_cast<int>(e))
                expected = true;
        if (output->events[e].on && !expected)
            ++result.falsePositives;
    }

    // No note may stay held once the input has been silent for a while.
    float smoothed[NUM_NOTES] = {};
    float energies[NUM_NOTES] = {};
    float momentary[NUM_NOTES] = {};
    float momentaryEnergy[NUM_NOTES] = {};
    bool noteOn[NUM_NOTES] = {};
    board.getNoteDebugData(smoothed, energies, momentary, momentaryEnergy, noteOn);
    for (int i = 0; i < NUM_NOTES; ++i)
        CHECK_MSG(!noteOn[i], "note still held after the input went silent");

    board.finalize();
    return result;
}

double meanOf(const std::vector<double> &values)
{
    if (values.empty())
        return 0.0;
    double sum = 0.0;
    for (double value : values)
        sum += value;
    return sum / values.size();
}

double stddevOf(const std::vector<double> &values)
{
    if (values.empty())
        return 0.0;
    const double mean = meanOf(values);
    double sum = 0.0;
    for (double value : values)
        sum += (value - mean) * (value - mean);
    return std::sqrt(sum / values.size());
}

} // namespace

int main()
{
    // The declared latency must match the scheduling constant.
    CHECK(GuitarMidi::NoteInferencer::latencyFrames() == (1 + PIPELINE_LATENCY_HOPS) * BUFFER_SIZE);

    const std::vector<int> pitches = {40, 45, 50};
    const double spacing = 0.7;
    const double declaredLatencyMs = 1000.0 * GuitarMidi::NoteInferencer::latencyFrames() / kSampleRate;

    for (int blockSize : {64, 100, 256, 300, 480})
    {
        const RunResult result = runCase(blockSize, pitches, spacing, true, 0);
        const double maxLatency = result.latenciesMs.empty()
                                      ? 0.0
                                      : *std::max_element(result.latenciesMs.begin(), result.latenciesMs.end());
        std::printf("block %3d: detected %d/%zu, latency mean %.2f ms, max %.2f ms, "
                    "sd %.2f ms, false positives %d, out of order %d, gaps %llu, "
                    "input drops %llu, inference mean %.2f ms max %.2f ms\n",
                    blockSize, result.detected, pitches.size(),
                    meanOf(result.latenciesMs), maxLatency, stddevOf(result.latenciesMs),
                    result.falsePositives, result.outOfOrder,
                    static_cast<unsigned long long>(result.stats.streamGaps),
                    static_cast<unsigned long long>(result.stats.inputDropped),
                    result.stats.meanInferenceMs, result.stats.maxInferenceMs);

        CHECK_MSG(result.detected == static_cast<int>(pitches.size()),
                  "note missing at block size " + std::to_string(blockSize));
        CHECK_MSG(result.outOfOrder == 0, "events out of order at block size " + std::to_string(blockSize));
        for (double latency : result.latenciesMs)
        {
            CHECK_MSG(latency >= 0.0, "event before its onset");
            // Declared delay plus one block of quantisation and the smoothing
            // ramp the detector needs to cross its threshold.
            CHECK_MSG(latency <= declaredLatencyMs + 60.0,
                      "latency above the declared delay plus margin at block size " +
                          std::to_string(blockSize));
        }
        if (result.stats.maxInferenceMs < 4.0)
        {
            CHECK_MSG(result.stats.inputDropped == 0,
                      "a fast model must not overflow the input queue");
            CHECK_MSG(result.stats.streamGaps == 0,
                      "a fast model must not produce stream gaps");
        }
    }

    // A model that cannot keep up: the queue must refuse frames, the pipeline
    // must report the holes, and the held notes must be flushed.
    // One note is enough: the point is that a model slower than the schedule
    // is reported and flushed, not that it recognises the whole phrase.
    const std::vector<int> slowPitches = {50};
    const RunResult slow = runCase(256, slowPitches, 0.6, true, 40);
    std::printf("slow inference: detected %d/%zu, gaps %llu, input drops %llu, "
                "output drops %llu, stale %llu, late %llu\n",
                slow.detected, slowPitches.size(),
                static_cast<unsigned long long>(slow.stats.streamGaps),
                static_cast<unsigned long long>(slow.stats.inputDropped),
                static_cast<unsigned long long>(slow.stats.outputDropped),
                static_cast<unsigned long long>(slow.stats.staleDropped),
                static_cast<unsigned long long>(slow.stats.late));
    CHECK_MSG(slow.stats.inputDropped > 0, "slow inference must overflow the bounded input queue");
    CHECK_MSG(slow.stats.streamGaps > 0, "holes in the stream must be reported");
    CHECK_MSG(slow.outOfOrder == 0, "events out of order with a slow model");

    // Offline rendering processes faster than real time. With the real-time
    // policy every frame would be refused; backpressure turns the queue into a
    // proper hand-off so a render keeps its notes. The timing must match the
    // real-time run because the scheduler does not depend on wall time.
    const RunResult offline = runCase(256, pitches, spacing, false, 0, 200);
    std::printf("offline backpressure: detected %d/%zu, hops %llu, gaps %llu, "
                "input drops %llu, note-ons %llu, inference %.2f ms, "
                "latency mean %.2f ms, max %.2f ms\n",
                offline.detected, pitches.size(),
                static_cast<unsigned long long>(offline.stats.hopsProcessed),
                static_cast<unsigned long long>(offline.stats.streamGaps),
                static_cast<unsigned long long>(offline.stats.inputDropped),
                static_cast<unsigned long long>(offline.stats.noteOns),
                offline.stats.meanInferenceMs,
                meanOf(offline.latenciesMs),
                offline.latenciesMs.empty() ? 0.0
                                            : *std::max_element(offline.latenciesMs.begin(),
                                                                offline.latenciesMs.end()));
    CHECK_MSG(offline.detected == static_cast<int>(pitches.size()),
              "offline rendering with backpressure must keep every note");
    CHECK_MSG(offline.stats.inputDropped == 0, "backpressure must not drop frames");
    CHECK_MSG(offline.outOfOrder == 0, "events out of order in an offline render");

    if (failures == 0)
        std::printf("test_pipeline_timing: OK\n");
    else
        std::printf("test_pipeline_timing: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
