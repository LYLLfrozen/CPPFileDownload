#include "common.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace fd {

bool send_all(int sockfd, const void* data, std::size_t len) {
    const auto* p = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(sockfd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
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

bool recv_all(int sockfd, void* data, std::size_t len) {
    auto* p = static_cast<char*>(data);
    std::size_t recvd = 0;
    while (recvd < len) {
        const ssize_t n = ::recv(sockfd, p + recvd, len - recvd, 0);
        if (n < 0) {
            if (errno == EINTR) {
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

bool send_line(int sockfd, const std::string& line) {
    return send_all(sockfd, line.data(), line.size());
}

bool recv_line(int sockfd, std::string& line) {
    line.clear();
    char ch = '\0';
    while (true) {
        const ssize_t n = ::recv(sockfd, &ch, 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
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

bool stream_file_to_socket(int file_fd, int sockfd, std::uint64_t size) {
    std::vector<char> buffer(kBufferSize);
    std::uint64_t remaining = size;

    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            remaining > buffer.size() ? buffer.size() : remaining);

        std::size_t total_read = 0;
        while (total_read < chunk) {
            const ssize_t n = ::read(file_fd, buffer.data() + total_read, chunk - total_read);
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

bool stream_socket_to_file(int sockfd, int file_fd, std::uint64_t size) {
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
            const ssize_t n = ::write(file_fd, buffer.data() + total_written, chunk - total_written);
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
