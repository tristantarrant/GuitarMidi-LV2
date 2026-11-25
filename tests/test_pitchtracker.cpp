/*
 * Monophonic pitch tracking for bends. Synthetic sines at known frequencies,
 * including a slow vibrato, must be reported within a few cents; silence and
 * noise must not be reported as a note.
 */
#include <guitarmidi/pitchtracker.hpp>

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

constexpr int kSampleRate = 48000;

double centsError(double expected, double measured)
{
    return 1200.0 * std::log2(measured / expected);
}

void testSteadyTones()
{
    const float frequencies[] = {82.41f, 110.0f, 196.0f, 220.0f, 329.63f, 440.0f, 659.26f};
    for (float expected : frequencies)
    {
        GuitarMidi::PitchTracker tracker;
        tracker.configure(kSampleRate);
        std::vector<float> block(256);
        float measured = 0.0f;
        float clarity = 0.0f;
        for (int n = 0; n < 40; ++n)
        {
            for (int i = 0; i < 256; ++i)
            {
                const double t = (n * 256 + i) / static_cast<double>(kSampleRate);
                block[i] = static_cast<float>(0.4 * std::sin(2.0 * M_PI * expected * t));
            }
            tracker.push(block.data(), 256);
            tracker.analyse(measured, clarity);
        }
        CHECK(tracker.analyse(measured, clarity));
        CHECK(std::fabs(centsError(expected, measured)) < 5.0);
        CHECK(clarity > 0.9f);
    }
}

void testVibrato()
{
    // 220 Hz with +/-50 cents at 5 Hz: every analysis must stay within the
    // bend window and follow the sign of the sweep.
    GuitarMidi::PitchTracker tracker;
    tracker.configure(kSampleRate);
    std::vector<float> block(256);
    double phase = 0.0;
    int above = 0;
    int below = 0;
    float measured = 0.0f;
    float clarity = 0.0f;
    for (int n = 0; n < 200; ++n)
    {
        const double t0 = n * 256.0 / kSampleRate;
        const double cents = 50.0 * std::sin(2.0 * M_PI * 5.0 * t0);
        const double frequency = 220.0 * std::pow(2.0, cents / 1200.0);
        for (int i = 0; i < 256; ++i)
        {
            const double t = (n * 256 + i) / static_cast<double>(kSampleRate);
            phase += 2.0 * M_PI * frequency / kSampleRate;
            block[i] = static_cast<float>(0.4 * std::sin(phase));
        }
        tracker.push(block.data(), 256);
        if (n > 20 && tracker.analyse(measured, clarity))
        {
            const double error = centsError(220.0, measured);
            CHECK(std::fabs(error) < 80.0);
            if (error > 10.0)
                ++above;
            if (error < -10.0)
                ++below;
        }
    }
    CHECK(above > 10);
    CHECK(below > 10);
}

void testSilenceAndNoise()
{
    GuitarMidi::PitchTracker tracker;
    tracker.configure(kSampleRate);
    std::vector<float> block(256, 0.0f);
    for (int n = 0; n < 20; ++n)
        tracker.push(block.data(), 256);
    float frequency = 0.0f;
    float clarity = 0.0f;
    CHECK(!tracker.analyse(frequency, clarity));

    // Deterministic pseudo-noise: no periodicity to lock onto.
    uint32_t state = 12345;
    for (int n = 0; n < 20; ++n)
    {
        for (int i = 0; i < 256; ++i)
        {
            state = state * 1664525u + 1013904223u;
            block[i] = static_cast<float>((state >> 9) * (1.0 / 4194304.0) - 1.0) * 0.3f;
        }
        tracker.push(block.data(), 256);
    }
    if (tracker.analyse(frequency, clarity))
        CHECK(clarity < 0.5f);
}

} // namespace

int main()
{
    testSteadyTones();
    testVibrato();
    testSilenceAndNoise();

    if (failures == 0)
        std::printf("test_pitchtracker: OK\n");
    else
        std::printf("test_pitchtracker: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
