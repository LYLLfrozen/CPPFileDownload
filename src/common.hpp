#pragma once

#include <cstdint>
#include <string>

namespace fd {

constexpr std::size_t kBufferSize = 8 * 1024 * 1024;  // 8 MiB

bool send_all(int sockfd, const void* data, std::size_t len);
bool recv_all(int sockfd, void* data, std::size_t len);
bool send_line(int sockfd, const std::string& line);
bool recv_line(int sockfd, std::string& line);

bool stream_file_to_socket(int file_fd, int sockfd, std::uint64_t size);
bool stream_socket_to_file(int sockfd, int file_fd, std::uint64_t size);

std::string sanitize_remote_path(const std::string& input);

}  // namespace fd
