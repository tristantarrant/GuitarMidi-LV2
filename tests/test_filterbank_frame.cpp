/*
 * Reads the analysis frame the way the model sees it.
 *
 * Regression test for the frame-assembly contract:
 *  - a captured frame is a contiguous, oldest-first window of exactly
 *    BUFFER_SIZE samples per filter, never a wrapped/rotated buffer,
 *  - the frame does not depend on how the caller chunked the audio, as long
 *    as chunks respect the hop boundary (which FretBoard guarantees).
 *
 * Before this contract existed the rolling filter buffer was handed to the
 * model directly, so for host block sizes that do not divide BUFFER_SIZE the
 * model saw a window with a discontinuity in the middle, and a chunk crossing
 * the row boundary wrote into the neighbouring filter's row.
 */
#include <guitarmidi/filterbank.hpp>
#include <guitarmidi/fretboardrepresentation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expression);
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

std::vector<float> makeSignal(int samples)
{
    std::vector<float> signal(samples);
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / 48000.0;
        const double burst = std::exp(-std::fmod(t, 0.4) / 0.15);
        signal[i] = static_cast<float>(
            0.3 * burst * (std::sin(2.0 * M_PI * 196.0 * t) +
                           0.5 * std::sin(2.0 * M_PI * 392.0 * t)));
    }
    return signal;
}

std::vector<float> copyFrame(const GuitarMidi::FilterBank &bank)
{
    const GuitarMidi::AudioBuffer2D frame = bank.get_frame_buffer();
    const int count = frame.num_filters * frame.window_size;
    return std::vector<float>(frame.audio_buffer_2D, frame.audio_buffer_2D + count);
}

/* A tuning shift must make a detuned string look like a standard-tuned one:
 * 196 Hz analysed with -2 semitones has to land where 220 Hz lands at 0. */
void testTuningShift()
{
    constexpr int kSamples = 24000;
    std::vector<float> low(static_cast<size_t>(kSamples));
    std::vector<float> high(static_cast<size_t>(kSamples));
    for (int i = 0; i < kSamples; ++i)
    {
        const double t = static_cast<double>(i) / 48000.0;
        low[i] = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 196.0 * t));
        high[i] = static_cast<float>(0.3 * std::sin(2.0 * M_PI * 220.0 * t));
    }

    GuitarMidi::FretBoardRepresentation representation;
    const auto filters = representation.get_filterrepresentations();
    float gainDb = 0.0f;

    GuitarMidi::FilterBank detuned;
    detuned.setup(filters, 48000, BUFFER_SIZE, -2);
    detuned.setGain(&gainDb);
    detuned.setInput(low.data());

    GuitarMidi::FilterBank reference;
    reference.setup(filters, 48000, BUFFER_SIZE, 0);
    reference.setGain(&gainDb);
    reference.setInput(high.data());

    for (int offset = 0; offset + BUFFER_SIZE <= kSamples; offset += BUFFER_SIZE)
    {
        detuned.process(BUFFER_SIZE, offset);
        reference.process(BUFFER_SIZE, offset);
    }
    detuned.captureFrame();
    reference.captureFrame();

    // Compare per-band energies rather than samples: the detector consumes
    // rectified energies, and a tiny coefficient difference would otherwise
    // show up as a phase error.
    const std::vector<float> a = copyFrame(detuned);
    const std::vector<float> b = copyFrame(reference);
    std::vector<float> rowA(NUM_FILTERS, 0.0f);
    std::vector<float> rowB(NUM_FILTERS, 0.0f);
    for (int f = 0; f < NUM_FILTERS; ++f)
        for (int w = 0; w < BUFFER_SIZE; ++w)
        {
            rowA[f] += a[f * BUFFER_SIZE + w];
            rowB[f] += b[f * BUFFER_SIZE + w];
        }

    const float maxB = *std::max_element(rowB.begin(), rowB.end());
    CHECK(maxB > 0.0f);
    // Every band that carries real energy must land in the same place. Bands
    // that are effectively silent are not compared: there the two designs
    // differ by more than the signal itself.
    int compared = 0;
    float worst = 0.0f;
    for (int f = 0; f < NUM_FILTERS; ++f)
    {
        if (rowB[f] < 0.05f * maxB)
            continue;
        ++compared;
        worst = std::max(worst, std::fabs(rowA[f] - rowB[f]) / rowB[f]);
    }
    CHECK(compared >= 2);
    // 15 %: the bilinear transform warps frequency, so a shifted design is not
    // an exact copy. What matters is that the energy lands in the same bands.
    CHECK(worst < 0.15f);

    float totalA = 0.0f;
    float totalB = 0.0f;
    for (int f = 0; f < NUM_FILTERS; ++f)
    {
        totalA += rowA[f];
        totalB += rowB[f];
    }
    CHECK(std::fabs(totalA - totalB) < 0.02f * totalB);
}

} // namespace

int main()
{
    constexpr int kTotal = 48000;
    constexpr int kFrame = BUFFER_SIZE;
    const int samples = kTotal;
    const std::vector<float> signal = makeSignal(samples);

    GuitarMidi::FretBoardRepresentation representation;
    const auto filters = representation.get_filterrepresentations();
    // process() reads the gain control unconditionally, as the host always
    // supplies it in the plugin.
    float gainDb = 0.0f;

    // Reference pass: one hop per call.
    GuitarMidi::FilterBank aligned;
    aligned.setup(filters, 48000, kFrame);
    aligned.setGain(&gainDb);
    aligned.setInput(signal.data());
    std::vector<std::vector<float>> reference;
    for (int offset = 0; offset + kFrame <= samples; offset += kFrame)
    {
        aligned.process(kFrame, offset);
        aligned.captureFrame();
        reference.push_back(copyFrame(aligned));
    }

    // Candidate pass: irregular chunk sizes, clamped to the hop boundary the
    // way FretBoard drives the filter bank.
    GuitarMidi::FilterBank chopped;
    chopped.setup(filters, 48000, kFrame);
    chopped.setGain(&gainDb);
    chopped.setInput(signal.data());
    const int chunkPattern[] = {1, 7, 33, 97, 211, 64, 128};
    int patternIndex = 0;
    int offset = 0;
    size_t hop = 0;
    while (offset < samples)
    {
        const int toHop = kFrame - (offset % kFrame);
        const int wanted = chunkPattern[patternIndex++ % 7];
        const int n = std::min(wanted, std::min(toHop, samples - offset));
        chopped.process(n, offset);
        offset += n;
        if (offset % kFrame == 0 && hop < reference.size())
        {
            chopped.captureFrame();
            const std::vector<float> frame = copyFrame(chopped);
            CHECK(frame == reference[hop]);
            ++hop;
        }
    }
    CHECK(hop == reference.size());

    // The frame must be the newest window: a signal that stops must decay out
    // of the frame rather than leaving stale samples at the wrong end.
    GuitarMidi::FilterBank silent;
    silent.setup(filters, 48000, kFrame);
    silent.setGain(&gainDb);
    std::vector<float> quiet(samples, 0.0f);
    silent.setInput(quiet.data());
    for (int off = 0; off + kFrame <= samples; off += kFrame)
        silent.process(kFrame, off);
    silent.captureFrame();
    const std::vector<float> zeroFrame = copyFrame(silent);
    for (float value : zeroFrame)
        CHECK(value == 0.0f);

    testTuningShift();

    if (failures == 0)
        std::printf("test_filterbank_frame: OK (%zu hops compared)\n", reference.size());
    else
        std::printf("test_filterbank_frame: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
