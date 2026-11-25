#pragma once
/* GuitarMidi-LV2 Library
 * Copyright (C) 2022 Gerald Mwangi
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */
#include <cstdint>

namespace GuitarMidi
{

/**
 * Note-level post-processing between the model output and the MIDI write.
 *
 * The model decides *which notes are sounding*; this stage decides what to do
 * with them: keep them in a scale, transpose them, and route them to a MIDI
 * channel. It is deliberately independent of the detector so it can be tested
 * without the model and without audio.
 *
 * All pitch decisions use the final pitch (raw + transpose), matching what the
 * downstream instrument will see.
 */
struct NotePostSettings
{
    int scale = 0;       // 0 = off, 1..17 = one of kScaleNames
    int root = 0;        // 0 = C .. 11 = B
    int transpose = 0;   // semitones, -24..24 (also covers octave shifts)
    int splitNote = 0;   // 0 = off, otherwise the first pitch of the high side
    int lowChannel = 1;  // 1..16
    int highChannel = 1; // 1..16
    int mpe = 0;         // 1 = give every sounding note its own member channel
};

/**
 * MPE-style channel pool: every sounding note gets its own member channel
 * (2..16) so an MPE instrument can apply per-note bend/aftertouch. When all
 * fifteen are busy the least recently allocated one is reused, which matches
 * how an MPE source is expected to behave with a voice-steal.
 *
 * The model currently exposes one 37-note output, so this is the routing half
 * of per-note expression; per-string articulation needs the model exported
 * with its 78 string/fret tensor as an additional output (see
 * docs/improvement-plan.md).
 */
class MpeChannelPool
{
public:
    static constexpr int kFirstMember = 2;
    static constexpr int kLastMember = 16;

    int allocate()
    {
        for (int channel = kFirstMember; channel <= kLastMember; ++channel)
            if (!m_used[channel])
                return take(channel);
        // All busy: steal the oldest allocation.
        int oldest = kFirstMember;
        for (int channel = kFirstMember; channel <= kLastMember; ++channel)
            if (m_age[channel] < m_age[oldest])
                oldest = channel;
        return take(oldest);
    }

    void release(int channel)
    {
        if (channel >= kFirstMember && channel <= kLastMember)
            m_used[channel] = false;
    }

    void reset()
    {
        for (bool &used : m_used)
            used = false;
        for (uint32_t &age : m_age)
            age = 0;
        m_counter = 0;
    }

private:
    int take(int channel)
    {
        m_used[channel] = true;
        m_age[channel] = ++m_counter;
        return channel;
    }

    bool m_used[kLastMember + 1] = {};
    uint32_t m_age[kLastMember + 1] = {};
    uint32_t m_counter = 0;
};

struct NotePostResult
{
    bool keep = true;
    int pitch = 0;     // MIDI pitch to emit
    int channel = 1;   // 1..16
};

/** Number of entries in kScaleNames (0 = off). */
inline constexpr int kScaleCount = 18;

/** Display names; index matches NotePostSettings::scale. */
inline constexpr const char *kScaleNames[kScaleCount] = {
    "Off",
    "Major",
    "Natural Minor",
    "Harmonic Minor",
    "Melodic Minor",
    "Dorian",
    "Phrygian",
    "Lydian",
    "Mixolydian",
    "Locrian",
    "Major Pentatonic",
    "Minor Pentatonic",
    "Blues",
    "Chromatic",
    "Whole Tone",
    "Diminished",
    "Hungarian Minor",
    "Phrygian Dominant",
};

/**
 * Scale degrees as semitones above the root, -1 terminated. The bit masks are
 * built from these tables at compile time so a transcription slip cannot make
 * a scale silently wrong.
 */
inline constexpr int8_t kScaleIntervals[kScaleCount][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7},  // Off (unused, full mask)
    {0, 2, 4, 5, 7, 9, 11, -1}, // Major
    {0, 2, 3, 5, 7, 8, 10, -1}, // Natural minor
    {0, 2, 3, 5, 7, 8, 11, -1}, // Harmonic minor
    {0, 2, 3, 5, 7, 9, 11, -1}, // Melodic minor
    {0, 2, 3, 5, 7, 9, 10, -1}, // Dorian
    {0, 1, 3, 5, 7, 8, 10, -1}, // Phrygian
    {0, 2, 4, 6, 7, 9, 11, -1}, // Lydian
    {0, 2, 4, 5, 7, 9, 10, -1}, // Mixolydian
    {0, 1, 3, 5, 6, 8, 10, -1}, // Locrian
    {0, 2, 4, 7, 9, -1},        // Major pentatonic
    {0, 3, 5, 7, 10, -1},       // Minor pentatonic
    {0, 3, 5, 6, 7, 10, -1},    // Blues
    {0, 1, 2, 3, 4, 5, 6, 7},   // Chromatic (full mask below)
    {0, 2, 4, 6, 8, 10, -1},    // Whole tone
    {0, 2, 3, 5, 6, 8, 9, 11},  // Diminished (whole-half)
    {0, 2, 3, 6, 7, 8, 11, -1}, // Hungarian minor
    {0, 1, 4, 5, 7, 8, 10, -1}, // Phrygian dominant
};

