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
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <guitarmidi/common.hpp>
#include <guitarmidi/ringbuffer.hpp>
#include <tensorflow/lite/version.h>
#include "tensorflow/core/public/release_version.h"
#include "tensorflow/core/public/version.h"
#include <tensorflow/lite/core/interpreter_builder.h>
#include <tensorflow/lite/interpreter.h>
#include <tensorflow/lite/kernels/register.h>
#include <tensorflow/lite/model_builder.h>
#include <tensorflow/lite/optional_debug_tools.h>
#include <tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h>

using namespace tflite;

/*
 * ModelInferencer runs the neural net on its own thread so the audio thread
 * only pays for the filter bank.
 *
 * The two threads communicate through two bounded, stamped queues: the audio
 * thread queues a completed analysis frame (148 x BUFFER_SIZE rectified filter
 * envelopes) with the index of the hop it came from, and the worker returns a
 * 37-note probability vector with that same index. Stamps are what let
 * NoteInferencer schedule results deterministically and notice a hole in the
 * stream; an un-stamped queue can only guess, which is what made note timing
 * drift under load.
 *
 * The worker sleeps on an atomic wait until a frame is queued. The previous
 * implementation spun on a flag, which burned a core and starved the audio
 * thread whenever inference became expensive.
 */
namespace GuitarMidi
{
class ModelInferencer
{
public:
    /** Size of the per-string head: 6 strings x 13 frets. */
    static constexpr size_t kStringValues = NUM_STRINGS * NUM_FRETS;
    /** One result frame: 37 note probabilities followed by the 78 string values. */
    static constexpr size_t kFrameValues = NUM_NOTES + kStringValues;

private:
    using InputQueue = SpscRingBuffer<RING_BUFFER_SIZE, NUM_NOTES * NUM_HARMONICS * BUFFER_SIZE>;
    using OutputQueue = SpscRingBuffer<RING_BUFFER_SIZE, kFrameValues>;

    std::unique_ptr<FlatBufferModel> m_model;
    std::unique_ptr<Interpreter> m_interpreter;
    InputQueue m_input_queue;
    OutputQueue m_output_queue;

    std::thread m_inferencing_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_wake{0};
    std::promise<bool> m_initialization_promise;

    std::atomic<uint64_t> m_infer_us_sum{0};
    std::atomic<uint64_t> m_infer_us_max{0};
    std::atomic<uint64_t> m_infer_count{0};
    std::atomic<int> m_inference_delay_ms{0};
    std::atomic<int> m_backpressure_ms{0};
    bool m_has_string_output = false;
    int m_notes_output = -1;
    int m_strings_output = -1;
    std::atomic<uint64_t> m_invoke_failures{0};

    /** Runs model inference on frames received from the audio thread. */
    void inferencing_loop(const std::string &bundle_path);
    /** Loads the model and allocates the interpreter. */
    bool initialize_interpreter(const std::string &bundle_path);

public:
    /** Creates the model inferencer. */
    ModelInferencer();
    /** Stops inference and releases model resources. */
    ~ModelInferencer();

    /** Loads the model from the plugin bundle and starts the worker. */
    bool initialize(const std::string &bundle_path);
    /** Finalizes inference. Must be called before destruction. */
    void finalize();

    /**
     * Queue one analysis frame. `stamp` identifies the hop it belongs to.
     * Returns false if the queue was full; the result is never overwritten.
     */
    bool add_audio_input(const float *input, uint64_t stamp);
    /**
     * Pop the next result. Returns false when none is ready; on success
     * `stamp` is set to the hop the result was computed from.
     */
    bool get_model_output(float *output, uint64_t &stamp);

    uint64_t inputDropped() const { return m_input_queue.dropped(); }
    /** True when the loaded model exposes the 6 x 13 string/fret head. */
    bool hasStringOutput() const { return m_has_string_output; }
    uint64_t invokeFailures() const;
    uint64_t outputDropped() const { return m_output_queue.dropped(); }

    /** Mean/max wall time of one interpreter invocation, in milliseconds. */
    double meanInferenceMs() const;
    double maxInferenceMs() const;

    /**
     * Testing/diagnostics hook: make each inference take at least this long,
     * so the bounded-queue and staleness behaviour can be exercised without a
     * slow machine. 0 disables the delay.
     */
    void setInferenceDelayMs(int delay_ms)
    {
        m_inference_delay_ms.store(delay_ms, std::memory_order_relaxed);
    }

    /**
     * How long the producer may wait for room when the queue is full.
     *
     * 0 (default) is the real-time policy: refuse the frame, keep the audio
     * thread's deadline, let the consumer report and flush the gap.
     *
     * A non-zero value trades the deadline for completeness, which is what an
     * offline render wants: the host is not on a clock, so waiting for
     * inference keeps every note instead of dropping the ones that arrive
     * while the model is busy.
     */
    void setBackpressureMs(int milliseconds)
    {
        m_backpressure_ms.store(milliseconds, std::memory_order_relaxed);
    }
};
} // namespace GuitarMidi
