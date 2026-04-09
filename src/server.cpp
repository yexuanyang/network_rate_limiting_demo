#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

constexpr int BUFFER_SIZE = 65536;
constexpr int DEFAULT_PORT = 8080;
constexpr char FILE_PREFIX = '@';

static void send_all(int sock, const void* data, std::size_t size) {
    std::size_t total_sent = 0;
    const char* ptr = static_cast<const char*>(data);
    while (total_sent < size) {
        ssize_t sent = send(sock, ptr + total_sent, size - total_sent, 0);
        if (sent <= 0) {
            throw std::runtime_error("send failed");
        }
        total_sent += sent;
    }
}

static void handle_client(int client_sock, const std::string& root_dir) {
    try {
        // 接收请求: @filename\n
        std::string buf;
        char ch;
        while (true) {
            ssize_t n = recv(client_sock, &ch, 1, 0);
            if (n <= 0) {
                close(client_sock);
                return;
            }
            if (ch == '\n') break;
            buf += ch;
        }

        if (buf.empty() || buf[0] != FILE_PREFIX) {
            std::cerr << "Invalid request: " << buf << std::endl;
            close(client_sock);
            return;
        }

        std::string filename = buf.substr(1);
        std::string filepath = root_dir + "/" + filename;
        std::cout << "Client requested: " << filename << std::endl;

        std::ifstream ifs(filepath, std::ios::binary);
        if (!ifs) {
            const char* err = "FILE_NOT_FOUND";
            send(client_sock, err, strlen(err), 0);
            close(client_sock);
            return;
        }

        // 全速发送文件，速率由客户端自行控制
        char buffer[BUFFER_SIZE];
        std::size_t total_sent = 0;
        while (ifs.read(buffer, sizeof(buffer)) || ifs.gcount() > 0) {
            std::size_t to_send = ifs.gcount();
            send_all(client_sock, buffer, to_send);
            total_sent += to_send;
        }

        std::cout << "Sent " << total_sent << " bytes to client" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    close(client_sock);
}

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    std::string root_dir = ".";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--root" && i + 1 < argc) {
            root_dir = argv[++i];
        }
    }

    std::cout << "Server starting on port " << port << ", root: " << root_dir << std::endl;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 8) < 0) { perror("listen"); return 1; }

    std::cout << "Listening..." << std::endl;

    while (true) {
        struct sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) { perror("accept"); continue; }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "Client connected: " << client_ip << std::endl;

        std::thread(handle_client, client_sock, root_dir).detach();
    }

    close(server_fd);
    return 0;
}
