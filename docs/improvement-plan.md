# Improvement plan

Proposed work on GuitarMidi-LV2 derived from the tracker comparison with
Stringler (`stringler/docs/TRACKER-COMPARISON.md`). The comparison identified
the structural difference: Stringler is a mono-first, expressive tracker;
GuitarMidi is a poly-first, note-level classifier. Its weaknesses are therefore
expression, range and hosting, not chord detection.

Nothing here is implemented (except where noted). Each item lists why it
matters, the concrete change, its cost, and how to verify it. The measurement
tool and tests from `docs/inference-timing.md` are the yardstick.

Status: everything is delivered on one branch, `improvements`, as a stack of
self-contained commits (timing/queue work first, then one commit per item
below), so individual commits can be proposed upstream as separate PRs.

## Progress

| Item | State |
|---|---|
| 1 defaults | done |
| 2 post-processing | scale/transpose/split done; harmonic suppression and velocity-to-CC deferred (see item) |
| 3 mono pitch bend | deferred; largest item, needs its own test with synthetic glides |
| 4 hosting | no `exit()`, no audio-path I/O, thread budget, MIDI-in/panic and `lv2:enabled` bypass done; resampler latency outstanding |
| 5 velocity/thresholds | velocity from the strongest harmonic done; the dB threshold law outstanding |
| 6 tuning/range | tuning shift done; per-string output / MPE outstanding |
| 7 sub-hop onset placement | deferred; only pays off with a one-hop-longer schedule, so it needs a scheduling change first |

## Priority summary

| # | Item | Value | Effort |
|---|---|---|---|
| 1 | Reconcile the documented defaults with the TTL | Removes ~30 ms of latency for users who got the wrong values | minutes |
| 2 | Post-processing: harmonic suppression, scale, transpose, split, CCs | Fewer phantom notes; makes the plugin usable for bass/lead splits and keys | small |
| 3 | Mono pitch-bend path | Turns a note detector into an instrument (bends, vibrato, slides) | medium |
| 4 | Hosting robustness: no `exit()`, bypass, panic, MIDI in, thread budget | Prevents taking the host down and hanging notes | small–medium |
| 5 | Velocity and energy-threshold calibration | Velocity from a dark/neck tone is wrong; the "dB" thresholds are not dB | small |
| 6 | Tuning shift, wider range, per-string output / MPE | Alternate tunings now; extended range needs retraining | small for shift, large for the rest |
| 7 | Sub-hop onset placement | Removes up to 5.3 ms of quantisation | small |

## 1. Reconcile the defaults

The README and `guitarmidi.ttl` disagree about the shipped defaults, and the
onset smoother is the single largest contributor to perceived latency.

| Control | README | TTL |
|---|---|---|
| Onset Smoothing | 0.8 | 0.3 |
| Offset Smoothing | 0.3 | 0.3 |
| Onset Confidence Threshold | 0.80 | 0.95 |
| Offset Confidence Threshold | 0.10 | 0.3 |
| Onset Energy Threshold | 0.70 dB | −18 dB |
| Offset Energy Threshold | −18 dB | −18 dB |
| Input Gain | 0.0 dB | 28 dB |

Effect: in the measurement harness, `Onset Smoothing = 0.8` gives a mean
detection latency of ~63 ms; `0.3` gives ~32 ms for the same phrase.

Change: pick one set (the TTL is what hosts actually use), make the README
match, and state in both that the onset smoother trades false triggers for
latency. Keep `tools/latency_report` numbers next to the control in the docs.

Verify: `./build/tools/latency_report --model python/neuralnetmodelling
--block 256 --realtime 1 --notes 8` with the documented values.

## 2. Post-processing on the 37-note vector

All of this runs on the model output and needs no retraining.

* **Harmonic-double suppression.** The comparison's "retire the harmonics"
  idea, in the cheap form: if note `N` and `N+12` both fire, drop the upper
  when `N`'s 2nd-harmonic band energy exceeds the upper note's fundamental
  band energy. The filter bank already exposes those bands
  (`FilterBank::get_frame_buffer()`), and the reports still show 1–2 phantom
  notes per phrase at every block size.
* **Scale filter.** 17 scales + root, applied to the final pitch; drop
  out-of-scale notes before the MIDI write. Table only.
* **Transpose / octave / channel split.** `final = raw + transpose + octave`;
  split at a pitch to send lows and highs to different MIDI channels. Useful
  for bass-plus-lead setups and cheap to implement on top of the existing
  note index.
* **Velocity to CC.** Brightness/loudness CC per note from the velocity, as
  Stringler does. Amount 0 disables.

Cost: roughly 40 + 30 + 40 + 20 lines, each independently testable. Add cases
to `test_pipeline_timing` or a new `test_postprocess` that feeds synthetic
probabilities instead of audio (no model needed, fast).

## 3. Mono pitch-bend path

The comparison's biggest finding: GuitarMidi has no continuous pitch. The
current workaround in `NoteInferencer::applyResult` force-releases the quieter
of two adjacent notes so a bend appears as a retrigger.

Proposal: when exactly one note is active, run a monophonic periodicity
detector (YIN-style: FFT autocorrelation, cumulative mean normalised
difference, parabolic interpolation) on the raw input, gated by the same hop
and level gate, and emit the deviation as **pitch bend** instead of a new
note. Outside a bend window, commit a new note as today.

