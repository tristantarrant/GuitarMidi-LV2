#include <guitarmidi/noteinferencer.hpp>
#include <cmath>
#include <cstring>
#include <chrono>
#include <thread>

namespace GuitarMidi
{

    NoteInferencer::NoteInferencer(GuitarMidiOutput *midioutput) : m_midioutput(midioutput)
    {
        for (auto &note_on : m_note_on)
            note_on.store(false, std::memory_order_relaxed);
    }

    bool NoteInferencer::initialize(const std::string &bundle_path)
    {
        if (m_midioutput)
            m_midioutput->initializeSequence();
        return m_model.initialize(bundle_path);
    }

    void NoteInferencer::finalize()
    {
        m_model.finalize();
        if (m_midioutput)
            m_midioutput->finalizeSequence();
    }

    void NoteInferencer::flushHeldNotes(int64_t pos)
    {
        for (int i = 0; i < NUM_NOTES; ++i)
        {
            if (m_note_on[i].load(std::memory_order_relaxed))
            {
                const int channel = m_emittedChannel[i] > 0 ? m_emittedChannel[i] : 1;
                uint8_t midinote[3] = {static_cast<uint8_t>(0x90 | (channel - 1)),
                                       static_cast<uint8_t>(m_emittedPitch[i]), 0x00};
                if (m_midioutput)
                    m_midioutput->sendMidiMessage(midinote, pos);
                m_note_on[i].store(false, std::memory_order_relaxed);
            }
            smoothed_onsetoutput[i] = 0.0f;
            smoothed_offsetoutput[i] = 0.0f;
            smoothed_noteenergies[i] = 0.0f;
            smoothed_offsetnoteenergies[i] = 0.0f;
            m_momentary_confidences[i] = 0.0f;
            m_momentary_noteenergies[i] = 0.0f;
        }
    }

    void NoteInferencer::drainResults(int64_t pos)
    {
        float probabilities[NUM_NOTES];
        uint64_t stamp = 0;
        while (m_model.get_model_output(probabilities, stamp))
        {
            if (m_haveLastResultStamp && stamp != m_lastResultStamp + 1)
            {
                // The inference stream skipped a hop: either a frame was
                // refused on the way in or a result on the way out. The
                // smoothing state and every held note describe audio that is
                // no longer contiguous with what follows, so start clean.
                ++m_streamGaps;
                flushHeldNotes(pos);
                m_pendingCount = 0;
                m_pendingHead = 0;
            }
            m_lastResultStamp = stamp;
            m_haveLastResultStamp = true;

            if (m_pendingCount == kMaxPending)
            {
                // Should not happen while the queues are healthy; keep the
                // newest results and account for the one we dropped.
                m_pendingHead = (m_pendingHead + 1) % kMaxPending;
                --m_pendingCount;
                ++m_staleDropped;
            }
            const int slot = (m_pendingHead + m_pendingCount) % kMaxPending;
            m_pending[slot].stamp = stamp;
            std::memcpy(m_pending[slot].probabilities, probabilities,
                        sizeof(float) * NUM_NOTES);
            ++m_pendingCount;
        }
    }

    void NoteInferencer::emitDueResults(int64_t pos)
    {
        while (m_pendingCount > 0)
        {
            PendingResult &pending = m_pending[m_pendingHead];
            const uint64_t age = m_hopsProcessed - pending.stamp;
            if (age < PIPELINE_LATENCY_HOPS)
                break;

            if (age > MAX_RESULT_AGE_HOPS)
            {
                // So far behind that the result describes audio the player has
                // left behind; emitting it would place a note in the wrong
                // place, which is worse than a gap.
                ++m_staleDropped;
                flushHeldNotes(pos);
            }
            else
            {
                if (age > PIPELINE_LATENCY_HOPS)
                    ++m_late;
                applyResult(pending.probabilities, pos);
            }

            m_pendingHead = (m_pendingHead + 1) % kMaxPending;
            --m_pendingCount;
        }
    }