inline constexpr uint16_t buildScaleMask(const int8_t (&intervals)[8])
{
    uint16_t mask = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (intervals[i] < 0)
            break;
        mask |= static_cast<uint16_t>(1u << intervals[i]);
    }
    return mask;
}

inline constexpr uint16_t kScaleMasks[kScaleCount] = {
    0x0FFF, // Off (unused, full mask)
    buildScaleMask(kScaleIntervals[1]),
    buildScaleMask(kScaleIntervals[2]),
    buildScaleMask(kScaleIntervals[3]),
    buildScaleMask(kScaleIntervals[4]),
    buildScaleMask(kScaleIntervals[5]),
    buildScaleMask(kScaleIntervals[6]),
    buildScaleMask(kScaleIntervals[7]),
    buildScaleMask(kScaleIntervals[8]),
    buildScaleMask(kScaleIntervals[9]),
    buildScaleMask(kScaleIntervals[10]),
    buildScaleMask(kScaleIntervals[11]),
    buildScaleMask(kScaleIntervals[12]),
    0x0FFF, // Chromatic: the table above only holds eight degrees
    buildScaleMask(kScaleIntervals[14]),
    buildScaleMask(kScaleIntervals[15]),
    buildScaleMask(kScaleIntervals[16]),
    buildScaleMask(kScaleIntervals[17]),
};

static_assert(kScaleMasks[1] == 0x0AB5, "major scale mask");
static_assert(kScaleMasks[2] == 0x05AD, "natural minor mask");
static_assert(kScaleMasks[12] == 0x04E9, "blues scale mask");

/** True when `pitch` belongs to the configured scale (scale 0 always true). */
inline bool pitchInScale(const NotePostSettings &settings, int pitch)
{
    if (settings.scale <= 0 || settings.scale >= kScaleCount)
        return true;
    const int rel = ((pitch - settings.root) % 12 + 12) % 12;
    return (kScaleMasks[settings.scale] >> rel) & 1;
}

/**
 * Applies the pitch transforms and channel routing to one detected note.
 * A note that leaves the MIDI pitch range or the scale is dropped.
 */
inline NotePostResult postProcessNote(const NotePostSettings &settings, int pitch)
{
    NotePostResult result;
    result.pitch = pitch + settings.transpose;

    if (result.pitch < 0 || result.pitch > 127)
    {
        result.keep = false;
        return result;
    }
    if (!pitchInScale(settings, result.pitch))
    {
        result.keep = false;
        return result;
    }

    if (settings.splitNote > 0)
        result.channel = (result.pitch < settings.splitNote) ? settings.lowChannel
                                                             : settings.highChannel;
    else
        result.channel = settings.lowChannel;

    // Defensive: a host can write anything into a control port.
    if (result.channel < 1)
        result.channel = 1;
    if (result.channel > 16)
        result.channel = 16;

    return result;
}

} // namespace GuitarMidi
