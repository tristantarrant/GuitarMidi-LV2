/*
 * Unit tests for the note post-processing stage (scale, transpose, routing).
 * No model, no audio: this is the pure decision layer between the detector's
 * 37-note vector and the MIDI write.
 */
#include <guitarmidi/notepostprocess.hpp>

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

using GuitarMidi::NotePostSettings;
using GuitarMidi::postProcessNote;

bool kept(const NotePostSettings &settings, int pitch)
{
    return postProcessNote(settings, pitch).keep;
}

void testScaleOff()
{
    NotePostSettings settings;
    for (int pitch = 0; pitch < 128; ++pitch)
        CHECK(kept(settings, pitch));
    CHECK(postProcessNote(settings, 60).pitch == 60);
    CHECK(postProcessNote(settings, 60).channel == 1);
}

void testCMajor()
{
    NotePostSettings settings;
    settings.scale = 1; // major
    settings.root = 0;  // C
    CHECK(!kept(settings, 61));            // C#
    CHECK(kept(settings, 60));             // C
    CHECK(kept(settings, 62));             // D
    CHECK(kept(settings, 64));             // E
    CHECK(kept(settings, 65));             // F
    CHECK(kept(settings, 67));             // G
    CHECK(kept(settings, 69));             // A
    CHECK(kept(settings, 71));             // B
    CHECK(kept(settings, 72));             // C an octave up
    CHECK(kept(settings, 48));             // C an octave down
}

void testRootRotation()
{
    NotePostSettings settings;
    settings.scale = 1;
    settings.root = 2; // D major: D E F# G A B C#
    CHECK(kept(settings, 62));   // D
    CHECK(kept(settings, 61));   // C# (the major seventh of D)
    CHECK(!kept(settings, 60));  // C natural is not in D major
    CHECK(kept(settings, 66));   // F#
    CHECK(!kept(settings, 65));  // F
    CHECK(kept(settings, 73));   // C# above
}

void testPentatonic()
{
    NotePostSettings settings;
    settings.scale = 11; // minor pentatonic: 0 3 5 7 10
    CHECK(kept(settings, 60));   // C
    CHECK(!kept(settings, 62));  // D
    CHECK(kept(settings, 63));   // D#
    CHECK(kept(settings, 65));   // F
    CHECK(kept(settings, 67));   // G
    CHECK(!kept(settings, 69));  // A
    CHECK(kept(settings, 70));   // A#
}

void testTransposeBeforeScale()
{
    // A scale filter runs on the emitted pitch, so a note that is legal only
    // after the transpose must survive and vice versa.
    NotePostSettings settings;
    settings.scale = 1;    // C major
    settings.transpose = 1;
    CHECK(postProcessNote(settings, 59).keep);       // B -> C, legal
    CHECK(postProcessNote(settings, 59).pitch == 60);
    CHECK(!postProcessNote(settings, 60).keep);      // C -> C#, out of C major
    CHECK(postProcessNote(settings, 64).pitch == 65); // E -> F
}

void testOutOfRange()
{
    NotePostSettings settings;
    settings.transpose = -24;
    CHECK(!postProcessNote(settings, 10).keep); // -14
    settings.transpose = 24;
    CHECK(!postProcessNote(settings, 120).keep); // 144
    settings.transpose = 0;
    CHECK(postProcessNote(settings, 0).keep);
    CHECK(postProcessNote(settings, 127).keep);
}

void testSplitRouting()
{
    NotePostSettings settings;
    settings.splitNote = 60;
    settings.lowChannel = 1;
    settings.highChannel = 2;
    CHECK(postProcessNote(settings, 59).channel == 1);
    CHECK(postProcessNote(settings, 60).channel == 2);
    CHECK(postProcessNote(settings, 72).channel == 2);

    settings.splitNote = 0; // off: everything on the low channel
    CHECK(postProcessNote(settings, 90).channel == 1);

    settings.splitNote = 60;
    settings.transpose = 12; // routing uses the emitted pitch
    CHECK(postProcessNote(settings, 55).channel == 2);
}

void testChannelClamp()
{
    NotePostSettings settings;
    settings.lowChannel = 0;
    CHECK(postProcessNote(settings, 60).channel == 1);
    settings.lowChannel = 99;
    CHECK(postProcessNote(settings, 60).channel == 16);
}

void testTables()
{
    CHECK(GuitarMidi::kScaleCount == 18);
    CHECK(GuitarMidi::kScaleMasks[0] == 0x0FFF);
    CHECK(GuitarMidi::kScaleMasks[13] == 0x0FFF);
    for (int i = 1; i < GuitarMidi::kScaleCount; ++i)
    {
        CHECK(GuitarMidi::kScaleMasks[i] != 0);
        CHECK(GuitarMidi::kScaleNames[i] != nullptr);
        CHECK(static_cast<unsigned>(GuitarMidi::kScaleMasks[i]) < 0x1000u);
    }
    // Every scale is a subset of the chromatic set and contains its root.
    for (int i = 1; i < GuitarMidi::kScaleCount; ++i)
        CHECK(GuitarMidi::kScaleMasks[i] & 1);
}

} // namespace

int main()
{
    testScaleOff();
    testCMajor();
    testRootRotation();
    testPentatonic();
    testTransposeBeforeScale();
    testOutOfRange();
    testSplitRouting();
    testChannelClamp();
    testTables();

    if (failures == 0)
        std::printf("test_postprocess: OK\n");
    else
        std::printf("test_postprocess: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
