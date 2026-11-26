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
#include <guitarmidi/fretboard.hpp>
#include <algorithm>
#include <omp.h>

using namespace GuitarMidi;

FretBoardAPI* creatFretBoard(GuitarMidiOutput* midioutput){
    return new FretBoard(midioutput);
}
FretBoard::FretBoard(GuitarMidiOutput* midioutput) : m_noteinferencer(midioutput)
{


}

void FretBoard::setAudioInput(const float *input)
{
    m_input_ptr = input;
    if (m_samplerate != NATIVE_SAMPLERATE)
    {
        m_input_buffer = const_cast<float *>(input);
        m_filterbank.setInput(m_resampled_buffer);
    }
    else
    {
        m_filterbank.setInput(input);
    }
}


bool FretBoard::initialize(const std::string &bundle_path, int samplerate, int buffer_size)
{
    g_logger.info("Initializing FretBoard with samplerate %d and buffer size %d ", samplerate, buffer_size);
    // check if the samplerate of the host is different from the native samplerate of the plugin, if so, setup the resampler
    if (samplerate != NATIVE_SAMPLERATE)
    {
        g_logger.info("Host samplerate  %d is different from plugin samplerate %d, resampling will be performed", samplerate, NATIVE_SAMPLERATE);
        m_resample_buffer_size = buffer_size;
        m_resampled_buffer = new float[m_resample_buffer_size];
        if (m_resampler.setup(samplerate, NATIVE_SAMPLERATE, 1, 16))
        {
            g_logger.error("Failed to setup resampler");
            return false;
        }
        // Prefill with inpsize()/2 - 1 zeroes, which is what makes the first
        // output sample correspond to the first input sample (see the
        // zita-resampler manual, "inpdist"). Priming with inpsize() - 1, as
        // this used to, adds half a filter of delay: about 0.34 ms at every
        // non-48 kHz rate, measured by tests/test_resampler_latency.cpp.
        m_resampler.inp_count = m_resampler.inpsize() / 2 - 1;
        m_resampler.inp_data = 0;
        m_resampler.out_count = 999999;
        m_resampler.out_data = 0;
        m_resampler.process();

        // Set the initial conditions, input buffer empty.
        m_resampler.out_data = m_resampled_buffer;
        m_resampler.out_count = buffer_size;
    }

    // setup the filterbank with the filter representations from the fretboard representation
    m_samplerate = samplerate;
    m_fretboard_rep = FretBoardRepresentation();
    m_filters = m_fretboard_rep.get_filterrepresentations();
    m_filterbank.setup(m_filters, NATIVE_SAMPLERATE, buffer_size, m_tuning);
    m_appliedTuning = m_tuning;
    m_filterbank.captureFrame();
    // The inference input is the contiguous frame, not the rolling buffer.
    m_noteinferencer.setAudioInputBuffer(m_filterbank.get_frame_buffer());
    return m_noteinferencer.initialize(bundle_path);
}

void FretBoard::finalize()
{
    m_noteinferencer.finalize();

    m_filterbank.reset();
    m_hopFill = 0;
}

