#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
using fd_socket_t = SOCKET;
#else
using fd_socket_t = int;
#endif

namespace fd {

constexpr std::size_t kBufferSize = 8 * 1024 * 1024;  // 8 MiB

bool send_all(fd_socket_t sockfd, const void* data, std::size_t len);
bool recv_all(fd_socket_t sockfd, void* data, std::size_t len);
bool send_line(fd_socket_t sockfd, const std::string& line);
bool recv_line(fd_socket_t sockfd, std::string& line);

bool stream_file_to_socket(int file_fd, fd_socket_t sockfd, std::uint64_t size);
bool stream_socket_to_file(fd_socket_t sockfd, int file_fd, std::uint64_t size);

int open_file_read(const char* path);
int open_file_write_trunc(const char* path);
bool close_file(int file_fd);
bool close_socket(fd_socket_t sockfd);
int last_socket_error();

std::string sanitize_remote_path(const std::string& input);

}  // namespace fd
