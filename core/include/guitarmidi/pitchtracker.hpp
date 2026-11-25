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
#include <cstdint>
#include <vector>

namespace GuitarMidi
{

/**
 * Monophonic pitch detector (YIN) used for bends while a single note sounds.
 *
 * The model answers "which note", in 37 steps; it cannot say "that note is 40
 * cents sharp". This tracker answers exactly that, on the raw input, and is
 * only consulted when one note is sounding, so it costs nothing for chords.
 *
 * Direct difference function: for W samples and a lag range of ~70 Hz .. 1.2
 * kHz that is ~700k multiply-adds per analysis (once per hop), which is cheap
 * compared to the four XNNPACK threads running the model.
 *
 * YIN: de Cheveigné & Kawahara (2002). Implemented from the paper.
 */
class PitchTracker
{
public:
    static constexpr int kWindow = 1024;

    /** Sets the sample rate and the accepted fundamental range. */
    void configure(double sampleRate, float minHz = 70.0f, float maxHz = 1200.0f)
    {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_lagMin = std::max(2, static_cast<int>(m_sampleRate / maxHz));
        m_lagMax = static_cast<int>(m_sampleRate / minHz);
        m_history.assign(kWindow + m_lagMax, 0.0f);
        m_frame.assign(kWindow + m_lagMax, 0.0f);
        m_diff.assign(m_lagMax + 1, 0.0f);
        m_cmndf.assign(m_lagMax + 1, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill(m_history.begin(), m_history.end(), 0.0f);
        m_write = 0;
        m_fill = 0;
    }

    /** Appends audio; mono, any block size. */
    void push(const float *samples, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            m_history[m_write] = samples[i];
            if (++m_write == static_cast<int>(m_history.size()))
                m_write = 0;
        }
        m_fill = std::min<int>(m_fill + frames, static_cast<int>(m_history.size()));
    }

    /** True once there is enough history for an analysis. */
    bool ready() const
    {
        return m_fill >= static_cast<int>(m_history.size());
    }

    /**
     * Estimates the fundamental. Returns false when the signal is not
     * periodic enough to be a note; `clarity` (0..1) is 1 - cmndf at the
     * chosen lag.
     */
    bool analyse(float &frequency, float &clarity)
    {
        frequency = 0.0f;
        clarity = 0.0f;
        if (!ready())
            return false;

        // Copy the history into time order.
        const int size = static_cast<int>(m_history.size());
        for (int i = 0; i < size; ++i)
        {
            int index = m_write + i;
            if (index >= size)
                index -= size;
            m_frame[i] = m_history[index];
        }

        const int window = kWindow;
        const int lagMax = m_lagMax;
        for (int tau = 1; tau <= lagMax; ++tau)
        {
            const float *a = m_frame.data();
            const float *b = a + tau;
            float sum = 0.0f;
            for (int j = 0; j < window; ++j)
            {
                const float d = a[j] - b[j];
                sum += d * d;
            }
            m_diff[tau] = sum;
        }

        float running = 0.0f;
        m_cmndf[0] = 1.0f;
        for (int tau = 1; tau <= lagMax; ++tau)
        {
            running += m_diff[tau];
            m_cmndf[tau] = running > 0.0f ? m_diff[tau] * tau / running : 1.0f;
        }

        int best = -1;
        for (int tau = m_lagMin; tau <= lagMax; ++tau)
        {
            if (m_cmndf[tau] < 0.15f)
            {
                while (tau + 1 <= lagMax && m_cmndf[tau + 1] < m_cmndf[tau])
                    ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)
        {
            // No dip under the strict threshold: accept a clear global minimum.
            float minimum = 1e9f;
            int minimumLag = -1;
            for (int tau = m_lagMin; tau <= lagMax; ++tau)
                if (m_cmndf[tau] < minimum)
                {
                    minimum = m_cmndf[tau];
                    minimumLag = tau;
                }
            if (minimumLag < 0 || minimum > 0.30f)
                return false;
            best = minimumLag;
        }

        float tau = static_cast<float>(best);
        if (best > 1 && best < lagMax)
        {
            const float a = m_cmndf[best - 1];
            const float b = m_cmndf[best];
            const float c = m_cmndf[best + 1];
            const float denominator = a - 2.0f * b + c;
            if (std::fabs(denominator) > 1e-9f)
            {
                float shift = 0.5f * (a - c) / denominator;
                shift = std::max(-0.5f, std::min(0.5f, shift));
                tau += shift;
            }
        }
        if (tau <= 0.0f)
            return false;

        frequency = static_cast<float>(m_sampleRate) / tau;
        clarity = std::max(0.0f, std::min(1.0f, 1.0f - m_cmndf[best]));
        return true;
    }

    int lagMin() const { return m_lagMin; }
    int lagMax() const { return m_lagMax; }

private:
    double m_sampleRate = 48000.0;
    int m_lagMin = 40;
    int m_lagMax = 686;
    std::vector<float> m_history;
    std::vector<float> m_frame;
    std::vector<float> m_diff;
    std::vector<float> m_cmndf;
    int m_write = 0;
    int m_fill = 0;
};

} // namespace GuitarMidi
