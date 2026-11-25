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
#include <cmath>

namespace GuitarMidi
{

/**
 * Converts a band's energy over one analysis frame to a level in dBFS.
 *
 * `frameEnergy` is the sum of the rectified filter output over the frame, so
 * dividing by the frame length gives the mean rectified amplitude. The filter
 * bank applies the input gain before filtering, so dividing by the gain again
 * gives the level of the *input*: the thresholds then mean "the guitar has to
 * be at least this loud", independent of the gain control and of the block
 * size.
 *
 * 0 dBFS is a mean rectified amplitude of 1.0 (a full-scale square wave).
 */
inline float bandLevelDb(float frameEnergy, float gain, int frameLength)
{
    if (frameLength <= 0)
        return -200.0f;
    const float meanRectified = frameEnergy / static_cast<float>(frameLength);
    const float preGain = meanRectified / (gain > 1e-6f ? gain : 1e-6f);
    return 20.0f * std::log10(preGain + 1e-9f);
}

} // namespace GuitarMidi