void FretBoard::process(int nsamples, const GuitarMidi::MidiInputEvent *events, int eventCount)
{
    // prepare the output stream
    m_noteinferencer.preprocess();
    m_blockSize = nsamples;

    // Incoming MIDI is merged with the tracker and passed through.
    for (int i = 0; i < eventCount; ++i)
        m_noteinferencer.handleInputEvent(events[i]);

    const bool enabled = (m_enabled == nullptr) || (*m_enabled > 0.5f);
    if (!enabled)
    {
        if (!m_bypassActive)
        {
            // Release what the tracker is holding and drop its state; while
            // bypassed no audio is analysed, so the next frame would otherwise
            // be stitched onto pre-bypass history.
            m_noteinferencer.resetState();
            m_filterbank.reset();
            m_hopFill = 0;
            m_bypassActive = true;
        }
        m_noteinferencer.postprocess();
        return;
    }
    if (m_bypassActive)
    {
        m_filterbank.reset();
        m_noteinferencer.resetState();
        m_hopFill = 0;
        m_bypassActive = false;
    }

    // A tuning change means new biquad coefficients. Do it here (not on the
    // host thread), once, and start from a clean filter state.
    if (m_tuning != m_appliedTuning)
    {
        m_filterbank.setup(m_filters, NATIVE_SAMPLERATE, 0, m_tuning);
        m_appliedTuning = m_tuning;
        m_hopFill = 0;
    }

    // Drive the filter bank in chunks that end exactly on an analysis hop.
    // That keeps the rolling filter buffer aligned (the model is never handed
    // a rotated window) and gives every hop a real position in the block.
    int done = 0;
    while (done < nsamples)
    {
        if (m_samplerate != NATIVE_SAMPLERATE)
        {
            int n = nsamples - done;
            const int budget = m_resampler.inpsize() - 1;
            if (budget > 0 && n > budget)
                n = budget;
            process_resampled(n, done);
            done += n;
        }
        else
        {
            const int n = std::min(nsamples - done, BUFFER_SIZE - m_hopFill);
            process_direct(n, done);
            done += n;
        }
    }

    m_noteinferencer.postprocess();
}

void FretBoard::process_resampled(int nsamples,int pos)
{
    m_resampler.inp_data = m_input_buffer + pos;
    m_resampler.inp_count = nsamples;
    while (m_resampler.inp_count > 0)
    {
        m_resampler.out_data = m_resampled_buffer;
        m_resampler.out_count = m_resample_buffer_size;
        const int before_in = m_resampler.inp_count;
        m_resampler.process();
        const int produced = m_resample_buffer_size - m_resampler.out_count;
        const int consumed = before_in - m_resampler.inp_count;
        if (produced > 0)
        {
            // The hop completed somewhere inside this input chunk; place its
            // events there instead of at the block boundary.
            const int eventPos = pos + std::min(nsamples - 1, consumed);
            consumeResampled(m_resampled_buffer, produced, eventPos);
        }
        if (consumed == 0 && produced == 0)
            break; // the resampler made no progress; do not spin
    }
}

void FretBoard::consumeResampled(const float* data,int count,int eventPos)
{
    int off = 0;
    while (off < count)
    {
        const int n = std::min(count - off, BUFFER_SIZE - m_hopFill);
        m_noteinferencer.pushAudio(data + off, n);
        m_filterbank.process(n, off);
        off += n;
        m_hopFill += n;
        if (m_hopFill == BUFFER_SIZE)
        {
            m_hopFill = 0;
            m_filterbank.captureFrame();
            m_noteinferencer.processHop(std::min<int64_t>(m_blockSize - 1, eventPos), m_blockSize);
        }
    }
}

void FretBoard::process_direct(int nsamples,int pos)
{
    if (m_input_ptr)
        m_noteinferencer.pushAudio(m_input_ptr + pos, nsamples);
    m_filterbank.process(nsamples,pos);
    m_hopFill += nsamples;
    if (m_hopFill >= BUFFER_SIZE)
    {
        m_hopFill = 0;
        m_filterbank.captureFrame();
        // pos + nsamples is the first sample after the analysed window; clamp
        // to the block because an LV2 event time must be inside it.
        m_noteinferencer.processHop(std::min<int64_t>(m_blockSize - 1, pos + nsamples), m_blockSize);
    }
}

void FretBoard::getFilterWaveforms(float *waveforms, uint32_t samples_per_filter) const
{
    if (samples_per_filter == 0)
        return;

    const AudioBuffer2D filter_buffer = m_filterbank.get_waveform_buffer();
    for (int filter = 0; filter < NUM_FILTERS; ++filter)
    {
        const float *samples = filter_buffer.audio_buffer_2D + filter * filter_buffer.window_size;
        for (uint32_t point = 0; point < samples_per_filter; ++point)
        {
            const uint32_t sample = point * filter_buffer.window_size / samples_per_filter;
            waveforms[filter * samples_per_filter + point] = samples[sample];
        }
    }
}
