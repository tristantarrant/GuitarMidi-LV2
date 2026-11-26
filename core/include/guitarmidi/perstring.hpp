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
#include <guitarmidi/common.hpp>

namespace GuitarMidi
{

/** Open-string MIDI pitches, low E to high E. */
inline constexpr int kOpenStrings[NUM_STRINGS] = {40, 45, 50, 55, 59, 64};

/**
 * "One note per string" constraint, using the model's per-string head.
 *
 * A string can sound one fret at a time. If two candidate notes are both
 * explained by the same string, only the one the model prefers on that string
 * can be real; the other is a harmonic ghost. A note that loses on one string
 * but wins on another (the same pitch exists in several positions) survives.
 *
 * `probabilities` is the 6 x 13 tensor, row-major: string * NUM_FRETS + fret.
 * `candidates` is modified in place; returns how many candidates were cleared.
 *
 * This is deliberately a pure function: the heuristic is easy to test and the
 * decision to use it is a control, because it changes detection and needs
 * evaluation on real playing.
 */
inline int resolveOneNotePerString(bool candidates[NUM_NOTES],
                                   const float stringProbabilities[NUM_STRINGS * NUM_FRETS])
{
    int bestNote[NUM_STRINGS];
    float bestProbability[NUM_STRINGS];
    for (int string = 0; string < NUM_STRINGS; ++string)
    {
        bestNote[string] = -1;
        bestProbability[string] = -1.0f;
    }

    for (int index = 0; index < NUM_NOTES; ++index)
    {
        if (!candidates[index])
            continue;
        const int pitch = index + NOTE_OFFSET;
        for (int string = 0; string < NUM_STRINGS; ++string)
        {
            const int fret = pitch - kOpenStrings[string];
            if (fret < 0 || fret >= NUM_FRETS)
                continue;
            const float probability =
                stringProbabilities[string * NUM_FRETS + fret];
            if (probability > bestProbability[string])
            {
                bestProbability[string] = probability;
                bestNote[string] = index;
            }
        }
    }

    bool allowed[NUM_NOTES] = {};
    for (int string = 0; string < NUM_STRINGS; ++string)
        if (bestNote[string] >= 0)
            allowed[bestNote[string]] = true;

    int cleared = 0;
    for (int index = 0; index < NUM_NOTES; ++index)
    {
        if (candidates[index] && !allowed[index])
        {
            candidates[index] = false;
            ++cleared;
        }
    }
    return cleared;
}

} // namespace GuitarMidi
