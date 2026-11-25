#include <guitarmidi/modelinferencer.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <tensorflow/lite/logger.h>

// TFlite C++ API reference: https://ai.google.dev/edge/api/tflite/cc?hl=en
#define TFLITE_MINIMAL_CHECK(x)                                  \
    if (!(x))                                                    \
    {                                                            \
        fprintf(stderr, "Error at %s:%d\n", __FILE__, __LINE__); \
        exit(1);                                                 \
    }

namespace GuitarMidi
{

    void ModelInferencer::inferencing_loop(const std::string &bundle_path)
    {
        if (!initialize_interpreter(bundle_path))
        {
            g_logger.error("Failed to initialize interpreter.\n");
            m_initialization_promise.set_value(false);
            return;
        }
        m_initialization_promise.set_value(true);

        // Local staging buffer: pop() writes the payload here and hands back
        // the stamp, so the interpreter's input tensor is only touched by this
        // thread and the queue performs exactly one copy.
        static thread_local float input_frame[NUM_NOTES * NUM_HARMONICS * BUFFER_SIZE];
        static thread_local float output_frame[NUM_NOTES];

        uint32_t seen = m_wake.load(std::memory_order_acquire);
        while (!m_stop.load(std::memory_order_acquire))
        {
            uint64_t stamp = 0;
            if (!m_input_queue.pop(input_frame, stamp))
            {
                // Sleep until the audio thread queues a frame. `seen` is read
                // after the failed pop, so a frame queued in between changes
                // the counter and the wait returns immediately.
                m_wake.wait(seen, std::memory_order_acquire);
                seen = m_wake.load(std::memory_order_acquire);
                continue;
            }

            float *input_buffer = m_interpreter->typed_input_tensor<float>(0);
            std::memcpy(input_buffer, input_frame, sizeof(input_frame));

            const int artificial_delay = m_inference_delay_ms.load(std::memory_order_relaxed);
            const auto begin = std::chrono::steady_clock::now();
            TFLITE_MINIMAL_CHECK(m_interpreter->Invoke() == kTfLiteOk);
            const auto end = std::chrono::steady_clock::now();
            const uint64_t elapsed_us =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());

            m_infer_us_sum.fetch_add(elapsed_us, std::memory_order_relaxed);
            m_infer_count.fetch_add(1, std::memory_order_relaxed);
            uint64_t previous_max = m_infer_us_max.load(std::memory_order_relaxed);
            while (elapsed_us > previous_max &&
                   !m_infer_us_max.compare_exchange_weak(previous_max, elapsed_us,
                                                         std::memory_order_relaxed))
            {
            }

            if (artificial_delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(artificial_delay));

            std::memcpy(output_frame, m_interpreter->typed_output_tensor<float>(0),
                        sizeof(output_frame));
            // A full output queue refuses the frame rather than overwriting an
            // older result; NoteInferencer sees the resulting stamp hole and
            // flushes its note state.
            if (!m_output_queue.push(output_frame, stamp))
                m_output_queue.countDrop();
            m_wake.fetch_add(1, std::memory_order_release);
            m_wake.notify_one();
        }
    }

    bool ModelInferencer::initialize_interpreter(const std::string &bundle_path)
    {
        const std::string tflite_path = bundle_path + "/guitarmidi.tflite";
        g_logger.info("Loading model from: %s\n", tflite_path.c_str());
        if (!std::filesystem::exists(tflite_path))
        {
            g_logger.error("Model file not found: %s\n", tflite_path.c_str());
            return false;
        }
        m_model = FlatBufferModel::BuildFromFile(tflite_path.c_str());
        TFLITE_MINIMAL_CHECK(m_model != nullptr);
        ops::builtin::BuiltinOpResolver resolver;
        InterpreterBuilder builder(*m_model, resolver);

        builder(&m_interpreter);
        TfLiteXNNPackDelegateOptions xnnpack_options =
            TfLiteXNNPackDelegateOptionsDefault();
        xnnpack_options.num_threads = NUM_INFERENCE_THREADS;
        xnnpack_options.weight_cache_file_path = TfLiteXNNPackDelegateInMemoryFilePath();

        if (m_interpreter->ModifyGraphWithDelegate(
                TfLiteXNNPackDelegateCreate(&xnnpack_options)) != kTfLiteOk)
        {
            g_logger.error("Failed to apply XNNPack delegate\n");
            return false;
        }
        TFLITE_MINIMAL_CHECK(m_interpreter->AllocateTensors() == kTfLiteOk);
        tflite::LoggerOptions::SetMinimumLogSeverity(tflite::TFLITE_LOG_ERROR);
        g_logger.info("Model loaded and interpreter initialized successfully.\n");
        g_logger.info("Inference thread count: %d\n", NUM_INFERENCE_THREADS);
        g_logger.info("Ring buffer size: %d frames\n", RING_BUFFER_SIZE);
        return true;
    }

    bool ModelInferencer::initialize(const std::string &bundle_path)
    {
        auto init_future = m_initialization_promise.get_future();
        m_inferencing_thread = std::thread(&ModelInferencer::inferencing_loop, this, bundle_path);
        const bool status = init_future.get();
        if (!status)
        {
            g_logger.error("Failed to initialize interpreter in inferencing thread.\n");
            return false;
        }
        return true;
    }

    void ModelInferencer::finalize()
    {
        if (!m_inferencing_thread.joinable())
            return;
        m_stop.store(true, std::memory_order_release);
        m_wake.fetch_add(1, std::memory_order_release);
        m_wake.notify_one();
        m_inferencing_thread.join();
        m_input_queue.reset();
        m_output_queue.reset();
    }

    ModelInferencer::ModelInferencer()
    {
        g_logger.info("Audio input ring buffer size: %d frames\n", RING_BUFFER_SIZE);
        g_logger.info("Model output ring buffer size: %d frames\n", RING_BUFFER_SIZE);
    }

    ModelInferencer::~ModelInferencer()
    {
        finalize();
    }

    bool ModelInferencer::add_audio_input(const float *input, uint64_t stamp)
    {
        if (m_input_queue.push(input, stamp))
        {
            m_wake.fetch_add(1, std::memory_order_release);
            m_wake.notify_one();
            return true;
        }

        // Opt-in backpressure (offline rendering): wait for room instead of
        // dropping, because the host is not holding a real-time deadline.
        const int budget_ms = m_backpressure_ms.load(std::memory_order_relaxed);
        if (budget_ms > 0)
        {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(budget_ms);
            while (std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
                if (m_input_queue.push(input, stamp))
                {
                    m_wake.fetch_add(1, std::memory_order_release);
                    m_wake.notify_one();
                    return true;
                }
            }
        }
        // Retries are not frames lost; only the final refusal is.
        m_input_queue.countDrop();
        return false;
    }

    bool ModelInferencer::get_model_output(float *output, uint64_t &stamp)
    {
        return m_output_queue.pop(output, stamp);
    }

    double ModelInferencer::meanInferenceMs() const
    {
        const uint64_t count = m_infer_count.load(std::memory_order_relaxed);
        if (count == 0)
            return 0.0;
        return static_cast<double>(m_infer_us_sum.load(std::memory_order_relaxed)) /
               static_cast<double>(count) / 1000.0;
    }

    double ModelInferencer::maxInferenceMs() const
    {
        return static_cast<double>(m_infer_us_max.load(std::memory_order_relaxed)) / 1000.0;
    }

} // namespace GuitarMidi
