// 测试要求：
//   - 只使用 loopback (127.0.0.1)，不修改任何网络配置
//   - 测试文件写入 /tmp 临时目录，运行结束后清理
//   - 测试端口 18080-18084（固定，远低于系统 ephemeral range 32768-60999）

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

constexpr char FILE_PREFIX = '@';
constexpr const char* TEST_HOST = "127.0.0.1";

// ─── 工具函数 ────────────────────────────────────────────────────────────────

// 生成内容可验证的测试文件：第 i 字节 = i % 256
static void create_test_file(const std::string& path, std::size_t size) {
    std::ofstream ofs(path, std::ios::binary);
    constexpr std::size_t CHUNK = 65536;
    std::vector<unsigned char> buf(CHUNK);
    for (std::size_t i = 0; i < CHUNK; ++i) buf[i] = i & 0xFF;
    std::size_t written = 0;
    while (written < size) {
        // 每个 chunk 的起始偏移量影响字节值
        std::size_t off = written % 256;
        for (std::size_t i = 0; i < CHUNK; ++i) buf[i] = (off + i) & 0xFF;
        std::size_t n = std::min(CHUNK, size - written);
        ofs.write(reinterpret_cast<char*>(buf.data()), n);
        written += n;
    }
}

// 验证文件内容与 create_test_file 模式一致
static bool verify_file(const std::string& path, std::size_t expected_size) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::vector<unsigned char> buf(65536);
    std::size_t total = 0;
    while (true) {
        ifs.read(reinterpret_cast<char*>(buf.data()), buf.size());
        std::size_t n = ifs.gcount();
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] != ((total + i) & 0xFF)) return false;  // 注意括号：& 优先级低于 !=
        }
        total += n;
    }
    return total == expected_size;
}

// ─── 服务端进程管理 ──────────────────────────────────────────────────────────

struct Server {
    pid_t pid = -1;
    int port = 0;

    // 启动服务端子进程
    static Server start(int port, const std::string& bin, const std::string& root,
                        const std::vector<std::string>& extra = {}) {
        Server s;
        s.port = port;
        s.pid = fork();
        if (s.pid == 0) {
            // 子进程：静默，exec 服务端
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
            std::string port_str = std::to_string(port);
            std::vector<const char*> args = {bin.c_str(), "--port", port_str.c_str(),
                                              "--root", root.c_str()};
            // extra 参数必须在调用期间保持存活
            for (const auto& a : extra) args.push_back(a.c_str());
            args.push_back(nullptr);
            execv(bin.c_str(), const_cast<char**>(args.data()));
            _exit(1);
        }
        s.wait_ready();
        return s;
    }

    // 轮询直到服务端开始接受连接
    void wait_ready(int timeout_ms = 3000) const {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
            bool ok = (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            close(sock);
            if (ok) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error("server did not start (port " + std::to_string(port) + ")");
    }

    void stop() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            pid = -1;
        }
    }

    ~Server() { stop(); }
};

// ─── 客户端下载逻辑 ──────────────────────────────────────────────────────────

struct DownloadResult {
    bool success    = false;
    bool busy       = false;  // 收到 SERVER_BUSY
    bool not_found  = false;
    std::size_t bytes = 0;
    double elapsed  = 0.0;   // 秒
};

// 检查服务端是否因连接数满而返回 SERVER_BUSY
// 原理：服务端在 accept loop 里（不等请求）就发 SERVER_BUSY；
//       如果未满则 handle_client 阻塞等待请求，recv 超时后返回 false。
// 不发请求的原因：若发请求后服务端 close，有未读数据会导致 RST，
//               客户端 recv 收到 ECONNRESET 而非 SERVER_BUSY。
static bool is_server_busy(int port, int timeout_ms = 300) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return false;
    }
    struct timeval tv{0, timeout_ms * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[32];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    close(sock);
    return n > 0 && std::string(buf, n).find("SERVER_BUSY") != std::string::npos;
}

