#include "control/midi_socket.h"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace namzero {

MidiSocket::~MidiSocket() { close(); }

bool MidiSocket::listen(const std::string& path, std::string& err) {
    close();

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        err = "percorso socket troppo lungo: " + path;
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        err = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    // Un file rimasto da un'esecuzione precedente impedirebbe il bind. Si
    // rimuove solo se e' davvero una socket: mai cancellare un file qualsiasi.
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
        ::unlink(path.c_str());
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind " + path + ": " + std::strerror(errno);
        close();
        return false;
    }
    // Il bridge gira come utente diverso? No: stesso utente, ma 0666 evita
    // sorprese se un giorno lo si avvia da un servizio con gruppo diverso.
    ::chmod(path.c_str(), 0666);

    if (::listen(fd_, 1) != 0) {
        err = "listen " + path + ": " + std::strerror(errno);
        close();
        return false;
    }
    path_ = path;
    return true;
}

void MidiSocket::drop_client() {
    if (client_ >= 0) {
        ::close(client_);
        client_ = -1;
    }
}

void MidiSocket::close() {
    drop_client();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        if (!path_.empty()) ::unlink(path_.c_str());
        path_.clear();
    }
}

void MidiSocket::poll(std::vector<MidiEvent>& out) {
    if (fd_ < 0) return;

    if (client_ < 0) {
        // accept4: su Linux il socket accettato NON eredita O_NONBLOCK. Con
        // accept() il primo recv resterebbe bloccato e fermerebbe il thread MIDI.
        client_ = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_ < 0) return;  // EAGAIN: nessuno si e' connesso
        parser_.reset();
    }

    for (;;) {
        const ssize_t n = ::recv(client_, buf_, sizeof(buf_), 0);
        if (n > 0) {
            parser_.feed(buf_, static_cast<std::size_t>(n), out);
            continue;
        }
        if (n == 0) {
            drop_client();  // il bridge ha chiuso
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        drop_client();
        return;
    }
}

}  // namespace namzero
