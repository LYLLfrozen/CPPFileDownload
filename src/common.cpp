#include "common.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <winsock2.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fd {

namespace {

bool socket_interrupted() {
#ifdef _WIN32
    const int err = WSAGetLastError();
    return err == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

}

bool send_all(fd_socket_t sockfd, const void* data, std::size_t len) {
    const auto* p = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < len) {
        const int n = ::send(sockfd, p + sent, static_cast<int>(len - sent), 0);
        if (n < 0) {
            if (socket_interrupted()) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_all(fd_socket_t sockfd, void* data, std::size_t len) {
    auto* p = static_cast<char*>(data);
    std::size_t recvd = 0;
    while (recvd < len) {
        const int n = ::recv(sockfd, p + recvd, static_cast<int>(len - recvd), 0);
        if (n < 0) {
            if (socket_interrupted()) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        recvd += static_cast<std::size_t>(n);
    }
    return true;
}

bool send_line(fd_socket_t sockfd, const std::string& line) {
    return send_all(sockfd, line.data(), line.size());
}

bool recv_line(fd_socket_t sockfd, std::string& line) {
    line.clear();
    char ch = '\0';
    while (true) {
        const int n = ::recv(sockfd, &ch, 1, 0);
        if (n < 0) {
            if (socket_interrupted()) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        line.push_back(ch);
        if (line.size() > 4096) {
            return false;
        }
    }
}

bool stream_file_to_socket(int file_fd, fd_socket_t sockfd, std::uint64_t size) {
    std::vector<char> buffer(kBufferSize);
    std::uint64_t remaining = size;

    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            remaining > buffer.size() ? buffer.size() : remaining);

        std::size_t total_read = 0;
        while (total_read < chunk) {
#ifdef _WIN32
            const int n = ::_read(file_fd, buffer.data() + total_read, static_cast<unsigned int>(chunk - total_read));
#else
            const ssize_t n = ::read(file_fd, buffer.data() + total_read, chunk - total_read);
#endif
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            total_read += static_cast<std::size_t>(n);
        }

        if (!send_all(sockfd, buffer.data(), chunk)) {
            return false;
        }
        remaining -= static_cast<std::uint64_t>(chunk);
    }

    return true;
}

bool stream_socket_to_file(fd_socket_t sockfd, int file_fd, std::uint64_t size) {
    std::vector<char> buffer(kBufferSize);
    std::uint64_t remaining = size;

    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            remaining > buffer.size() ? buffer.size() : remaining);

        if (!recv_all(sockfd, buffer.data(), chunk)) {
            return false;
        }

        std::size_t total_written = 0;
        while (total_written < chunk) {
#ifdef _WIN32
            const int n = ::_write(file_fd, buffer.data() + total_written, static_cast<unsigned int>(chunk - total_written));
#else
            const ssize_t n = ::write(file_fd, buffer.data() + total_written, chunk - total_written);
#endif
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            total_written += static_cast<std::size_t>(n);
        }

        remaining -= static_cast<std::uint64_t>(chunk);
    }

    return true;
}

int open_file_read(const char* path) {
#ifdef _WIN32
    return ::_open(path, _O_RDONLY | _O_BINARY);
#else
    return ::open(path, O_RDONLY);
#endif
}

int open_file_write_trunc(const char* path) {
#ifdef _WIN32
    return ::_open(path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return ::open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
#endif
}

bool close_file(int file_fd) {
#ifdef _WIN32
    return ::_close(file_fd) == 0;
#else
    return ::close(file_fd) == 0;
#endif
}

bool close_socket(fd_socket_t sockfd) {
#ifdef _WIN32
    return ::closesocket(sockfd) == 0;
#else
    return ::close(sockfd) == 0;
#endif
}

int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string sanitize_remote_path(const std::string& input) {
    std::string path = input;

    while (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        path.erase(path.begin());
    }

    if (path.empty()) {
        return {};
    }

    std::string normalized;
    normalized.reserve(path.size());
    for (char c : path) {
        normalized.push_back(c == '\\' ? '/' : c);
    }

    if (normalized.find("..") != std::string::npos) {
        return {};
    }

    return normalized;
}

}  // namespace fd
