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
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace GuitarMidi
{

/**
 * Single-producer/single-consumer ring of fixed-size float frames.
 *
 * Each frame carries a monotonically increasing stamp (the index of the
 * analysis hop it was produced for) so the consumer can tell *which* audio
 * the result belongs to. Without that, a result that surfaces a few
 * milliseconds late cannot be distinguished from a fresh one, and the
 * host has no way to compensate.
 *
 * Properties, in order of importance for audio use:
 *
 *  - **Bounded and lossless by refusal.** push() returns false when the
 *    queue is full instead of overwriting the slot a reader is copying
 *    from. The old implementation wrapped around and produced frames
 *    spliced from two different points in time; a 148 KiB input frame is
 *    wide enough for that to be audible garbage rather than a dropped note.
 *  - **Lock-free.** Neither side takes a mutex; the producer never blocks
 *    and the consumer wakes on the producer's notification.
 *  - **Counts what it refused**, so callers can flush held notes when the
 *    stream had a hole.
 *
 * NumSlots must be a power of two. Read and write indices are free-running
 * counters (`size()` is their difference), which avoids the "wasted slot"
 * convention and overflow for practical durations.
 */
template <size_t NumSlots, size_t Stride>
class SpscRingBuffer
{
    static_assert(NumSlots > 0 && (NumSlots & (NumSlots - 1)) == 0,
                  "NumSlots must be a power of two");
    static_assert(Stride > 0, "Stride must be non-zero");

public:
    static constexpr size_t capacity() { return NumSlots; }
    static constexpr size_t stride() { return Stride; }

    /** True when no unread frame is queued. */
    bool empty() const
    {
        return m_write.load(std::memory_order_acquire) ==
               m_read.load(std::memory_order_relaxed);
    }

    /** Number of frames queued right now (approximate under concurrency). */
    size_t size() const
    {
        const uint64_t w = m_write.load(std::memory_order_acquire);
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        const uint64_t used = w - r;
        return used > NumSlots ? NumSlots : static_cast<size_t>(used);
    }

    /** Frames the pipeline had to abandon (refusals it did not retry). */
    uint64_t dropped() const
    {
        return m_dropped.load(std::memory_order_relaxed);
    }

    /**
     * Record that a frame was abandoned.
     *
     * Counting lives at the call site because only the caller knows whether a
     * refusal was final (real-time policy) or just a retry (backpressure).
     */
    void countDrop()
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * Producer side: queue one frame unless the queue is full.
     * Never blocks, never overwrites. Returns false when the frame was not
     * queued; the caller decides whether that is a drop (countDrop()).
     */
    bool push(const float *data, uint64_t stamp)
    {
        const uint64_t w = m_write.load(std::memory_order_relaxed);
        const uint64_t r = m_read.load(std::memory_order_acquire);
        if (w - r >= NumSlots)
            return false;
        const size_t slot = static_cast<size_t>(w & (NumSlots - 1));
        std::memcpy(m_buffer[slot], data, Stride * sizeof(float));
        m_stamps[slot] = stamp;
        // Release publishes the payload and the stamp together.
        m_write.store(w + 1, std::memory_order_release);
        return true;
    }

    /**
     * Consumer side: pop the oldest unread frame and its stamp.
     * Returns false when the queue is empty.
     */
    bool pop(float *out, uint64_t &stamp)
    {
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        if (m_write.load(std::memory_order_acquire) == r)
            return false;
        const size_t slot = static_cast<size_t>(r & (NumSlots - 1));
        std::memcpy(out, m_buffer[slot], Stride * sizeof(float));
        stamp = m_stamps[slot];
        m_read.store(r + 1, std::memory_order_release);
        return true;
    }

    /** Discard every queued frame. Only safe while the producer is stopped. */
    void reset()
    {
        const uint64_t w = m_write.load(std::memory_order_acquire);
        m_read.store(w, std::memory_order_release);
        m_dropped.store(0, std::memory_order_relaxed);
    }

private:
    float m_buffer[NumSlots][Stride] = {};
    uint64_t m_stamps[NumSlots] = {};
    std::atomic<uint64_t> m_write{0};
    std::atomic<uint64_t> m_read{0};
    std::atomic<uint64_t> m_dropped{0};
};

} // namespace GuitarMidi
