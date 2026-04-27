#include "common.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>      // _read, _write (MinGW / MSVC)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>   // for getaddrinfo
#endif

// Retrieve the first non-loopback IPv4 address of the host.
std::string get_local_ip() {
#if defined(_WIN32)
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return "";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return "";
    std::string ip;
    for (addrinfo* p = res; p; p = p->ai_next) {
        sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(ipv4->sin_addr), ipstr, INET_ADDRSTRLEN);
        if (strcmp(ipstr, "127.0.0.1") != 0) { ip = ipstr; break; }
    }
    freeaddrinfo(res);
    return ip;
#else
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return "";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return "";
    std::string ip;
    for (addrinfo* p = res; p; p = p->ai_next) {
        sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(ipv4->sin_addr), ipstr, INET_ADDRSTRLEN);
        if (strcmp(ipstr, "127.0.0.1") != 0) { ip = ipstr; break; }
    }
    freeaddrinfo(res);
    return ip;
#endif
}

void log(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[20];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    std::cerr << "[" << buf << "] " << msg << std::endl;
}

namespace {

struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;
};

std::string to_lower(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

bool recv_http_line(fd_socket_t fd, std::string& line) {
    line.clear();
    char ch = '\0';
    while (true) {
        const int n = ::recv(fd, &ch, 1, 0);
        if (n < 0) {
            if (fd::last_socket_error() ==
#ifdef _WIN32
                WSAEINTR
#else
                EINTR
#endif
            ) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        line.push_back(ch);
        if (line.size() > 8192) {
            return false;
        }
    }
}

bool read_http_request(fd_socket_t fd, HttpRequest& req) {
    std::string line;
    if (!recv_http_line(fd, line)) {
        return false;
    }

    std::istringstream request_line(line);
    std::string version;
    request_line >> req.method >> req.target >> version;
    if (req.method.empty() || req.target.empty() || version.rfind("HTTP/", 0) != 0) {
        return false;
    }

    req.headers.clear();
    while (true) {
        if (!recv_http_line(fd, line)) {
            return false;
        }
        if (line.empty()) {
            break;
        }
        const std::size_t sep = line.find(':');
        if (sep == std::string::npos) {
            return false;
        }
        const std::string key = to_lower(trim(line.substr(0, sep)));
        const std::string value = trim(line.substr(sep + 1));
        req.headers[key] = value;
    }
    return true;
}

bool send_http_response_head(fd_socket_t fd,
                             int status_code,
                             const std::string& status_text,
                             const std::vector<std::pair<std::string, std::string>>& headers) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n";
    for (const auto& h : headers) {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "Connection: close\r\n\r\n";
    return fd::send_all(fd, oss.str().data(), oss.str().size());
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            std::istringstream hex(s.substr(i + 1, 2));
            int value = 0;
            if (hex >> std::hex >> value) {
                out.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string get_query_param(const std::string& target, const std::string& key) {
    const std::size_t q = target.find('?');
    if (q == std::string::npos || q + 1 >= target.size()) {
        return {};
    }
    const std::string query = target.substr(q + 1);
    std::size_t pos = 0;
    while (pos < query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            const std::string k = url_decode(pair.substr(0, eq));
            if (k == key) {
                return url_decode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return {};
}

std::string base_target_path(const std::string& target) {
    const std::size_t q = target.find('?');
    return q == std::string::npos ? target : target.substr(0, q);
}

std::string load_file_text(const std::filesystem::path& file) {
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string build_files_json(const std::filesystem::path& root_dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        names.push_back(entry.path().filename().string());
    }

    std::ostringstream oss;
    oss << "{\"files\":[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << '"' << json_escape(names[i]) << '"';
    }
    oss << "]}";
    return oss.str();
}

bool send_plain_text(fd_socket_t fd, int code, const std::string& status, const std::string& body) {
    if (!send_http_response_head(fd,
                                 code,
                                 status,
                                 {{"Content-Type", "text/plain; charset=utf-8"},
                                  {"Content-Length", std::to_string(body.size())}})) {
        return false;
    }
    return fd::send_all(fd, body.data(), body.size());
}

// Helper: format bytes as human-readable string (B / KB / MB / GB).
std::string fmt_bytes(std::uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes < 1024ULL) {
        oss << bytes << " B";
    } else if (bytes < 1024ULL * 1024) {
        oss << static_cast<double>(bytes) / 1024.0 << " KB";
    } else if (bytes < 1024ULL * 1024 * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024) << " MB";
    } else {
        oss << static_cast<double>(bytes) / (1024.0 * 1024 * 1024) << " GB";
    }
    return oss.str();
}

// Helper: format speed in KB/s or MB/s.
std::string fmt_speed(double bytes_per_sec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes_per_sec < 1024.0 * 1024) {
        oss << bytes_per_sec / 1024.0 << " KB/s";
    } else {
        oss << bytes_per_sec / (1024.0 * 1024) << " MB/s";
    }
    return oss.str();
}

void handle_client(fd_socket_t client_fd, const std::filesystem::path& root_dir, const std::string& client_ip) {
    HttpRequest req;
    if (!read_http_request(client_fd, req)) {
        fd::close_socket(client_fd);
        return;
    }

    const std::string route = base_target_path(req.target);

    if (req.method == "GET" && route == "/") {
        const std::filesystem::path index_html = std::filesystem::path(FD_WEB_ROOT) / "index.html";
        const std::string html = load_file_text(index_html);
        if (html.empty()) {
            send_plain_text(client_fd, 500, "Internal Server Error", "front-end file missing\n");
            fd::close_socket(client_fd);
            return;
        }
        if (!send_http_response_head(client_fd,
                                     200,
                                     "OK",
                                     {{"Content-Type", "text/html; charset=utf-8"},
                                      {"Content-Length", std::to_string(html.size())}})) {
            fd::close_socket(client_fd);
            return;
        }
        fd::send_all(client_fd, html.data(), html.size());

    } else if (req.method == "POST" && route == "/api/upload") {
        const auto it_len  = req.headers.find("content-length");
        const auto it_name = req.headers.find("x-file-name");
        if (it_len == req.headers.end() || it_name == req.headers.end()) {
            send_plain_text(client_fd, 400, "Bad Request", "missing content-length or x-file-name\n");
            fd::close_socket(client_fd);
            return;
        }

        std::uint64_t file_size = 0;
        try {
            file_size = static_cast<std::uint64_t>(std::stoull(it_len->second));
        } catch (...) {
            send_plain_text(client_fd, 400, "Bad Request", "invalid content-length\n");
            fd::close_socket(client_fd);
            return;
        }

        // Header value was percent-encoded on the client side; decode it first.
        const std::string decoded_name = url_decode(it_name->second);
        const std::string file_name_raw = fd::sanitize_remote_path(decoded_name);
        const std::string file_name = std::filesystem::path(file_name_raw).filename().string();
        if (file_name.empty()) {
            send_plain_text(client_fd, 400, "Bad Request", "invalid file name\n");
            fd::close_socket(client_fd);
            return;
        }

        const std::filesystem::path final_path = root_dir / file_name;

        const int file_fd = fd::open_file_write_trunc(final_path.string().c_str());
        if (file_fd < 0) {
            send_plain_text(client_fd, 500, "Internal Server Error", "cannot open destination file\n");
            fd::close_socket(client_fd);
            return;
        }

        log("upload   START  [" + client_ip + "] \"" + file_name + "\"  total=" + fmt_bytes(file_size));

        // Receive data from socket and write to file, logging progress every second.
        {
            std::vector<char> up_buf(fd::kBufferSize);
            std::uint64_t up_remaining = file_size;
            std::uint64_t up_received  = 0;
            auto up_start    = std::chrono::steady_clock::now();
            auto up_last_log = up_start;
            bool up_ok       = true;

            while (up_remaining > 0 && up_ok) {
                const std::size_t chunk = std::min(
                    static_cast<std::size_t>(up_buf.size()),
                    static_cast<std::size_t>(up_remaining));
                const int n = ::recv(client_fd, up_buf.data(), static_cast<int>(chunk), 0);
                if (n < 0) {
#ifdef _WIN32
                    if (fd::last_socket_error() == WSAEINTR) continue;
#else
                    if (errno == EINTR) continue;
#endif
                    up_ok = false;
                    break;
                }
                if (n == 0) { up_ok = false; break; }

                // Write received bytes to file.
                std::size_t written = 0;
                const std::size_t to_write = static_cast<std::size_t>(n);
                while (written < to_write) {
#ifdef _WIN32
                    const int w = ::_write(file_fd,
                        up_buf.data() + written,
                        static_cast<unsigned int>(to_write - written));
#else
                    const ssize_t w = ::write(file_fd,
                        up_buf.data() + written,
                        to_write - written);
#endif
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        up_ok = false;
                        break;
                    }
                    written += static_cast<std::size_t>(w);
                }
                if (!up_ok) break;

                up_received  += to_write;
                up_remaining -= to_write;

                auto now = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - up_start).count();
                auto since_log  = std::chrono::duration_cast<std::chrono::milliseconds>(now - up_last_log).count();
                if (since_log >= 1000) {
                    // Avoid division by zero: elapsed_ms is always > 0 here (since_log >= 1000).
                    const double speed = static_cast<double>(up_received) * 1000.0
                                         / static_cast<double>(elapsed_ms);
                    const int pct = file_size > 0
                        ? static_cast<int>(up_received * 100 / file_size)
                        : 0;
                    std::ostringstream oss;
                    oss << "upload   PROG   [" << client_ip << "] \"" << file_name << "\""
                        << "  " << fmt_bytes(up_received) << " / " << fmt_bytes(file_size)
                        << "  (" << pct << "%)"
                        << "  " << fmt_speed(speed);
                    log(oss.str());
                    up_last_log = now;
                }
            }

            fd::close_file(file_fd);

            const auto end      = std::chrono::steady_clock::now();
            const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - up_start).count();
            const double avg_speed = total_ms > 0
                ? static_cast<double>(up_received) * 1000.0 / static_cast<double>(total_ms)
                : 0.0;

            if (!up_ok) {
                log("upload   FAIL   [" + client_ip + "] \"" + file_name + "\""
                    + "  received=" + fmt_bytes(up_received));
                send_plain_text(client_fd, 500, "Internal Server Error", "upload failed\n");
                fd::close_socket(client_fd);
                return;
            }

            std::ostringstream done_oss;
            done_oss << "upload   DONE   [" << client_ip << "] \"" << file_name << "\""
                     << "  " << fmt_bytes(up_received)
                     << "  avg=" << fmt_speed(avg_speed)
                     << "  time=" << total_ms << " ms";
            log(done_oss.str());
        }

        send_plain_text(client_fd, 200, "OK", "upload success: " + file_name + "\n");

    } else if (req.method == "GET" && route == "/api/download") {
        const std::string file_name_raw = fd::sanitize_remote_path(get_query_param(req.target, "name"));
        const std::string file_name = std::filesystem::path(file_name_raw).filename().string();
        if (file_name.empty()) {
            send_plain_text(client_fd, 400, "Bad Request", "invalid file name\n");
            fd::close_socket(client_fd);
            return;
        }

        const std::filesystem::path final_path = root_dir / file_name;
        std::error_code ec;
        const std::uint64_t file_size = static_cast<std::uint64_t>(std::filesystem::file_size(final_path, ec));
        if (ec) {
            send_plain_text(client_fd, 404, "Not Found", "file not found\n");
            fd::close_socket(client_fd);
            return;
        }

        const int file_fd = fd::open_file_read(final_path.string().c_str());
        if (file_fd < 0) {
            send_plain_text(client_fd, 500, "Internal Server Error", "cannot open source file\n");
            fd::close_socket(client_fd);
            return;
        }

        if (!send_http_response_head(client_fd,
                                     200,
                                     "OK",
                                     {{"Content-Type", "application/octet-stream"},
                                      {"Content-Length", std::to_string(file_size)},
                                      {"Content-Disposition", "attachment; filename=\"" + file_name + "\""}})) {
            fd::close_file(file_fd);
            fd::close_socket(client_fd);
            return;
        }

        log("download START  [" + client_ip + "] \"" + file_name + "\"  total=" + fmt_bytes(file_size));

        // Stream file to socket with per-second progress logging.
        {
            std::vector<char> buffer(fd::kBufferSize);
            std::uint64_t remaining = file_size;
            std::uint64_t sent      = 0;
            auto start_time  = std::chrono::steady_clock::now();
            auto last_log    = start_time;
            bool ok          = true;

            while (remaining > 0 && ok) {
                const std::size_t chunk = std::min(
                    static_cast<std::size_t>(buffer.size()),
                    static_cast<std::size_t>(remaining));

                // Read exactly `chunk` bytes from file.
                std::size_t total_read = 0;
                while (total_read < chunk) {
#ifdef _WIN32
                    const int n = ::_read(file_fd,
                        buffer.data() + total_read,
                        static_cast<unsigned int>(chunk - total_read));
#else
                    const ssize_t n = ::read(file_fd,
                        buffer.data() + total_read,
                        chunk - total_read);
#endif
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        ok = false;
                        break;
                    }
                    if (n == 0) { ok = false; break; }   // Unexpected EOF
                    total_read += static_cast<std::size_t>(n);
                }
                if (!ok) break;

                // BUG FIX: send `total_read` bytes actually read, not `chunk`.
                // (they are equal in normal conditions, but must be correct on short reads)
                if (!fd::send_all(client_fd, buffer.data(), total_read)) {
                    ok = false;
                    break;
                }

                sent      += total_read;
                remaining -= total_read;

                auto now        = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                auto since_log  = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();

                if (since_log >= 1000) {
                    // BUG FIX: use milliseconds to avoid division-by-zero when elapsed < 1 s.
                    const double speed = elapsed_ms > 0
                        ? static_cast<double>(sent) * 1000.0 / static_cast<double>(elapsed_ms)
                        : 0.0;
                    const int pct = file_size > 0
                        ? static_cast<int>(sent * 100 / file_size)
                        : 0;
                    std::ostringstream oss;
                    oss << "download PROG   [" << client_ip << "] \"" << file_name << "\""
                        << "  " << fmt_bytes(sent) << " / " << fmt_bytes(file_size)
                        << "  (" << pct << "%)"
                        << "  " << fmt_speed(speed);
                    log(oss.str());
                    last_log = now;
                }
            }

            const auto end      = std::chrono::steady_clock::now();
            const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();
            const double avg_speed = total_ms > 0
                ? static_cast<double>(sent) * 1000.0 / static_cast<double>(total_ms)
                : 0.0;

            if (!ok) {
                log("download FAIL   [" + client_ip + "] \"" + file_name + "\""
                    + "  sent=" + fmt_bytes(sent));
            } else {
                std::ostringstream oss;
                oss << "download DONE   [" << client_ip << "] \"" << file_name << "\""
                    << "  " << fmt_bytes(sent)
                    << "  avg=" << fmt_speed(avg_speed)
                    << "  time=" << total_ms << " ms";
                log(oss.str());
            }
        }

        fd::close_file(file_fd);

    } else if (req.method == "GET" && route == "/api/files") {
        const std::string body = build_files_json(root_dir);
        if (!send_http_response_head(client_fd,
                                     200,
                                     "OK",
                                     {{"Content-Type", "application/json; charset=utf-8"},
                                      {"Content-Length", std::to_string(body.size())}})) {
            fd::close_socket(client_fd);
            return;
        }
        fd::send_all(client_fd, body.data(), body.size());
    } else {
        send_plain_text(client_fd, 404, "Not Found", "unsupported route\n");
    }

    fd::close_socket(client_fd);
}

}  // namespace

