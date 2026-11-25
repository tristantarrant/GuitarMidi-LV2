/*
 * Sub-hop onset placement: the smoothed confidence crosses the threshold
 * somewhere inside the hop, and the event is placed there instead of always at
 * the hop end.
 */
#include <guitarmidi/onset.hpp>

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

constexpr int kHop = 256;

} // namespace

int main()
{
    using GuitarMidi::onsetFramePosition;

    // Crossing halfway through the ramp: the onset is half a hop before the end.
    CHECK(onsetFramePosition(kHop, 0.0f, 1.6f, 0.8f, kHop, 512) == kHop - kHop / 2);

    // Crossed almost at the end: a few samples of shift.
    CHECK(onsetFramePosition(kHop, 0.0f, 0.81f, 0.8f, kHop, 512) == 253);

    // Crossed early in the ramp: most of the hop is subtracted.
    CHECK(onsetFramePosition(kHop, 0.0f, 8.0f, 0.8f, kHop, 512) == 26);

    // No rise (already above threshold): place at the hop end.
    CHECK(onsetFramePosition(kHop, 0.9f, 0.9f, 0.8f, kHop, 512) == kHop);

    // Small block: clamped into the block instead of going negative.
    CHECK(onsetFramePosition(64, 0.0f, 1.6f, 0.8f, kHop, 64) == 0);

    // Block end position is clamped to the last frame.
    CHECK(onsetFramePosition(512, 0.0f, 1.6f, 0.8f, kHop, 512) == 512 - 128);

    // Degenerate inputs do not crash.
    CHECK(onsetFramePosition(0, 0.0f, 1.0f, 0.8f, 0, 0) == 0);
    CHECK(onsetFramePosition(10, 0.0f, 1.0f, 0.8f, kHop, 0) == 0);

    if (failures == 0)
        std::printf("test_onset: OK\n");
    else
        std::printf("test_onset: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
