#include <cstdio>
#include <vector>

#include "control/midi_parser.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

int main() {
    namzero::MidiParser parser;
    std::vector<namzero::MidiEvent> events;

    // CC completo.
    {
        const uint8_t bytes[] = {0xB0, 20, 127};
        parser.feed(bytes, sizeof(bytes), events);
        CHECK(events.size() == 1);
        CHECK(events[0].cc == 20);
        CHECK(events[0].value == 127);
    }

    // Running status: il secondo CC non ripete lo status.
    {
        events.clear();
        const uint8_t bytes[] = {0xB0, 21, 0, 22, 64};
        parser.feed(bytes, sizeof(bytes), events);
        CHECK(events.size() == 2);
        CHECK(events[1].cc == 22);
        CHECK(events[1].value == 64);
    }

    // Nota e program change (un solo dato).
    {
        events.clear();
        const uint8_t bytes[] = {0x90, 60, 100, 0xC2, 7};
        parser.feed(bytes, sizeof(bytes), events);
        CHECK(events.size() == 2);
        CHECK(events[0].cc == 60);
        CHECK(events[1].cc == 7);
        CHECK(events[1].status == 0xC2);
    }

    // Il clock realtime non rompe lo stato.
    {
        events.clear();
        const uint8_t bytes[] = {0xF8, 0xB0, 10, 1, 0xF8};
        parser.feed(bytes, sizeof(bytes), events);
        CHECK(events.size() == 1);
        CHECK(events[0].cc == 10);
    }

    // Un messaggio incompleto non viene consegnato.
    {
        events.clear();
        const uint8_t bytes[] = {0xB0, 25};
        parser.feed(bytes, sizeof(bytes), events);
        CHECK(events.empty());
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "midi_parser: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("midi_parser: cc, running status, note e clock ok\n");
    return 0;
}
