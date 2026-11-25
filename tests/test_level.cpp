/*
 * The level conversion behind the onset/offset energy thresholds. The
 * thresholds used to compare a sum of 256 rectified samples against a dB
 * amplitude, which made the control's scale meaningless and its range land at
 * a very low level.
 */
#include <guitarmidi/level.hpp>

#include <cstdio>

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
#define CHECK_NEAR(a, b) check(std::fabs((a) - (b)) < 0.01f, #a " ~= " #b, __LINE__)

constexpr int kFrames = 256;

} // namespace

int main()
{
    using GuitarMidi::bandLevelDb;

    // Mean rectified amplitude 1.0 is 0 dBFS.
    CHECK_NEAR(bandLevelDb(static_cast<float>(kFrames), 1.0f, kFrames), 0.0f);
    // A tenth of that is -20 dB.
    CHECK_NEAR(bandLevelDb(0.1f * kFrames, 1.0f, kFrames), -20.0f);
    // A thousandth is -60 dB.
    CHECK_NEAR(bandLevelDb(0.001f * kFrames, 1.0f, kFrames), -60.0f);

    // The gain cancels: turning up the input gain must not move the level the
    // threshold is compared against.
    CHECK_NEAR(bandLevelDb(0.001f * kFrames * 10.0f, 10.0f, kFrames), -60.0f);
    CHECK_NEAR(bandLevelDb(0.001f * kFrames * 0.1f, 0.1f, kFrames), -60.0f);

    // Frame length cancels too.
    CHECK_NEAR(bandLevelDb(0.001f * 256.0f, 1.0f, 256), bandLevelDb(0.001f * 512.0f, 1.0f, 512));

    // Silence is very quiet, and never a division by zero.
    CHECK(bandLevelDb(0.0f, 1.0f, kFrames) < -150.0f);
    CHECK(bandLevelDb(1.0f, 0.0f, 0) < -150.0f);

    // The old default (-18 dB in the old scale) was implicitly -66 dBFS of the
    // input: 10^(-18/20)/256. Changing the law must not change sensitivity.
    const float oldEquivalent = 20.0f * std::log10(std::pow(10.0f, -18.0f / 20.0f) / 256.0f);
    CHECK_NEAR(oldEquivalent, -66.16f);
    CHECK_NEAR(bandLevelDb(std::pow(10.0f, -18.0f / 20.0f) / 256.0f * kFrames, 1.0f, kFrames), oldEquivalent);

    if (failures == 0)
        std::printf("test_level: OK\n");
    else
        std::printf("test_level: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
