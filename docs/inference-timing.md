# Inference timing: why the queue had to change

The detector runs the neural net on a worker thread and hands results back to
the audio thread. That hand-off used an un-timestamped ring buffer that
overwrote its own unread frames, and the audio thread stamped every result with
the *current* block position. The consequence was not a rare glitch but a
systematic one: note timing depended on how fast the machine happened to run
the model, on the host block size, and on how far the worker had fallen behind.

This document records what was wrong, what the pipeline does now, and the
before/after measurements produced by `tools/latency_report`.

## What was wrong

1. **Results carried no time.** `SpscRingBuffer` stored only the payload. A
   result that surfaced two hops late was indistinguishable from a fresh one,
   and `NoteInferencer::process()` emitted it at the current block's frame
   offset. The plugin also did not expose `lv2:latency`, so the host had
   nothing to compensate.
2. **The producer ignored capacity.** `add_data()` wrote unconditionally and
   wrapped. One input frame is 148 KiB (`37 notes × 4 harmonics × 256`), so a
   wrap while the worker was reading spliced two different moments into one
   analysis window — worse than a dropped note.
3. **The worker spun.** `while (!stop) if (has_new_data()) …` burned a core and
   competed with the audio thread (and with XNNPACK's four workers).
4. **Frames depended on the host block size.** The filter bank wrote into a
   rolling 256-sample buffer and the inferencer sampled it on a `m_frames`
   accumulator, so with 100- or 300-sample blocks the model was handed a
   rotated window; a chunk crossing the row boundary also wrote into the
   neighbouring filter's row.
5. **Nothing reported a hole.** If the model fell behind, the plugin kept
   emitting stale detections and held notes that no longer matched the audio,
   with no counter to say so.

## What the pipeline does now

* **Stamped frames.** `core/include/guitarmidi/ringbuffer.hpp` is a bounded
  SPSC queue of `(stamp, frame)` pairs. `push()` refuses when full instead of
  overwriting; the payload and the stamp are published together with a release
  store and consumed with an acquire load.
* **A fixed schedule.** Every result for hop *N* is emitted at hop
  *N + PIPELINE_LATENCY_HOPS*, so the delay is constant regardless of when the
  worker finished. The constant is reported to the host as `lv2:latency`
  (`(1 + PIPELINE_LATENCY_HOPS) × 256` frames = 768 frames ≈ 16 ms at 48 kHz).
  Results that miss the schedule are still emitted and counted as `late`.
* **Contiguous frames.** `FilterBank::captureFrame()` copies the newest 256
  samples per filter into a linear buffer, and `FretBoard::process()` drives
  the filter bank in chunks that end exactly on a hop. The model therefore sees
  the same window for every host block size.
* **Holes are explicit.** A gap in the result stamps (a refused frame on
  either queue) flushes every held note, resets the smoothing state and
  increments `streamGaps`. Results older than `MAX_RESULT_AGE_HOPS` are dropped
  instead of being emitted in the wrong place.
* **The worker sleeps** on `std::atomic::wait`, woken by the producer, and is
  stopped with an atomic flag and a notification.
* **Observability.** `getInferenceStats()` exposes hops processed, input/output
  drops, stream gaps, stale and late results, note-ons and the mean/max
  inference time. The LV2 output port `latency` carries the declared delay.

### Backpressure (offline rendering)

0 ms is the real-time policy: refuse the frame and keep the audio deadline.
With that policy an offline render (a host processing as fast as it can) would
outrun the worker and lose everything, so `setBackpressureMs()` switches to a
hand-off: the producer waits for the result that is due now, and for room in
the queue, instead of dropping. An offline render then produces the same
timing as a real-time run.

The plugin exposes no UI for this yet; a host-side render mode or a preset
would be the place to set it.

## Measurements

`tools/latency_report` feeds synthetic plucks at known sample positions through
the public `FretBoardAPI` and reports, per note, the delay between the true
onset and the MIDI note-on. It only uses the interface that predates this work,
so the same source builds against `master` and against this branch. Both runs
used gain 20 dB, onset smoothing 0.3, onset confidence 0.8, and a 48 kHz
real-time feed.

Mixed phrase (8 plucks, E2…E4):

| block | before: mean / max latency | before: false positives | after: mean / max latency | after: false positives |
|---|---|---|---|---|
| 64 | 29.50 / 38.67 ms | 4 | 32.98 / 41.31 ms | 1 |
| 100 | 35.42 / 47.92 ms | 8 | 33.00 / 41.33 ms | 1 |
| 256 | 35.00 / 43.33 ms | 0 | 32.98 / 41.31 ms | 1 |
| 300 | 31.90 / 55.33 ms | 2 | 33.00 / 41.33 ms | 1 |
| 480 | 36.59 / 55.33 ms | 0 | 33.00 / 41.33 ms | 1 |

Repeated pitch (8 plucks on A3; standard deviation is timing jitter, since the
pitch is constant):

| block | before: mean / sd | after: mean / sd |
|---|---|---|
| 64 | 30.34 / 1.66 ms | 32.32 / 1.53 ms |
| 100 | 31.25 / 0.01 ms | 32.33 / 1.52 ms |
| 256 | 35.00 / 2.93 ms | 32.32 / 1.53 ms |
| 300 | 30.34 / 0.01 ms | 32.34 / 1.53 ms |
| 480 | 35.34 / 0.01 ms | 32.33 / 1.53 ms |

Offline (block 256, no real-time pacing):

| build | detected | mean / max latency |
|---|---|---|
| before | 6/6 | 86.34 / 100.00 ms |
| after, real-time policy | 0/6 | — (frames are refused, by design) |
| after, backpressure | 6/6 | 32.09 / 34.65 ms |

Reading the numbers:

* **The block size stops mattering.** Before, the same phrase measured 29.5 ms
  at 64 samples and 36.6 ms at 480 samples, with up to 8 phantom notes at
  block 100 (the rotated/overlapping frames). After, every block size gives the
  same mean and the same maximum.
* **Offline rendering stops lying.** Before, results were stamped with the
  block that happened to pop them, so a fast render reported "late" notes with
  a growing, unpredictable lag; now the drop policy is explicit, and
  backpressure restores normal timing without the clock.
* **The absolute latency is slightly higher in the best case** (about +2 ms):
  the fixed schedule spends `PIPELINE_LATENCY_HOPS` hops of headroom so that a
  result that arrives one hop late is still on time. That is the price of not
  drifting; the declared 16 ms is the algorithmic part, while the measured
  32–33 ms additionally contains the model's rise time and the confidence
  one-pole, which depend on the detection controls.
* The residual false positive (1 per phrase) is the model's harmonic double,
  which is present at every block size and is a model issue, not a timing one.

## Reproducing

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build latency_report

# Before/after comparison, from a checkout of the old and the new revision:
./build/tools/latency_report --model python/neuralnetmodelling \
    --block 300 --realtime 1 --notes 8

# Jitter for one pitch:
./build/tools/latency_report --model python/neuralnetmodelling \
    --block 300 --realtime 1 --notes 8 --pitch 50

# Offline render (needs backpressure to keep the notes):
./build/tools/latency_report --model python/neuralnetmodelling \
    --block 256 --realtime 0 --notes 6 --pitch 50 --backpressure 200
```

Unit and pipeline tests (CTest):

```bash
ninja -C build test_ringbuffer test_filterbank_frame test_pipeline_timing
ctest --test-dir build --output-on-failure
```

* `test_ringbuffer` — capacity, refusal, ordering, and a threaded SPSC stress
  run that checks for torn frames and stamps.
* `test_filterbank_frame` — a captured frame is contiguous and identical
  whatever chunk sizes the caller used.
* `test_pipeline_timing` — end-to-end: every block size detects the phrase,
  events stay inside their block and in order, latency stays within the
  declared delay plus one block, a deliberately slow model produces drops and
  gaps without hanging notes, and an offline render with backpressure keeps
  every note.