int main(int argc, char** argv) {
    int port;
    if (argc == 1) {
        port = 9000;
    } else if (argc == 2) {
        port = std::stoi(argv[1]);
    } else {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup() failed\n";
        return 1;
    }
#endif

    const std::filesystem::path storage_dir = std::filesystem::current_path() / "upload";
    std::error_code ec;
    std::filesystem::create_directories(storage_dir, ec);
    if (ec) {
        std::cerr << "Cannot create storage directory: " << storage_dir << "\n";
        return 1;
    }

    const fd_socket_t server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (server_fd == INVALID_SOCKET) {
#else
    if (server_fd < 0) {
#endif
        std::cerr << "socket() failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    int reuse = 1;
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed\n";
        fd::close_socket(server_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind() failed, errno=" << fd::last_socket_error() << "\n";
        fd::close_socket(server_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (::listen(server_fd, 128) != 0) {
        std::cerr << "listen() failed\n";
        fd::close_socket(server_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "HTTP server listening on port " << port
              << ", fixed storage dir: " << storage_dir << '\n';
    std::string lan_ip = get_local_ip();
    if (!lan_ip.empty()) {
        std::cout << "LAN: http://" << lan_ip << ":" << port << "/\n";
    }
    std::cout << "Open: http://127.0.0.1:" << port << "/\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const fd_socket_t client_fd = ::accept(
            server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
#ifdef _WIN32
        if (client_fd == INVALID_SOCKET) {
#else
        if (client_fd < 0) {
#endif
            if (fd::last_socket_error() ==
#ifdef _WIN32
                WSAEINTR
#else
                EINTR
#endif
            ) {
                continue;
            }
            std::cerr << "accept() failed, errno=" << fd::last_socket_error() << '\n';
            continue;
        }

        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip_str, INET_ADDRSTRLEN);
        std::string client_ip(client_ip_str);
        std::thread(handle_client, client_fd, storage_dir, client_ip).detach();
    }

    fd::close_socket(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}