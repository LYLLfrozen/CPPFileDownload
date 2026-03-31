#include "common.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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

void handle_client(fd_socket_t client_fd, const std::filesystem::path& root_dir) {
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
        const auto it_len = req.headers.find("content-length");
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

        const std::string file_name_raw = fd::sanitize_remote_path(it_name->second);
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

        const bool ok = fd::stream_socket_to_file(client_fd, file_fd, file_size);
        fd::close_file(file_fd);

        if (!ok) {
            send_plain_text(client_fd, 500, "Internal Server Error", "upload failed\n");
            fd::close_socket(client_fd);
            return;
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

        const bool ok = fd::stream_file_to_socket(file_fd, client_fd, file_size);
        fd::close_file(file_fd);
        if (!ok) {
            std::cerr << "download failed: " << final_path << '\n';
        }
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
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
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

    const int port = std::stoi(argv[1]);
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
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed\n";
        fd::close_socket(server_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

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

    std::cout << "HTTP server listening on port " << port << ", fixed storage dir: " << storage_dir << '\n';
    std::cout << "Open: http://127.0.0.1:" << port << "/\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const fd_socket_t client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
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

        std::thread(handle_client, client_fd, storage_dir).detach();
    }

    fd::close_socket(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