* Reuses the existing hop/scheduling machinery; no new thread.
* Allocation-free; ~150 lines including the FFT wrapper (the core already has
  no FFT, but a radix-2 transform is small).
* **Licensing:** Stringler is GPL-2.0+ and the GuitarMidi core is LGPL. Do not
  copy Stringler's implementation; YIN is a published algorithm, implement it
  from the paper.
* Sets up MPE later: with per-note channels, bends are per-note.

Verify: a new `test_pitch_track` with synthetic glides (a sine swept ±200
cents) asserting the emitted bend follows the sweep and that no spurious
note-ons appear; plus a manual check against a real guitar.

## 4. Hosting robustness

* **Never `exit()` from a plugin.** `TFLITE_MINIMAL_CHECK` in
  `core/src/modelinferencer.cpp:9` calls `exit(1)`; it is used on `Invoke()`
  at line 54, on model load at 93 and on `AllocateTensors` at 109. A failure
  in the inference thread kills the host. Log and disable detection instead.
* **Bypass.** No `lv2:enabled` port exists, so bypassing can leave notes
  hanging. Add the port and flush held notes when it turns on, as Stringler
  does.
* **Panic / note flush.** `FretBoard::finalize()` sends no note-offs. There is
  also no MIDI input port at all, so CC 123 and sustain cannot reach the
  plugin and a host stop leaves notes on. A MIDI-in port merged with the
  tracker (Stringler's "Both" source) would fix panic, sustain and the "play
  a keyboard over the guitar" case at once.
* **No `printf` on the audio path.** `src/midioutput.cpp:41,46,51,98` print
  from inside `run()` on failure. Drop or rate-limit them.
* **Thread budget.** `NUM_INFERENCE_THREADS = 4` (`core/CMakeLists.txt:10`)
  on a Pi leaves three cores for JACK minus everything else. Default to
  `cores - 1`, and put the worker below the audio thread.
* **Resampler latency.** At rates other than 48 kHz the zita resampler adds
  delay that is not in the reported `lv2:latency`. Include it, and add a
  44.1 kHz case to the timing test.

## 5. Velocity and energy-threshold calibration

`core/src/noteinferencer.cpp`:

* Velocity uses the fundamental band only (`int h = 1;` at line 137, with the
  harmonic loop commented out). A neck pickup or a dark tone therefore
  under-reports velocity. Sum or weight all four harmonics.
* `en_threshold = pow(10, *m_onset_energy_threshold / 20) * gain` (line 128)
  is compared against a **sum of 256 rectified samples** (line ~145), not an
  RMS or a dB value. That is why the README tells users to raise the onset
  energy threshold "to reduce spurious onsets of lower notes" — the control
  does not behave like decibels. Normalise the band energy (divide by
  `BUFFER_SIZE` or convert to a real RMS→dB) so the thresholds mean what they
  say.

Verify: a calibration test that sweeps input gain and checks that the same
threshold admits/rejects the same signal level.

## 6. Tuning, range and per-string output

* **Tuning shift (cheap).** Multiply the filter centre frequencies by
  `2^(semitones/12)` in `FilterBank::setup` before designing the biquads, and
  add the same offset back to the output note. Drop tunings and capo
  positions then work with the existing model, because it only sees relative
  band structure.
* **Wider range (expensive).** `NUM_NOTES = 37` (`common.hpp:27`) with
  `NOTE_OFFSET = 40` covers E2–E5. Anything above the 12th fret of the high E
  is absent. Extending it is a model/training task, i.e. the funded path
  described in `AGENTS.md`, not a code change.
* **Per-string output (medium, enables MPE).** The model predicts per
  string/fret and then collapses to 37 pitch classes, discarding which string
  produced a note. Keeping the per-string probabilities (78 string/fret
  combinations) would allow a per-string MIDI channel, which combined with
  item 3 gives per-note bends — the MPE behaviour the comparison notes
  Stringler only partially has.

## 7. Sub-hop onset placement

Events are currently emitted at the hop end (`FretBoard::process_direct` calls
`processHop(pos + nsamples)`), so note timing is quantised to 5.3 ms on top of
the model and smoothing delays. Two options:

* Interpolate the threshold crossing between the previous and current
  smoothed confidence and offset the event by the fraction of the hop.
* Halve the hop to 128 samples with a 256-sample context, which gives the same
  resolution with the existing signal path but doubles filter-bank taps per
  unit of latency saved.

Expected gain: ±5.3 ms → about ±1 ms on the timing of the onset, without
changing the declared latency.

## Non-goals

* Beating the model's chord accuracy with DSP. The comparison is explicit
  that the learned classifier is the right tool for polyphony; the items above
  complement it rather than replace it.
* Reimplementing Stringler's expression engine wholesale. Scale/transpose/
  split/CC are worth having; the chord wheel and the octave-down companion
  are products of Stringler's original and have no place here.

## Delivery

Work happens on the `improvements` branch, one commit per item, in the order
above, each building and passing `ctest` on its own:

1. inference hand-off (timing, queue) - prerequisite for the rest
2. defaults reconciliation
3. this document
4. post-processing
5. mono pitch bend
6. hosting robustness
7. velocity/threshold calibration
8. tuning shift, then per-string/MPE
9. sub-hop onset placement

Any prefix of that stack is a coherent PR; a later commit that touches the
same file has been written on top of the earlier one, so propose them in
order. Where behaviour changes, record the before/after in
`tools/latency_report` terms rather than in prose.
