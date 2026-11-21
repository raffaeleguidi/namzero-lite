#pragma once

#include <string>
#include <vector>

namespace namzero {

// Canale di controllo del motore: righe JSON su una socket Unix.
// Il motore e' il server; il servizio web si connette, manda comandi e riceve
// gli snapshot di stato (a ogni cambiamento). Un client per volta: e' il
// servizio web, non un utente.
class ControlSocket {
public:
    ControlSocket() = default;
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    bool listen(const std::string& path, std::string& err);
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool connected() const { return client_ >= 0; }

    // Righe complete arrivate dal client (senza '\n').
    void poll(std::vector<std::string>& lines);
    // Manda una riga (aggiunge '\n'); se nessuno e' connesso non fa nulla.
    void send(const std::string& line);

private:
    void drop_client();

    int fd_ = -1;
    int client_ = -1;
    std::string path_;
    std::string buf_;
};

}  // namespace namzero
