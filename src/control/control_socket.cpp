#include "control/control_socket.h"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace namzero {

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
    if (::listen(fd_, 1) != 0) {
        err = "listen " + path + ": " + std::strerror(errno);
        close();
        return false;
    }
    path_ = path;
    return true;
}

void ControlSocket::drop_client() {
    if (client_ >= 0) {
        ::close(client_);
        client_ = -1;
        buf_.clear();
    }
}

void ControlSocket::close() {
    drop_client();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        if (!path_.empty()) ::unlink(path_.c_str());
        path_.clear();
    }
}

void ControlSocket::poll(std::vector<std::string>& lines) {
    if (fd_ < 0) return;
    if (client_ < 0) {
        client_ = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_ < 0) return;
        buf_.clear();
    }
    char tmp[1024];
    for (;;) {
        const ssize_t n = ::recv(client_, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf_.append(tmp, static_cast<std::size_t>(n));
            std::size_t pos = 0;
            while ((pos = buf_.find('\n')) != std::string::npos) {
                std::string line = buf_.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf_.erase(0, pos + 1);
                if (!line.empty()) lines.push_back(std::move(line));
            }
            if (buf_.size() > 16384) buf_.clear();  // client rotto: non cresce
            continue;
        }
        if (n == 0) {
            drop_client();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        drop_client();
        return;
    }
}

void ControlSocket::send(const std::string& line) {
    if (client_ < 0) return;
    std::string out = line;
    out.push_back('\n');
    const ssize_t n = ::send(client_, out.data(), out.size(), MSG_NOSIGNAL);
    if (n < 0) drop_client();
}

}  // namespace namzero