// 仅建立 TCP 连接不发请求，用于占用服务端连接槽。
// 建立后短暂等待（50 ms）检查是否收到 SERVER_BUSY：
//   - 超时（无数据）→ 槽位已占住（服务端在等待请求）→ 返回 fd
//   - 收到 SERVER_BUSY → 槽位没被占（服务端已拒绝）→ 返回 -1
static int connect_hold(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    // 短暂 peek：若服务端立即发 SERVER_BUSY 则说明未真正占槽
    struct timeval tv{0, 50000}; // 50 ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[20];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0 && std::string(buf, n).find("SERVER_BUSY") != std::string::npos) {
        close(sock);
        return -1; // 被拒绝，槽未占住
    }
    // 超时（n<=0 且 errno==EAGAIN）= 服务端阻塞等请求 = 槽已占住，清除超时后返回
    tv = {0, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

static DownloadResult download(int port, const std::string& filename,
                               const std::string& out_path) {
    DownloadResult res;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return res;
    }

    std::string req = std::string(1, FILE_PREFIX) + filename + "\n";
    send(sock, req.c_str(), req.size(), 0);

    auto t0 = std::chrono::steady_clock::now();
    char buf[65536];
    std::ofstream ofs(out_path, std::ios::binary);
    bool first = true;

    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        if (first) {
            first = false;
            std::string hdr(buf, std::min<ssize_t>(n, 20));
            if (hdr.find("SERVER_BUSY") != std::string::npos) {
                res.busy = true;
                close(sock);
                return res;
            }
            if (hdr.find("FILE_NOT_FOUND") != std::string::npos) {
                res.not_found = true;
                close(sock);
                return res;
            }
        }
        ofs.write(buf, n);
        res.bytes += n;
    }

    res.elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    res.success = (res.bytes > 0);
    close(sock);
    return res;
}

// ─── 测试框架 ────────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void run_test(const std::string& name, std::function<bool()> fn) {
    std::cout << "[TEST] " << name << " ... " << std::flush;
    try {
        bool ok = fn();
        std::cout << (ok ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m") << std::endl;
        ok ? ++g_pass : ++g_fail;
    } catch (const std::exception& e) {
        std::cout << "\033[31mERROR: " << e.what() << "\033[0m" << std::endl;
        ++g_fail;
    }
}

// ─── 测试用例 ────────────────────────────────────────────────────────────────

// T1：文件完整性 — 下载后内容与原始文件字节一致
bool t_integrity(const std::string& bin, const std::string& tmp) {
    const std::size_t SIZE = 4 * 1024 * 1024; // 4 MB
    create_test_file(tmp + "/t1.dat", SIZE);

    Server srv = Server::start(18080, bin, tmp);
    auto r = download(18080, "t1.dat", tmp + "/t1_out.dat");
    srv.stop();

    if (!r.success || r.bytes != SIZE) return false;
    return verify_file(tmp + "/t1_out.dat", SIZE);
}

// T2：每连接限速 — 实际吞吐量接近配置速率
// 机器：16 核 / 32 GB，loopback 带宽充足，限速误差主要来自 token bucket 初始满桶
// 文件 8 MB，限速 2 MB/s
//   初始满桶 = 2 MB → 前 2 MB "瞬发"
//   剩余 6 MB @ 2 MB/s → ~3 s
//   期望总耗时 ≈ 3 s（容差 ±60%，即 [1.2 s, 4.8 s]）
bool t_per_conn_rate(const std::string& bin, const std::string& tmp) {
    const std::size_t SIZE = 8 * 1024 * 1024;
    const double RATE = 2.0 * 1024 * 1024;
    create_test_file(tmp + "/t2.dat", SIZE);

    Server srv = Server::start(18081, bin, tmp,
        {"--per-conn-rate", std::to_string(static_cast<long long>(RATE))});
    auto r = download(18081, "t2.dat", tmp + "/t2_out.dat");
    srv.stop();

    if (!r.success || r.bytes != SIZE) return false;

    double actual = r.bytes / r.elapsed;
    double ratio  = actual / RATE;
    std::cout << " [" << actual/1e6 << " MB/s, limit=" << RATE/1e6
              << " MB/s, ratio=" << ratio << "] " << std::flush;
    return ratio >= 0.4 && ratio <= 1.6;
}

// T3：全局聚合限速 — 多客户端并发时总吞吐量不超过全局限制
// 3 个客户端同时下载，各 6 MB，全局限速 3 MB/s
// 期望：聚合速率在 [0.4x, 1.5x] 全局限速范围内
bool t_global_rate(const std::string& bin, const std::string& tmp) {
    const int N = 3;
    const std::size_t SIZE = 6 * 1024 * 1024;
    const double GLOBAL = 3.0 * 1024 * 1024;
    create_test_file(tmp + "/t3.dat", SIZE);

    Server srv = Server::start(18082, bin, tmp,
        {"--global-rate", std::to_string(static_cast<long long>(GLOBAL)),
         "--max-clients", "10"});

    auto wall0 = std::chrono::steady_clock::now();
    std::atomic<std::size_t> total{0};
    std::atomic<int> ok_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            auto r = download(18082, "t3.dat", tmp + "/t3_out" + std::to_string(i) + ".dat");
            if (r.success) {
                total.fetch_add(r.bytes);
                ok_count.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall0).count();
    srv.stop();

    if (ok_count != N) return false;
    double agg   = total / wall;
    double ratio = agg / GLOBAL;
    std::cout << " [agg=" << agg/1e6 << " MB/s, limit=" << GLOBAL/1e6
              << " MB/s, ratio=" << ratio << "] " << std::flush;
    return ratio <= 1.5 && ratio >= 0.4;
}

