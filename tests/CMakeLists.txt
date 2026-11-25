include_directories(${PROJECT_SOURCE_DIR}/core/include)
include_directories(${PROJECT_BINARY_DIR}/core/include)
include_directories(${PROJECT_SOURCE_DIR}/ext/zita-resampler-1.11.2/source)

add_executable(test_ringbuffer test_ringbuffer.cpp)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

add_executable(test_postprocess test_postprocess.cpp)
add_test(NAME postprocess COMMAND test_postprocess)

add_executable(test_onset test_onset.cpp)
add_test(NAME onset COMMAND test_onset)

add_executable(test_pitchtracker test_pitchtracker.cpp)
add_test(NAME pitchtracker COMMAND test_pitchtracker)

add_executable(test_mpe test_mpe.cpp)
add_test(NAME mpe COMMAND test_mpe)

add_executable(test_level test_level.cpp)
add_test(NAME level COMMAND test_level)

add_executable(test_midi_input test_midi_input.cpp)
target_link_libraries(test_midi_input PRIVATE guitarmidi_core)
add_test(NAME midi_input COMMAND test_midi_input)

# The frame-assembly test needs no TensorFlow: compile the DSP sources directly.
add_executable(test_filterbank_frame
    test_filterbank_frame.cpp
    ${PROJECT_SOURCE_DIR}/core/src/filterbank.cpp
    ${PROJECT_SOURCE_DIR}/core/src/fretboardrepresentation.cpp
    ${PROJECT_SOURCE_DIR}/core/src/guitarnote.cpp
    ${PROJECT_SOURCE_DIR}/core/src/guitarstring.cpp
    ${PROJECT_SOURCE_DIR}/core/src/logging.cpp)
add_test(NAME filterbank_frame COMMAND test_filterbank_frame)

if(EXISTS ${PROJECT_SOURCE_DIR}/python/neuralnetmodelling/guitarmidi.tflite)
    add_executable(test_pipeline_timing test_pipeline_timing.cpp)
    target_link_libraries(test_pipeline_timing PRIVATE guitarmidi_core)
    target_compile_definitions(test_pipeline_timing PRIVATE
        GUITARMIDI_TEST_MODEL_DIR="${PROJECT_SOURCE_DIR}/python/neuralnetmodelling")
    add_test(NAME pipeline_timing COMMAND test_pipeline_timing)
    set_tests_properties(pipeline_timing PROPERTIES TIMEOUT 240)
else()
    message(STATUS "guitarmidi.tflite not found; skipping the pipeline timing test")
endif()