    void NoteInferencer::applyResult(const float *output_data, int64_t pos)
    {
        if (!m_gain_db || !m_expressivity_db || !m_smoothing || !m_smoothing_offset ||
            !m_onset_threshold || !m_offset_threshold ||
            !m_onset_energy_threshold || !m_offset_energy_threshold)
            return;

        int num_active_notes = 0;
        for (int h = 0; h < NUM_NOTES; h++)
            num_active_notes += m_note_on[h].load(std::memory_order_relaxed);

        const float gain = powf(10.0f, *m_gain_db * 0.05f);
        const float expressivity = powf(10.0f, *m_expressivity_db * 0.05f);
        const float en_threshold = pow(10, *m_onset_energy_threshold / 20) * gain;
        const float en_threshold_offset = pow(10, *m_offset_energy_threshold / 20) * gain;

        bool triggeron[NUM_NOTES] = {false};
        bool triggeroff[NUM_NOTES] = {false};
        for (int i = 0; i < NUM_NOTES; i++)
        {
            // check if all harmonics of the note are active before sending note on message
            float note_energy = 0;
            int h = 1; // for (int h = 1; h <= NUM_HARMONICS; h++)
            {
                int harmonic_index = i * NUM_HARMONICS + h - 1;
                if (harmonic_index < NUM_HARMONICS * NUM_NOTES)
                {
                    float harmonicenergy = 0;
                    for (int w = 0; w < BUFFER_SIZE; w++)
                    {
                        harmonicenergy += m_audiobuffer.audio_buffer_2D[harmonic_index * BUFFER_SIZE + w];
                    }
                    note_energy += harmonicenergy;
                }
            }

            m_momentary_confidences[i] = output_data[i];
            m_momentary_noteenergies[i] = note_energy;

            smoothed_onsetoutput[i] = *m_smoothing * smoothed_onsetoutput[i] + (1 - *m_smoothing) * output_data[i]; // simple low pass filter to smooth the output and reduce jitter
            smoothed_offsetoutput[i] = *m_smoothing_offset * smoothed_offsetoutput[i] + (1 - *m_smoothing_offset) * output_data[i];
            if (num_active_notes == 1)
            {
                smoothed_noteenergies[i] = *m_smoothing * smoothed_noteenergies[i] + (1 - *m_smoothing) * note_energy * output_data[i];
                smoothed_offsetnoteenergies[i] = *m_smoothing_offset * smoothed_offsetnoteenergies[i] + (1 - *m_smoothing_offset) * note_energy * output_data[i];
            }
            else
            {
                smoothed_noteenergies[i] = *m_smoothing * smoothed_noteenergies[i] + (1 - *m_smoothing) * note_energy;
                smoothed_offsetnoteenergies[i] = *m_smoothing_offset * smoothed_offsetnoteenergies[i] + (1 - *m_smoothing_offset) * note_energy;
            }

            if (!m_note_on[i].load(std::memory_order_relaxed))
            {
                triggeron[i] = (smoothed_onsetoutput[i] > *m_onset_threshold && smoothed_noteenergies[i] > en_threshold);
            }
            else
            {
                triggeroff[i] = (smoothed_offsetoutput[i] < *m_offset_threshold && smoothed_offsetnoteenergies[i] < en_threshold_offset);
            }
        }
        if (num_active_notes == 1) // fix for bendings in solos
            for (int i = 0; i < (NUM_NOTES - 1); i++)
            {
                if (smoothed_onsetoutput[i] > smoothed_onsetoutput[i + 1])
                {
                    triggeroff[i + 1] = m_note_on[i + 1].load(std::memory_order_relaxed);
                }
                else
                {
                    triggeroff[i] = m_note_on[i].load(std::memory_order_relaxed);
                }
            }
        for (int i = 0; i < NUM_NOTES; i++)
        {
            if (triggeron[i])
            {
                const NotePostResult post = postProcessNote(m_post, i + NOTE_OFFSET);
                if (!post.keep)
                {
                    // Out of scale or out of MIDI range: no note, and no note
                    // to release later.
                    ++m_notesFiltered;
                    continue;
                }
                int velocity = (int)(smoothed_noteenergies[i] / (gain * expressivity) * 127);
                velocity = std::min(velocity, 127); // cap the velocity at 127
                uint8_t midinote[3] = {static_cast<uint8_t>(0x90 | (post.channel - 1)),
                                       static_cast<uint8_t>(post.pitch),
                                       static_cast<uint8_t>(velocity)};
                if (m_midioutput)
                    m_midioutput->sendMidiMessage(midinote, pos);
                m_emittedPitch[i] = post.pitch;
                m_emittedChannel[i] = post.channel;
                m_note_on[i].store(true, std::memory_order_relaxed);
                ++m_noteOns;
            }
            if (triggeroff[i] && m_note_on[i])
            {
                const int channel = m_emittedChannel[i] > 0 ? m_emittedChannel[i] : 1;
                uint8_t midinote[3] = {static_cast<uint8_t>(0x90 | (channel - 1)),
                                       static_cast<uint8_t>(m_emittedPitch[i]), 0x00};
                if (m_midioutput)
                    m_midioutput->sendMidiMessage(midinote, pos);
                m_note_on[i].store(false, std::memory_order_relaxed);
            }
        }
    }