// T4：接入控制 — 连接数达上限后新连接收到 SERVER_BUSY
// max-clients=2：先占满 2 个槽，再尝试 3 次额外连接，全部应返回 SERVER_BUSY
bool t_admission(const std::string& bin, const std::string& tmp) {
    const int MAX = 2;
    const int EXTRA = 3;
    create_test_file(tmp + "/t4.dat", 512 * 1024); // 512 KB（用于验证正常下载）

    Server srv = Server::start(18083, bin, tmp, {"--max-clients", std::to_string(MAX)});
    // wait_ready 的探针连接会短暂占用一个槽并在 handle_client 中阻塞，
    // 等探针线程处理完 EOF 并退出（计数器归零）后再执行占槽逻辑
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 占满所有连接槽（仅建立 TCP 连接，不发请求，handle_client 在 recv 上阻塞）
    std::vector<int> held;
    for (int i = 0; i < MAX; ++i) {
        int s = connect_hold(18083);
        if (s < 0) return false; // 被拒绝说明有干扰，测试失败
        held.push_back(s);
    }
    // 再等一轮确保计数器已到达 MAX
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // 额外尝试：全部应被拒绝（直接连接后读响应，不发请求，避免 RST）
    int busy = 0;
    for (int i = 0; i < EXTRA; ++i) {
        if (is_server_busy(18083)) ++busy;
    }

    for (int s : held) close(s); // 释放槽位
    srv.stop();

    std::cout << " [busy=" << busy << "/" << EXTRA << "] " << std::flush;
    return busy == EXTRA;
}

// T5：SO_REUSEPORT — 两个实例共享同一端口，所有客户端均可成功连接
// 两个 server 实例 + 4 个并发客户端，均应下载成功
bool t_reuseport(const std::string& bin, const std::string& tmp) {
    const int N = 4;
    const std::size_t SIZE = 1 * 1024 * 1024; // 1 MB
    create_test_file(tmp + "/t5.dat", SIZE);

    // 两个实例必须都带 --reuseport
    Server srv1 = Server::start(18084, bin, tmp, {"--reuseport"});
    Server srv2 = Server::start(18084, bin, tmp, {"--reuseport"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 等两个实例均进入 accept

    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            auto r = download(18084, "t5.dat", tmp + "/t5_out" + std::to_string(i) + ".dat");
            if (r.success && r.bytes == SIZE) ok.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    srv1.stop();
    srv2.stop();

    std::cout << " [ok=" << ok << "/" << N << "] " << std::flush;
    return ok == N;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // 定位服务端可执行文件（与测试程序同目录）
    std::string server_bin = (fs::path(argv[0]).parent_path() / "server").string();
    if (!fs::exists(server_bin)) {
        std::cerr << "Server binary not found: " << server_bin << std::endl;
        return 1;
    }

    // 创建临时目录（/tmp/server_test_XXXXXX），测试结束后删除
    char tmpl[] = "/tmp/server_test_XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    std::string tmp(tmpl);
    std::cout << "Temp dir: " << tmp << std::endl;
    std::cout << "Server:   " << server_bin << std::endl << std::endl;

    // 所有测试仅使用 127.0.0.1，不修改任何网络配置
    run_test("T1 file integrity          (4 MB, no rate limit)",      [&]{ return t_integrity   (server_bin, tmp); });
    run_test("T2 per-connection rate     (8 MB @ 2 MB/s)",            [&]{ return t_per_conn_rate(server_bin, tmp); });
    run_test("T3 global aggregate rate   (3×6 MB, limit=3 MB/s)",     [&]{ return t_global_rate  (server_bin, tmp); });
    run_test("T4 admission control       (max=2, attempt 5)",          [&]{ return t_admission    (server_bin, tmp); });
    run_test("T5 SO_REUSEPORT            (2 instances, 4 clients)",   [&]{ return t_reuseport    (server_bin, tmp); });

    fs::remove_all(tmp);

    std::cout << "\nResults: \033[32m" << g_pass << " passed\033[0m, "
              << (g_fail ? "\033[31m" : "") << g_fail << " failed"
              << (g_fail ? "\033[0m" : "") << std::endl;
    return g_fail > 0 ? 1 : 0;
}
