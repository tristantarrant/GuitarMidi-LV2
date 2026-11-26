/*
 * Measures the delay the zita resampler adds between the host sample rate and
 * the plugin's native 48 kHz, so it can be accounted for in lv2:latency.
 *
 * Method: prime the resampler exactly as FretBoard does, feed a unit impulse
 * at a known input index, locate the response peak in the 48 kHz output and
 * compare it with the ideal position. The peak of a symmetric FIR is its
 * delay, and the result is converted from output samples to host frames.
 *
 * The library documents that inpsize()/2 - 1 samples of prefill make the first
 * output correspond to the first input; priming with inpsize() - 1 (what the
 * plugin does) is twice that. Both are measured here.
 */
#include <guitarmidi/common.hpp>
#include <zita-resampler/resampler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

constexpr int kNativeRate = 48000;
constexpr int kImpulseIndex = 64;
constexpr int kInputSamples = 4096;

struct Measurement
{
    int inputRate = 0;
    int inpsize = 0;
    int prime = 0;
    double inpdist = 0.0;
    double delayOutputSamples = 0.0;
    double delayHostFrames = 0.0;
};

Measurement measure(int inputRate, bool pluginPriming)
{
    Measurement result;
    result.inputRate = inputRate;

    Resampler resampler;
    CHECK(resampler.setup(inputRate, kNativeRate, 1, 16) == 0);
    result.inpsize = resampler.inpsize();
    result.prime = pluginPriming ? result.inpsize - 1 : result.inpsize / 2 - 1;

    // Prime with silence, exactly like FretBoard::initialize: a null input
    // pointer feeds zeroes, and out_count must be non-zero or zita returns
    // before reading anything.
    resampler.inp_count = result.prime;
    resampler.inp_data = nullptr;
    resampler.out_count = 1 << 20;
    resampler.out_data = nullptr;
    resampler.process();
    result.inpdist = resampler.inpdist();

    std::vector<float> input(kInputSamples, 0.0f);
    input[kImpulseIndex] = 1.0f;

    const int capacity = 200000;
    std::vector<float> output(capacity, 0.0f);
    int consumed = 0;
    int produced = 0;
    while (consumed < kInputSamples)
    {
        resampler.inp_data = input.data() + consumed;
        resampler.inp_count = kInputSamples - consumed;
        resampler.out_data = output.data() + produced;
        resampler.out_count = capacity - produced;
        const int before = resampler.inp_count;
        resampler.process();
        consumed += before - resampler.inp_count;
        produced = capacity - resampler.out_count;
        if (before == resampler.inp_count && produced == 0)
            break;
    }

    // Locate the peak with parabolic interpolation.
    int peak = 0;
    float peakValue = 0.0f;
    for (int i = 0; i < produced; ++i)
        if (std::fabs(output[i]) > peakValue)
        {
            peakValue = std::fabs(output[i]);
            peak = i;
        }

    CHECK(peakValue > 0.1f);
    double position = peak;
    if (peak > 0 && peak + 1 < produced)
    {
        const double a = std::fabs(output[peak - 1]);
        const double b = std::fabs(output[peak]);
        const double c = std::fabs(output[peak + 1]);
        const double denominator = a - 2.0 * b + c;
        if (std::fabs(denominator) > 1e-12)
            position += 0.5 * (a - c) / denominator;
    }

    // With correct prefill the output timeline is aligned with the signal, so
    // signal sample k belongs at output k * ratio.
    const double ratio = static_cast<double>(kNativeRate) / inputRate;
    const double ideal = kImpulseIndex * ratio;
    result.delayOutputSamples = position - ideal;
    // Convert to frames on the host timeline.
    result.delayHostFrames = result.delayOutputSamples / ratio;
    return result;
}

void report(const Measurement &m)
{
    std::printf("  in %6d Hz  inpsize %3d  prime %3d  inpdist %6.3f  "
                "delay %7.3f out samples = %7.3f host frames\n",
                m.inputRate, m.inpsize, m.prime, m.inpdist,
                m.delayOutputSamples, m.delayHostFrames);
}

} // namespace

int main()
{
    // lv2:latency is in host frames, not in the 48 kHz analysis domain.
    using GuitarMidi::nativeToHostFrames;
    CHECK(nativeToHostFrames(768, 48000) == 768);
    CHECK(nativeToHostFrames(768, 44100) == 706);
    CHECK(nativeToHostFrames(768, 96000) == 1536);
    CHECK(nativeToHostFrames(768, 192000) == 3072);

    const int rates[] = {44100, 88200, 96000, 192000};

    std::printf("plugin priming (inpsize - 1):\n");
    for (int rate : rates)
    {
        const Measurement m = measure(rate, true);
        report(m);
    }

    std::printf("documented priming (inpsize / 2 - 1):\n");
    for (int rate : rates)
    {
        const Measurement m = measure(rate, false);
        report(m);
        // With the documented prefill the first output corresponds to the
        // first input, so the impulse must arrive at its ideal position.
        CHECK(std::fabs(m.delayHostFrames) < 2.0);
    }

    if (failures == 0)
        std::printf("test_resampler_latency: OK\n");
    else
        std::printf("test_resampler_latency: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