    void NoteInferencer::processHop(int64_t pos)
    {
        const uint64_t stamp = ++m_hopsProcessed;
        // A refused frame is a hole in the stream; the resulting stamp gap is
        // handled in drainResults once inference catches up.
        m_model.add_audio_input(m_audiobuffer.audio_buffer_2D, stamp);

        drainResults(pos);

        // Render mode (backpressure set): the host is not on a clock, so do
        // not let the audio run further ahead of the results than the schedule
        // allows. Without this the producer would race to fill the queue, the
        // results would all arrive older than the schedule and be discarded,
        // and an offline render would come out silent.
        if (m_backpressureMs > 0 && m_hopsProcessed > PIPELINE_LATENCY_HOPS)
        {
            const uint64_t needed = m_hopsProcessed - PIPELINE_LATENCY_HOPS;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(m_backpressureMs);
            while ((!m_haveLastResultStamp || m_lastResultStamp < needed) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
                drainResults(pos);
            }
        }

        emitDueResults(pos);
    }

    void NoteInferencer::getDebugData(float *smoothed_confidences, float *smoothed_energies,
                                      float *momentary_confidences, float *momentary_energies,
                                      bool *note_on) const
    {
        for (int i = 0; i < NUM_NOTES; ++i)
        {
            smoothed_confidences[i] = smoothed_onsetoutput[i];
            smoothed_energies[i] = smoothed_noteenergies[i];
            momentary_confidences[i] = m_momentary_confidences[i];
            momentary_energies[i] = m_momentary_noteenergies[i];
            if (note_on != nullptr)
                note_on[i] = m_note_on[i].load(std::memory_order_relaxed);
        }
    }

    NoteInferenceStats NoteInferencer::getStats() const
    {
        NoteInferenceStats stats;
        stats.hopsProcessed = m_hopsProcessed;
        stats.inputDropped = m_model.inputDropped();
        stats.outputDropped = m_model.outputDropped();
        stats.streamGaps = m_streamGaps;
        stats.staleDropped = m_staleDropped;
        stats.late = m_late;
        stats.noteOns = m_noteOns;
        stats.notesFiltered = m_notesFiltered;
        stats.invokeFailures = m_model.invokeFailures();
        stats.meanInferenceMs = m_model.meanInferenceMs();
        stats.maxInferenceMs = m_model.maxInferenceMs();
        return stats;
    }

} // namespace GuitarMidi
