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
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <guitarmidi/fretboard_api.hpp>
#include <guitarmidi/common.hpp>
#include <guitarmidi/config.hpp>
#include <guitarmidi/modelinferencer.hpp>
#include <guitarmidi/notepostprocess.hpp>
#include <guitarmidi/pitchtracker.hpp>

namespace GuitarMidi
{
    /**
     * Converts model note predictions into MIDI events.
     *
     * Runs on the audio thread. Once per analysis hop the caller hands it a
     * contiguous filter-bank frame; the frame is queued for the inference
     * thread and results are emitted on a fixed schedule:
     *
     *  - every result carries the hop index it was computed from,
     *  - a result for hop N is emitted at hop N + PIPELINE_LATENCY_HOPS,
     *    which makes the delay constant and host-compensatable (the plugin
     *    reports it as lv2:latency) instead of "whenever inference finished",
     *  - results that miss the schedule are still emitted and counted, results
     *    older than MAX_RESULT_AGE_HOPS are dropped,
     *  - a hole in the result stamps (a refused frame on either queue) flushes
     *    all held notes and resets the smoothers, because the continuity the
     *    thresholds rely on is gone.
     */
    class NoteInferencer
    {
        static constexpr int kMaxPending = PIPELINE_LATENCY_HOPS + 4;

        struct PendingResult
        {
            uint64_t stamp = 0;
            float probabilities[NUM_NOTES] = {};
        };

        ModelInferencer m_model;

        GuitarMidiOutput *m_midioutput = nullptr;
        AudioBuffer2D m_audiobuffer{};
        float *m_gain_db = nullptr;
        float *m_expressivity_db = nullptr;
        float *m_onset_threshold = nullptr;
        float *m_offset_threshold = nullptr;
        float *m_onset_energy_threshold = nullptr;
        float *m_offset_energy_threshold = nullptr;
        float *m_smoothing = nullptr;
        float *m_smoothing_offset = nullptr;
        float smoothed_onsetoutput[NUM_NOTES] = {0};
        float smoothed_offsetoutput[NUM_NOTES] = {0};
        float smoothed_noteenergies[NUM_NOTES] = {0};
        float smoothed_offsetnoteenergies[NUM_NOTES] = {0};
        // Velocity follows the strongest harmonic band, not just the
        // fundamental; kept separate so the gate behaviour is unchanged.
        float smoothed_velocityenergies[NUM_NOTES] = {0};
        float m_momentary_confidences[NUM_NOTES] = {0};
        float m_momentary_noteenergies[NUM_NOTES] = {0};

        int m_backpressureMs = 0;
        uint64_t m_hopsProcessed = 0;
        uint64_t m_ignoreResultsBefore = 0;
        uint64_t m_lastResultStamp = 0;
        bool m_haveLastResultStamp = false;
        uint64_t m_staleDropped = 0;
        uint64_t m_late = 0;
        uint64_t m_streamGaps = 0;
        uint64_t m_noteOns = 0;
        uint64_t m_notesFiltered = 0;

        // Note post-processing (scale, transpose, channel routing) and the
        // pitch/channel each tracked note was actually emitted on, so note-offs
        // match even if the settings changed while it was held.
        NotePostSettings m_post;
        MpeChannelPool m_mpe;
        PitchTracker m_pitch;
        float *m_bend_range = nullptr;
        int m_lastBend = 8192;
        int m_lastBendChannel = 1;
        int m_tuningOffset = 0;
        int m_emittedPitch[NUM_NOTES] = {};
        int m_emittedChannel[NUM_NOTES] = {};

        PendingResult m_pending[kMaxPending];
        int m_pendingHead = 0;
        int m_pendingCount = 0;

        std::atomic<bool> m_note_on[NUM_NOTES];

        /** Moves finished results into the pending queue and notices gaps. */
        void drainResults(int64_t pos);
        /** Applies the thresholds for one result and emits MIDI at `pos`. */
        void applyResult(const float *probabilities, int64_t pos);
        /** Emits the results whose scheduled hop has arrived. */
        void emitDueResults(int64_t pos);
        /** Releases every held note and clears the smoothing state. */
        void flushHeldNotes(int64_t pos);

    public:
        /** Creates a note inferencer connected to the supplied MIDI output. */
        NoteInferencer(GuitarMidiOutput *midioutput);
        /** Releases the MIDI output owned by the inferencer. */
        ~NoteInferencer()
        {
            if (m_midioutput != nullptr)
                delete m_midioutput;
        }

        /** Loads and initializes the neural-network model. */
        bool initialize(const std::string &bundle_path);
        /** Finalizes model inference and releases inference resources. */
        void finalize();
        /** Returns the MIDI output used for inferred notes. */
        GuitarMidiOutput *getMidiOutput() { return m_midioutput; }
        /** Sets the contiguous filter-bank frame consumed by inference. */
        void setAudioInputBuffer(AudioBuffer2D input) { m_audiobuffer = input; }

