#include "../include/rate_limiter.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

constexpr int BUFFER_SIZE = 65536;
constexpr int DEFAULT_PORT = 8080;
constexpr char FILE_PREFIX = '@';
constexpr int DEFAULT_MAX_CLIENTS = 64;
constexpr int LISTEN_BACKLOG = 128;

// 全局活跃连接计数（原子，所有线程共享）
static std::atomic<int> g_active_connections{0};

// 连接退出时自动递减的 RAII guard
struct ConnectionGuard {
    ~ConnectionGuard() { g_active_connections.fetch_sub(1, std::memory_order_relaxed); }
};

static void send_all(int sock, const void* data, std::size_t size) {
    std::size_t sent = 0;
    const char* ptr = static_cast<const char*>(data);
    while (sent < size) {
        ssize_t n = send(sock, ptr + sent, size - sent, 0);
        if (n <= 0) throw std::runtime_error("send failed");
        sent += n;
    }
}

static void handle_client(int client_sock,
                          const std::string& root_dir,
                          std::shared_ptr<RateLimiter> global_limiter,
                          double per_conn_rate) {
    ConnectionGuard guard;  // 退出时自动 g_active_connections--
    try {
        // 接收请求: @filename\n
        std::string buf;
        char ch;
        while (true) {
            ssize_t n = recv(client_sock, &ch, 1, 0);
            if (n <= 0) { close(client_sock); return; }
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

        // 每连接独立限速器
        RateLimiter per_conn_limiter(per_conn_rate);

        char buffer[BUFFER_SIZE];
        std::size_t total_sent = 0;
        while (ifs.read(buffer, sizeof(buffer)) || ifs.gcount() > 0) {
            std::size_t to_send = ifs.gcount();

            // 先消耗每连接令牌（保证单连接不超限）
            per_conn_limiter.consume(to_send);

            // 再消耗全局令牌（保证所有连接聚合不超限）
            // nullptr 表示不设全局限速
            if (global_limiter) global_limiter->consume(to_send);

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
    int max_clients = DEFAULT_MAX_CLIENTS;
    double global_rate = 0;       // 0 = 不限全局速率
    double per_conn_rate = 0;     // 0 = 不限单连接速率（设为极大值）
    std::string root_dir = ".";
    std::string iface;            // 绑定特定 NIC（空 = 不限）
    bool reuseport = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--root" && i + 1 < argc) {
            root_dir = argv[++i];
        } else if (arg == "--max-clients" && i + 1 < argc) {
            max_clients = std::stoi(argv[++i]);
        } else if (arg == "--global-rate" && i + 1 < argc) {
            global_rate = std::stod(argv[++i]);
        } else if (arg == "--per-conn-rate" && i + 1 < argc) {
            per_conn_rate = std::stod(argv[++i]);
        } else if (arg == "--iface" && i + 1 < argc) {
            iface = argv[++i];
        } else if (arg == "--reuseport") {
            reuseport = true;
        }
    }

    // 速率为 0 时设为极大值（不限速）
    if (per_conn_rate <= 0) per_conn_rate = 1e18;

    std::shared_ptr<RateLimiter> global_limiter;
    if (global_rate > 0) global_limiter = std::make_shared<RateLimiter>(global_rate);

    std::cout << "Server starting on port " << port << ", root: " << root_dir << "\n"
              << "  max-clients:    " << max_clients << "\n"
              << "  global-rate:    " << (global_rate > 0 ? std::to_string(global_rate / 1024) + " KB/s" : "unlimited") << "\n"
              << "  per-conn-rate:  " << (per_conn_rate < 1e17 ? std::to_string(per_conn_rate / 1024) + " KB/s" : "unlimited") << "\n"
              << "  iface:          " << (iface.empty() ? "any" : iface) << "\n"
              << "  reuseport:      " << (reuseport ? "yes" : "no") << std::endl;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // SO_REUSEPORT：支持多进程共享同一端口，内核按连接哈希分配
    if (reuseport) {
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            perror("SO_REUSEPORT");
            return 1;
        }
    }

    // SO_BINDTODEVICE：绑定到特定 NIC
    if (!iface.empty()) {
        if (setsockopt(server_fd, SOL_SOCKET, SO_BINDTODEVICE,
                       iface.c_str(), iface.size() + 1) < 0) {
            perror("SO_BINDTODEVICE");
            return 1;
        }
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, LISTEN_BACKLOG) < 0) { perror("listen"); return 1; }

    std::cout << "Listening..." << std::endl;

    while (true) {
        struct sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) { perror("accept"); continue; }

        // 接入控制：超过最大连接数则拒绝
        int cur = g_active_connections.load(std::memory_order_relaxed);
        if (cur >= max_clients) {
            const char* msg = "SERVER_BUSY\n";
            send(client_sock, msg, strlen(msg), 0);
            close(client_sock);
            std::cerr << "Connection rejected (limit " << max_clients << " reached)" << std::endl;
            continue;
        }
        g_active_connections.fetch_add(1, std::memory_order_relaxed);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "Client connected: " << client_ip
                  << " (active: " << g_active_connections.load() << ")" << std::endl;

        std::thread(handle_client, client_sock, root_dir, global_limiter, per_conn_rate).detach();
    }

    close(server_fd);
    return 0;
}
