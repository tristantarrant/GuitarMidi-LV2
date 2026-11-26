#!/usr/bin/env python3
"""Export the production model with a second, per-string output.

The shipped guitarmidi.tflite ends at a 37-note vector, although the graph
computes a 6 x 13 string/fret tensor just before the scatter. This script
rebuilds the architecture with a batch of one, loads the trained weights and
exports both tensors:

    output 0: (1, 37)  note probabilities, unchanged
    output 1: (1, 78)  string/fret probabilities, row-major (string, fret)

It does not retrain anything and does not touch the shipped file; it writes a
new one, then checks that the two heads agree (the note output is the scatter
of the string output through the guitar mask).

Run it in a Python 3.11/3.12 environment with TensorFlow >= 2.16:

    python export_two_head.py --weights guitarmidi.weights.h5 \
                              --out guitarmidi_two_head.tflite

Note: the checked-in guitarmidi.weights.h5 predates the current architecture
(15 layers fail to load from it). The authoritative weights are the
model.weights.h5 member of guitarmidi.keras, which this script uses by default.

Note: build_1d_cnn_model's default input_shape omits the channel dimension;
callers pass common.INPUT_SHAPE, which is what this script does.
"""
import argparse
import os
import sys
import tempfile
import zipfile

import numpy as np
import tensorflow as tf

from model import build_1d_cnn_model, mask, N_FRETS, N_STRINGS

INPUT_SHAPE = (148, 256, 1)   # height, width, channels (common.INPUT_SHAPE)
NUM_NOTES = 37


def resolve_weights(path):
    """Returns usable weights: the explicit path, or the ones inside the
    .keras archive (the loose .h5 in the repo is from an older build)."""
    if path:
        return path
    if os.path.exists("guitarmidi.keras"):
        handle = tempfile.NamedTemporaryFile(suffix=".weights.h5", delete=False)
        with zipfile.ZipFile("guitarmidi.keras") as archive:
            handle.write(archive.read("model.weights.h5"))
        handle.close()
        print("using weights extracted from guitarmidi.keras")
        return handle.name
    return "guitarmidi.weights.h5"


def build_two_head_model(weights):
    model = build_1d_cnn_model(batch_sz=1, input_shape=INPUT_SHAPE,
                               output_dim=NUM_NOTES, training=False)
    model.load_weights(weights)

    scatter = next(layer for layer in model.layers
                   if layer.__class__.__name__ == "SparseGuitarOutput")
    # (1, 6, 13) -> (1, 78)
    strings = tf.keras.layers.Flatten(name="string_probabilities")(scatter.input)
    return tf.keras.Model(model.inputs, [model.outputs[0], strings],
                          name="guitar_note_detector_two_head")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", default="",
                        help="default: model.weights.h5 inside guitarmidi.keras")
    parser.add_argument("--out", default="guitarmidi_two_head.tflite")
    args = parser.parse_args()

    model = build_two_head_model(resolve_weights(args.weights))
    print("keras input:", model.input_shape)
    print("keras outputs:", [t.shape for t in model.outputs])

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]  # weights-only int8
    data = converter.convert()

    with open(args.out, "wb") as handle:
        handle.write(data)
    print(f"wrote {args.out} ({len(data)} bytes)")

    # --- validation -------------------------------------------------------
    interpreter = tf.lite.Interpreter(model_content=data)
    interpreter.allocate_tensors()
    inputs = interpreter.get_input_details()
    outputs = interpreter.get_output_details()
    print("tflite input:", inputs[0]["shape"])
    print("tflite outputs:", [o["shape"] for o in outputs])

    problems = []
    if int(np.prod(inputs[0]["shape"])) != 148 * 256:
        problems.append("input is not 148x256 per frame")
    if len(outputs) != 2:
        problems.append("expected two outputs")
    by_size = {}
    for output in outputs:
        by_size[int(np.prod(output["shape"]))] = output
    if NUM_NOTES not in by_size:
        problems.append("no 37-value note output")
    if N_STRINGS * N_FRETS not in by_size:
        problems.append("no 78-value string output")

    if not problems:
        rng = np.random.default_rng(1)
        frame = rng.random(inputs[0]["shape"], dtype=np.float32)
        interpreter.set_tensor(inputs[0]["index"], frame)
        interpreter.invoke()
        notes = interpreter.get_tensor(by_size[NUM_NOTES]["index"]).reshape(-1)
        strings = interpreter.get_tensor(by_size[N_STRINGS * N_FRETS]["index"]).reshape(-1)
        # The note output must be the "OR" scatter of the string output.
        scattered = 1.0 - np.prod(
            1.0 - strings.reshape(-1, 1) * mask.astype(np.float32), axis=0)
        if not np.allclose(scattered, notes, atol=2e-5):
            problems.append("the two heads disagree (max delta %.3g)"
                            % float(np.max(np.abs(scattered - notes))))

    if problems:
        for problem in problems:
            print("FAILED:", problem, file=sys.stderr)
        return 1

    print("OK: shapes and head consistency verified")
    print("Next: swap the bundle's guitarmidi.tflite for this file and run ctest;")
    print("the plugin reads the second output when it is present.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
