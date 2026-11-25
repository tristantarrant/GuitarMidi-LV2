/*
 * The MIDI input path: messages are forwarded to the output (the plugin
 * merges its input with the tracker) and the all-notes-off controllers are
 * recognised as a request to release the tracker's notes.
 */
#include <guitarmidi/noteinferencer.hpp>

#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expression);
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

struct Message
{
    uint8_t data[3];
    int64_t frame;
};

struct Recorder : GuitarMidiOutput
{
    std::vector<Message> messages;

    void initializeSequence() override {}
    void finalizeSequence() override {}
    void sendMidiMessage(uint8_t midinote[3], int64_t frames) override
    {
        messages.push_back({{midinote[0], midinote[1], midinote[2]}, frames});
    }
};

GuitarMidi::MidiInputEvent event(uint8_t status, uint8_t data1, uint8_t data2)
{
    GuitarMidi::MidiInputEvent message;
    message.frame = 0;
    message.size = 3;
    message.data[0] = status;
    message.data[1] = data1;
    message.data[2] = data2;
    return message;
}

} // namespace

int main()
{
    auto *recorder = new Recorder();
    {
        GuitarMidi::NoteInferencer inferencer(recorder);
        inferencer.preprocess();

        // A note from the keyboard is forwarded unchanged.
        inferencer.handleInputEvent(event(0x93, 60, 100));
        CHECK(recorder->messages.size() == 1);
        CHECK(recorder->messages[0].data[0] == 0x93);
        CHECK(recorder->messages[0].data[1] == 60);
        CHECK(recorder->messages[0].data[2] == 100);
        CHECK(recorder->messages[0].frame == 0);

        // Sustain and pitch bend pass through untouched.
        inferencer.handleInputEvent(event(0xB0, 64, 127));
        inferencer.handleInputEvent(event(0xE0, 0, 64));
        CHECK(recorder->messages.size() == 3);
        CHECK(recorder->messages[1].data[1] == 64);
        CHECK(recorder->messages[2].data[0] == 0xE0);

        // Panic is forwarded, and also clears the tracker (no notes held here,
        // so the observable effect is the forwarded message itself).
        inferencer.handleInputEvent(event(0xB5, 123, 0));
        CHECK(recorder->messages.size() == 4);
        CHECK(recorder->messages[3].data[0] == 0xB5);
        CHECK(recorder->messages[3].data[1] == 123);

        inferencer.handleInputEvent(event(0xB5, 120, 0));
        CHECK(recorder->messages.size() == 5);
        CHECK(recorder->messages[4].data[1] == 120);

        inferencer.postprocess();
    } // the inferencer owns and deletes the recorder here

    if (failures == 0)
        std::printf("test_midi_input: OK\n");
    else
        std::printf("test_midi_input: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
