#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "control/midi_socket.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

namespace {

int connect_to(const char* path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Il server e' non bloccante: si insiste finche' l'evento arriva.
bool wait_event(namzero::MidiSocket& socket, std::vector<namzero::MidiEvent>& events,
                std::size_t wanted) {
    for (int i = 0; i < 200 && events.size() < wanted; ++i) {
        socket.poll(events);
        if (events.size() >= wanted) break;
        ::usleep(1000);
    }
    return events.size() >= wanted;
}

}  // namespace

int main() {
    const char* path = "/tmp/namzero-midi-socket-test.sock";

    namzero::MidiSocket server;
    std::string err;
    CHECK(server.listen(path, err));
    CHECK(server.is_open());

    // Primo client: due CC in un colpo solo.
    {
        const int fd = connect_to(path);
        CHECK(fd >= 0);
        const uint8_t bytes[] = {0xB0, 20, 127, 22, 0};
        CHECK(::write(fd, bytes, sizeof(bytes)) == static_cast<ssize_t>(sizeof(bytes)));

        std::vector<namzero::MidiEvent> events;
        CHECK(wait_event(server, events, 2));
        CHECK(events.size() == 2);
        CHECK(events[0].cc == 20);
        CHECK(events[0].value == 127);
        CHECK(events[1].cc == 22);
        ::close(fd);
    }

    // Secondo client: la socket resta viva e il parser riparte pulito.
    {
        std::vector<namzero::MidiEvent> drain;
        for (int i = 0; i < 5; ++i) server.poll(drain);  // assorbe la chiusura

        const int fd = connect_to(path);
        CHECK(fd >= 0);
        const uint8_t bytes[] = {0x90, 60, 100};
        CHECK(::write(fd, bytes, sizeof(bytes)) == static_cast<ssize_t>(sizeof(bytes)));

        std::vector<namzero::MidiEvent> events;
        CHECK(wait_event(server, events, 1));
        CHECK(events.size() == 1);
        CHECK(events[0].cc == 60);
        ::close(fd);
    }

    server.close();
    CHECK(!server.is_open());
    CHECK(::access(path, F_OK) != 0);  // il file della socket e' stato rimosso

    if (g_fail != 0) {
        std::fprintf(stderr, "midi_socket: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("midi_socket: connessione, lettura, riconnessione e pulizia ok\n");
    return 0;
}
