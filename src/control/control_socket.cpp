#include "control/control_socket.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace namzero {

namespace {
// Un client rotto non deve poter esaurire i descrittori del motore: oltre
// questi limiti lo si chiude invece di accumulare memoria.
constexpr std::size_t kMaxClients = 128;
constexpr std::size_t kMaxIn = 64 * 1024;
constexpr std::size_t kMaxOut = 1024 * 1024;
}  // namespace

ControlSocket::~ControlSocket() { close(); }

bool ControlSocket::listen(const std::string& path, std::string& err) {
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
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
        ::unlink(path.c_str());
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind " + path + ": " + std::strerror(errno);
        close();
        return false;
    }
    ::chmod(path.c_str(), 0666);
    if (::listen(fd_, 64) != 0) {
        err = "listen " + path + ": " + std::strerror(errno);
        close();
        return false;
    }
    path_ = path;
    return true;
}

ControlSocket::Client* ControlSocket::find(int id) {
    for (auto& c : clients_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

void ControlSocket::drop(std::size_t index) {
    if (index >= clients_.size()) return;
    if (clients_[index].fd >= 0) ::close(clients_[index].fd);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

void ControlSocket::close() {
    for (auto& c : clients_) {
        if (c.fd >= 0) ::close(c.fd);
    }
    clients_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        if (!path_.empty()) ::unlink(path_.c_str());
        path_.clear();
    }
}

bool ControlSocket::flush(Client& c) {
    while (!c.out.empty()) {
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    return true;
}

bool ControlSocket::queue(Client& c, const std::string& line) {
    if (c.out.size() + line.size() + 1 > kMaxOut) return false;
    c.out += line;
    c.out.push_back('\n');
    // c.last non si aggiorna qui: un client che riceve i broadcast ma non manda
    // piu' comandi resta "inattivo" e va chiuso dopo idle_ms. Altrimenti la web
    // UI aperta (che fa polling) terrebbe in vita per sempre ogni connessione
    // appesa, e il timeout non scatterebbe mai.
    return true;
}

void ControlSocket::poll(std::vector<CtlLine>& lines, int idle_ms) {
    if (fd_ < 0) return;

    // Tutte le connessioni pendenti: il web ne apre una per richiesta e durante
    // il re-exec si accavallano. Accettarne una sola per poll riempirebbe il
    // backlog (64) e farebbe sembrare il motore irraggiungibile.
    for (;;) {
        const int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;  // EAGAIN/EWOULDBLOCK: non c'e' altro, oppure errore
        }
        if (clients_.size() >= kMaxClients) {
            ::close(cfd);
            continue;
        }
        Client c;
        c.fd = cfd;
        c.id = next_id_++;
        c.last = std::chrono::steady_clock::now();
        clients_.push_back(std::move(c));
        ++accepted_total_;
    }

    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        bool alive = flush(c);

        char tmp[4096];
        while (alive) {
            const ssize_t n = ::recv(c.fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                c.in.append(tmp, static_cast<std::size_t>(n));
                c.last = now;
                std::size_t pos = 0;
                while ((pos = c.in.find('\n')) != std::string::npos) {
                    std::string line = c.in.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    c.in.erase(0, pos + 1);
                    if (!line.empty()) lines.push_back(CtlLine{c.id, std::move(line)});
                }
                if (c.in.size() > kMaxIn) c.in.clear();  // client rotto: non cresce
                continue;
            }
            if (n == 0) {
                alive = false;  // EOF: il client ha chiuso
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            alive = false;
            break;
        }

        if (alive && idle_ms > 0) {
            const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.last);
            if (idle.count() > idle_ms) alive = false;  // appeso: non tenerlo
        }
        if (!alive) {
            drop(i);
            continue;
        }
        ++i;
    }
}

void ControlSocket::send(int client, const std::string& line) {
    Client* c = find(client);
    if (c == nullptr) return;
    if (!queue(*c, line) || !flush(*c)) {
        // find() non da' l'indice: si rilancia la ricerca per chiudere.
        for (std::size_t i = 0; i < clients_.size(); ++i) {
            if (clients_[i].id == client) {
                drop(i);
                break;
            }
        }
    }
}

void ControlSocket::broadcast(const std::string& line) {
    for (std::size_t i = 0; i < clients_.size();) {
        if (!queue(clients_[i], line) || !flush(clients_[i])) {
            drop(i);
            continue;
        }
        ++i;
    }
}

}  // namespace namzero
