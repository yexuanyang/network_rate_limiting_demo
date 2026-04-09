#include "../include/rate_limiter.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

constexpr int BUFFER_SIZE = 65536;
constexpr int DEFAULT_PORT = 8080;
constexpr const char* DEFAULT_HOST = "127.0.0.1";
constexpr char FILE_PREFIX = '@';

static std::string format_size(std::size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = bytes;
    while (size >= 1024 && unit < 3) {
        size /= 1024;
        ++unit;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return oss.str();
}

static std::string format_speed(double bytes_per_sec) {
    return format_size(static_cast<std::size_t>(bytes_per_sec)) + "/s";
}

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

int main(int argc, char* argv[]) {
    std::string host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    std::string filename;
    std::string output;
    double initial_rate = 102400; // 100 KB/s

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--file" && i + 1 < argc) {
            filename = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--rate" && i + 1 < argc) {
            initial_rate = std::stod(argv[++i]);
        }
    }

    if (filename.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --file <filename> [--host host] [--port port] [--output output] [--rate bytes_per_sec]"
                  << std::endl;
        return 1;
    }

    if (output.empty()) {
        output = "downloaded_" + filename;
    }

    std::cout << "Connecting to " << host << ":" << port << std::endl;
    std::cout << "Requesting file: " << filename << std::endl;
    std::cout << "Client-side rate limit: " << format_speed(initial_rate) << std::endl;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket failed");
        return 1;
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << host << std::endl;
        return 1;
    }

    if (connect(sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("connect failed");
        return 1;
    }

    // 发送文件请求（客户端自行限速，无需告知服务端）
    std::string request = std::string(1, FILE_PREFIX) + filename + "\n";
    send_all(sock, request.c_str(), request.size());

    std::ofstream ofs(output, std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to create output file: " << output << std::endl;
        return 1;
    }

    // 客户端本地限流器
    auto limiter = std::make_shared<RateLimiter>(initial_rate);

    std::atomic<bool> running{true};
    std::atomic<std::size_t> total_received{0};
    auto start_time = std::chrono::steady_clock::now();

    // 下载线程：recv → 限速 → 写文件
    std::thread download_thread([&]() {
        char buffer[BUFFER_SIZE];
        auto last_update = std::chrono::steady_clock::now();

        while (running.load()) {
            ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
            if (n > 0) {
                // 客户端限速：消费令牌（不足则阻塞等待）
                limiter->consume(static_cast<std::size_t>(n));
                ofs.write(buffer, n);
                total_received.fetch_add(static_cast<std::size_t>(n), std::memory_order_relaxed);

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - last_update).count() >= 1.0) {
                    auto elapsed = std::chrono::duration<double>(now - start_time).count();
                    std::size_t received = total_received.load(std::memory_order_relaxed);
                    std::cout << "\rReceived: " << format_size(received)
                              << " | Speed: " << format_speed(received / elapsed)
                              << " | Limit: " << format_speed(limiter->rate())
                              << "    " << std::flush;
                    last_update = now;
                }
            } else if (n == 0 || (n < 0 && errno != EINTR)) {
                // 连接关闭或真实错误
                break;
            }
        }

        running.store(false);
        ofs.flush();

        auto end_time = std::chrono::steady_clock::now();
        double total_time = std::chrono::duration<double>(end_time - start_time).count();
        std::size_t received = total_received.load();

        std::cout << std::endl;
        std::cout << "Download complete!" << std::endl;
        std::cout << "  File:      " << output << std::endl;
        std::cout << "  Size:      " << format_size(received) << std::endl;
        std::cout << "  Time:      " << std::fixed << std::setprecision(2) << total_time << "s" << std::endl;
        if (total_time > 0) {
            std::cout << "  Avg speed: " << format_speed(received / total_time) << std::endl;
        }
    });

    std::cout << "Downloading... Commands: rate <bytes_per_sec>  |  quit" << std::endl;

    // 主线程：处理键盘输入，动态调整客户端限速
    while (running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 500000}; // 500ms 轮询，保证 running 标志能被及时感知

        int ret = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0 && errno != EINTR) break;
        if (ret <= 0) continue;

        std::string input;
        if (!std::getline(std::cin, input)) break;

        if (input.size() >= 5 && input.substr(0, 4) == "rate") {
            try {
                double new_rate = std::stod(input.substr(5));
                if (new_rate <= 0) throw std::invalid_argument("rate must be positive");
                limiter->set_rate(new_rate);
                std::cout << "\nClient rate limit -> " << format_speed(new_rate) << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "\nUsage: rate <bytes_per_second>  e.g. rate 51200 (50 KB/s)" << std::endl;
            }
        } else if (input == "quit" || input == "exit") {
            running.store(false);
            shutdown(sock, SHUT_RDWR); // 唤醒阻塞中的 recv
            break;
        } else if (!input.empty()) {
            std::cout << "\nCommands: rate <bytes_per_second>  |  quit" << std::endl;
        }
    }

    download_thread.join();
    close(sock);
    return 0;
}
