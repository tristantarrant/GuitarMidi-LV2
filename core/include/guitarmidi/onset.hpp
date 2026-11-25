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
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace GuitarMidi
{

/**
 * Where inside an analysis hop did the onset happen?
 *
 * The onset confidence is smoothed with a one-pole filter and compared against
 * a threshold, so the crossing happened somewhere between the previous and the
 * current hop. Treating the ramp as linear between the two gives the fraction
 * `t` of the hop at which the threshold was crossed, and therefore how far
 * before the hop end the note started.
 *
 * `hopEndPos` is the frame position of the hop end in the block that emits the
 * event (the result scheduler makes that a constant number of hops after the
 * analysed one). Returns a frame position clamped into the block.
 */
inline int64_t onsetFramePosition(int64_t hopEndPos, float previous, float current,
                                  float threshold, int hopLength, int64_t blockSize)
{
    if (hopLength <= 0)
        return std::max<int64_t>(0, hopEndPos);
    if (blockSize <= 0)
        blockSize = 1;

    float t = 1.0f;
    const float rise = current - previous;
    if (rise > 1e-6f)
        t = (threshold - previous) / rise;
    t = std::max(0.0f, std::min(1.0f, t));

    // t = 1 means the threshold was crossed at the hop end, t = 0 at its start.
    const int64_t offset = static_cast<int64_t>(std::lround((1.0f - t) * hopLength));
    int64_t position = hopEndPos - offset;
    if (position < 0)
        position = 0;
    if (position >= blockSize)
        position = blockSize - 1;
    return position;
}

} // namespace GuitarMidi
