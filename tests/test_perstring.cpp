/*
 * The "one note per string" constraint over the per-string head.
 */
#include <guitarmidi/perstring.hpp>

#include <cstdio>
#include <cstring>

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

using GuitarMidi::resolveOneNotePerString;

// NUM_FRETS / NUM_NOTES / NUM_STRINGS / NOTE_OFFSET are macros.
// Index of a note in the 37-note output.
int noteIndex(int pitch) { return pitch - NOTE_OFFSET; }

void setStringProbability(float *probabilities, int string, int fret, float value)
{
    probabilities[string * NUM_FRETS + fret] = value;
}

} // namespace

int main()
{
    float probabilities[NUM_STRINGS * NUM_FRETS];
    bool candidates[NUM_NOTES];

    // Two notes on the same string (E2 and F2 both on the low E): only the
    // stronger one can sound.
    std::memset(probabilities, 0, sizeof(probabilities));
    setStringProbability(probabilities, 0, 0, 0.2f); // E2
    setStringProbability(probabilities, 0, 1, 0.9f); // F2
    std::memset(candidates, 0, sizeof(candidates));
    candidates[noteIndex(40)] = true;
    candidates[noteIndex(41)] = true;
    CHECK(resolveOneNotePerString(candidates, probabilities) == 1);
    CHECK(!candidates[noteIndex(40)]);
    CHECK(candidates[noteIndex(41)]);

    // Notes on different strings both survive.
    std::memset(probabilities, 0, sizeof(probabilities));
    setStringProbability(probabilities, 0, 0, 0.8f); // E2
    setStringProbability(probabilities, 1, 0, 0.7f); // A2
    std::memset(candidates, 0, sizeof(candidates));
    candidates[noteIndex(40)] = true;
    candidates[noteIndex(45)] = true;
    CHECK(resolveOneNotePerString(candidates, probabilities) == 0);
    CHECK(candidates[noteIndex(40)]);
    CHECK(candidates[noteIndex(45)]);

    // A pitch that loses on one string but wins on another is kept.
    // E3 (52) is fret 12 of the low E and fret 8 of the D string.
    std::memset(probabilities, 0, sizeof(probabilities));
    setStringProbability(probabilities, 0, 12, 0.4f); // E3 on low E
    setStringProbability(probabilities, 2, 8, 0.9f);  // E3 on D
    setStringProbability(probabilities, 0, 0, 0.95f); // E2 (also low E)
    std::memset(candidates, 0, sizeof(candidates));
    candidates[noteIndex(52)] = true;
    candidates[noteIndex(40)] = true;
    CHECK(resolveOneNotePerString(candidates, probabilities) == 0);

    // A single candidate is never removed.
    std::memset(probabilities, 0, sizeof(probabilities));
    std::memset(candidates, 0, sizeof(candidates));
    candidates[noteIndex(40)] = true;
    CHECK(resolveOneNotePerString(candidates, probabilities) == 0);
    CHECK(candidates[noteIndex(40)]);

    // Out-of-range pitches (below/above the guitar) are ignored.
    std::memset(probabilities, 0, sizeof(probabilities));
    std::memset(candidates, 0, sizeof(candidates));
    candidates[0] = true;              // pitch 40
    candidates[NUM_NOTES - 1] = true;  // pitch 76, high E fret 12
    resolveOneNotePerString(candidates, probabilities);

    if (failures == 0)
        std::printf("test_perstring: OK\n");
    else
        std::printf("test_perstring: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
