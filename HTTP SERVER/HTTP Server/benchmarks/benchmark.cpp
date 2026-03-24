#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fmt/core.h>
#include <fmt/chrono.h>

struct BenchmarkConfig {
    std::string host           = "127.0.0.1";
    int         port           = 8080;
    int         connections    = 100;
    int         duration_secs  = 10;
    int         threads        = 4;
    std::string path           = "/health";
    bool        keep_alive     = true;
};

struct BenchmarkResult {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> successful_requests{0};
    std::atomic<uint64_t> failed_requests{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<bool>     stop{false};
};

static std::string build_request(const BenchmarkConfig& cfg) {
    std::ostringstream oss;
    oss << "GET " << cfg.path << " HTTP/1.1\r\n"
        << "Host: " << cfg.host << ":" << cfg.port << "\r\n"
        << "Connection: " << (cfg.keep_alive ? "keep-alive" : "close") << "\r\n"
        << "Accept: */*\r\n"
        << "\r\n";
    return oss.str();
}

static int connect_to_server(const BenchmarkConfig& cfg) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(cfg.port));
    addr.sin_addr.s_addr = inet_addr(cfg.host.c_str());

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

static void worker_thread(const BenchmarkConfig& cfg,
                           const std::string& request,
                           BenchmarkResult& result,
                           int conns_per_thread) {
    std::vector<int> fds(conns_per_thread, -1);
    char buf[65536];

    for (int i = 0; i < conns_per_thread; ++i) {
        fds[i] = connect_to_server(cfg);
    }

    while (!result.stop.load(std::memory_order_acquire)) {
        for (int i = 0; i < conns_per_thread; ++i) {
            if (fds[i] < 0) {
                fds[i] = connect_to_server(cfg);
                if (fds[i] < 0) {
                    result.failed_requests.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }

            ssize_t sent = ::send(fds[i], request.data(), request.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                ::close(fds[i]);
                fds[i] = -1;
                result.failed_requests.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            ssize_t received = ::recv(fds[i], buf, sizeof(buf), 0);
            if (received <= 0) {
                ::close(fds[i]);
                fds[i] = -1;
                result.failed_requests.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            result.total_requests.fetch_add(1, std::memory_order_relaxed);
            result.successful_requests.fetch_add(1, std::memory_order_relaxed);
            result.total_bytes.fetch_add(static_cast<uint64_t>(received), std::memory_order_relaxed);

            if (!cfg.keep_alive) {
                ::close(fds[i]);
                fds[i] = -1;
            }
        }
    }

    for (int fd : fds) {
        if (fd >= 0) ::close(fd);
    }
}

int main(int argc, char* argv[]) {
    BenchmarkConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host"        && i + 1 < argc) cfg.host          = argv[++i];
        if (arg == "--port"        && i + 1 < argc) cfg.port          = std::stoi(argv[++i]);
        if (arg == "--connections" && i + 1 < argc) cfg.connections   = std::stoi(argv[++i]);
        if (arg == "--duration"    && i + 1 < argc) cfg.duration_secs = std::stoi(argv[++i]);
        if (arg == "--threads"     && i + 1 < argc) cfg.threads       = std::stoi(argv[++i]);
        if (arg == "--path"        && i + 1 < argc) cfg.path          = argv[++i];
    }

    std::string request = build_request(cfg);
    BenchmarkResult result;

    fmt::print("hphttp Benchmark\n");
    fmt::print("================\n");
    fmt::print("Target:      {}:{}{}\n", cfg.host, cfg.port, cfg.path);
    fmt::print("Connections: {}\n", cfg.connections);
    fmt::print("Threads:     {}\n", cfg.threads);
    fmt::print("Duration:    {}s\n\n", cfg.duration_secs);

    int conns_per_thread = cfg.connections / cfg.threads;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(cfg.threads));

    for (int i = 0; i < cfg.threads; ++i) {
        workers.emplace_back(worker_thread,
                              std::cref(cfg),
                              std::cref(request),
                              std::ref(result),
                              conns_per_thread);
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (int elapsed = 0; elapsed < cfg.duration_secs; ++elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t reqs = result.total_requests.load();
        fmt::print("[{:2d}s] Requests: {:8d}  RPS: {:8.0f}\n",
                   elapsed + 1,
                   reqs,
                   static_cast<double>(reqs) / (elapsed + 1));
    }

    result.stop.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();

    auto end   = std::chrono::high_resolution_clock::now();
    double dur = std::chrono::duration<double>(end - start).count();

    uint64_t total      = result.total_requests.load();
    uint64_t successful = result.successful_requests.load();
    uint64_t failed     = result.failed_requests.load();
    uint64_t bytes      = result.total_bytes.load();

    fmt::print("\n=== Results ===\n");
    fmt::print("Duration:          {:.2f}s\n",   dur);
    fmt::print("Total requests:    {}\n",          total);
    fmt::print("Successful:        {}\n",          successful);
    fmt::print("Failed:            {}\n",          failed);
    fmt::print("Requests/sec:      {:.2f}\n",     static_cast<double>(total) / dur);
    fmt::print("Throughput:        {:.2f} MB/s\n",static_cast<double>(bytes) / dur / 1024.0 / 1024.0);
    fmt::print("Success rate:      {:.2f}%\n",
               total > 0 ? 100.0 * static_cast<double>(successful) / static_cast<double>(total) : 0.0);

    return 0;
}
