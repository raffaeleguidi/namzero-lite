#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace namzero {

// Riga completa arrivata da un client, con l'id della connessione.
struct CtlLine {
    int client = -1;
    std::string line;
};

// Canale di controllo del motore: righe JSON su una socket Unix.
// Il motore e' il server; la web UI apre una connessione per richiesta,
// manda un comando e legge lo stato. Piu' client possono essere connessi
// insieme: se ne accettano tutte le connessioni pendenti a ogni poll, si legge
// da ognuno senza bloccarsi e lo stato va in broadcast. Un client lento non
// ferma gli altri (l'uscita e' bufferizzata e, se non smaltita, lo si chiude).
class ControlSocket {
public:
    ControlSocket() = default;
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    bool listen(const std::string& path, std::string& err);
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool connected() const { return !clients_.empty(); }
    std::size_t client_count() const { return clients_.size(); }
    // Connessioni accettate da sempre: il thread ctl la usa per accorgersi
    // anche di un client entrato e uscito fra due poll (nuovo stato da mandare).
    std::uint64_t accepted_total() const { return accepted_total_; }

    // Accetta tutte le connessioni pendenti, legge da ogni client senza
    // bloccarsi, chiude EOF/errori e i client inattivi da piu' di idle_ms.
    void poll(std::vector<CtlLine>& lines, int idle_ms = 10000);
    // Manda una riga (aggiunge '\n'). send() a un client sparito non fa nulla.
    void send(int client, const std::string& line);
    void broadcast(const std::string& line);

private:
    struct Client {
        int fd = -1;
        int id = -1;
        std::string in;
        std::string out;
        // Ultimo comando ricevuto dal client (non l'ultimo invio): il timeout
        // chiude chi non manda piu' niente, anche se sta leggendo i broadcast.
        std::chrono::steady_clock::time_point last;
    };

    Client* find(int id);
    void drop(std::size_t index);
    bool flush(Client& c);
    bool queue(Client& c, const std::string& line);

    int fd_ = -1;
    int next_id_ = 1;
    std::uint64_t accepted_total_ = 0;
    std::string path_;
    std::vector<Client> clients_;
};

}  // namespace namzero