        /** Sets the gain control. */
        void setGain(float *gain) { m_gain_db = gain; }
        /** Sets the MIDI expressivity control. */
        void setExpressivity(float *expressivity) { m_expressivity_db = expressivity; }
        /** Sets the note-on threshold control. */
        void setOnsetThreshold(float *threshold) { m_onset_threshold = threshold; }
        /** Sets the note-off threshold control. */
        void setOffsetThreshold(float *threshold) { m_offset_threshold = threshold; }
        /** Sets the onset smoothing control. */
        void setSmoothing(float *smoothing) { m_smoothing = smoothing; }
        /** Sets the offset smoothing control. */
        void setSmoothingOffset(float *smoothing_offset) { m_smoothing_offset = smoothing_offset; }
        /** Sets the note-on energy threshold control. */
        void setOnsetEnergyThreshold(float *threshold) { m_onset_energy_threshold = threshold; }
        /** Sets the note-off energy threshold control. */
        void setOffsetEnergyThreshold(float *threshold) { m_offset_energy_threshold = threshold; }

        /** Guitar tuning offset: the detected note is reported this much lower. */
        void setTuningOffset(int semitones) { m_tuningOffset = semitones; }

        /** Note post-processing: scale, root, transpose and channel routing. */
        void setScale(int scale) { m_post.scale = scale; }
        void setScaleRoot(int root) { m_post.root = root; }
        void setTranspose(int semitones) { m_post.transpose = semitones; }
        void setSplitNote(int pitch) { m_post.splitNote = pitch; }
        void setLowChannel(int channel) { m_post.lowChannel = channel; }
        void setHighChannel(int channel) { m_post.highChannel = channel; }
        void setMpe(int enabled) { m_post.mpe = enabled; }

        /** Testing/diagnostics hook: lower bound for one inference, in ms. */
        void setInferenceDelayMs(int delay_ms) { m_model.setInferenceDelayMs(delay_ms); }
        /** Opt-in producer wait for room (offline rendering); 0 = never wait. */
        void setBackpressureMs(int milliseconds)
        {
            m_backpressureMs = milliseconds;
            m_model.setBackpressureMs(milliseconds);
        }

        /**
         * Handles one incoming MIDI message: forwards it to the output (the
         * plugin merges its input with the tracker) and treats the all-notes-off
         * controllers as a request to release the notes the tracker is holding.
         */
        void handleInputEvent(const MidiInputEvent &event);
        /** Releases every note the tracker is holding, at `pos`. */
        void flushNotes(int64_t pos) { flushHeldNotes(pos); }
        /** Clears held notes, pending results and smoothing state. */
        void resetState();
        /** Releases the MPE member channel a voice was using, if any. */
        void releaseChannel(int index);

        /** Feeds raw input audio to the monophonic bend tracker. */
        void pushAudio(const float *samples, int frames) { m_pitch.push(samples, frames); }
        /** Pitch-bend range in semitones; 0 (or null) disables bend tracking. */
        void setBendRange(float *range) { m_bend_range = range; }
        /** Emits pitch bend for the currently sounding note, if any. */
        void trackBend(int64_t pos);
        /** Recentres the pitch wheel if it was moved. */
        void resetBend(int64_t pos);

        /** Starts a MIDI output sequence before processing a block. */
        void preprocess()        {
            if (m_midioutput)
                m_midioutput->initializeSequence();
        }

        /** Completes a MIDI output sequence after processing a block. */
        void postprocess()
        {
            if (m_midioutput)
                m_midioutput->finalizeSequence();
        }

        /**
         * Called once per completed analysis hop, after the filter bank has
         * captured a contiguous frame. `pos` is the frame offset within the
         * host block at which that hop ended.
         */
        void processHop(int64_t pos);

        /** Constant pipeline delay in frames, including the analysis window. */
        static constexpr uint32_t latencyFrames()
        {
            return (1 + PIPELINE_LATENCY_HOPS) * BUFFER_SIZE;
        }

        /** Copies the latest smoothed and momentary confidence and energy values. */
        void getDebugData(float *smoothed_confidences, float *smoothed_energies,
                          float *momentary_confidences, float *momentary_energies,
                          bool *note_on = nullptr) const;

        /** Pipeline counters and inference timings. */
        NoteInferenceStats getStats() const;

#ifdef WITH_AUDIO_OUTPUT
        float *audio_output;
        /** Sets the optional audio output buffer. */
        void setAudioOutputBuffer(float *output) { audio_output = output; }
#endif
    };
} // namespace GuitarMidi
