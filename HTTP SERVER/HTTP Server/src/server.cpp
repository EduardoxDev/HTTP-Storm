#include "server.hpp"
#include "logger.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace hphttp {

static Server* g_server_instance = nullptr;

Server::Server(ServerConfig config)
    : config_(std::move(config))
    , start_time_(std::chrono::steady_clock::now())
{
    std::size_t pool_size = config_.thread_pool_size > 0
        ? config_.thread_pool_size
        : std::max(1u, std::thread::hardware_concurrency());

    thread_pool_  = std::make_unique<ThreadPool>(pool_size);
    router_       = std::make_unique<Router>();
    middleware_   = std::make_unique<MiddlewareChain>();
    static_server_= std::make_unique<StaticFileServer>(
        config_.static_file_directory,
        config_.enable_directory_listing);

    if (config_.tls.enabled) {
        tls_context_ = std::make_unique<TlsContext>(config_.tls);
    }

    register_default_routes();

    if (config_.log_access) {
        middleware_->use(middleware::request_logger());
    }
    if (config_.rate_limit.enabled) {
        middleware_->use(middleware::rate_limiter(
            config_.rate_limit.requests_per_second,
            config_.rate_limit.burst_size));
    }
    if (config_.enable_gzip) {
        middleware_->use(middleware::gzip_compression(config_.gzip_level));
    }
    middleware_->use(middleware::security_headers());
    middleware_->use(middleware::request_id());
}

Server::~Server() {
    stop();
}

void Server::start() {
    setup_server_socket();
    setup_epoll();

    running_.store(true, std::memory_order_release);

    Logger::instance().info("Server listening on {}:{} with {} threads",
                             config_.host, config_.port,
                             thread_pool_->size());

    if (config_.tls.enabled) {
        Logger::instance().info("TLS enabled");
    }

    event_loop();
}

void Server::stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    running_.store(false, std::memory_order_release);

    Logger::instance().info("Initiating graceful shutdown...");

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [fd, conn] : connections_) {
            conn->close();
        }
        connections_.clear();
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    thread_pool_->shutdown();

    Logger::instance().info("Server stopped. Total requests handled: {}",
                             total_requests_.load());
}

void Server::wait() {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

Router& Server::router() noexcept {
    return *router_;
}

MiddlewareChain& Server::middleware() noexcept {
    return *middleware_;
}

bool Server::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void Server::install_signal_handlers(Server* server) {
    g_server_instance = server;

    struct sigaction sa{};
    sa.sa_handler = [](int) {
        if (g_server_instance) {
            g_server_instance->stop();
        }
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

void Server::setup_server_socket() {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    int sndbuf = 1 << 17;
    int rcvbuf = 1 << 17;
    setsockopt(server_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(server_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY,   &opt, sizeof(opt));
    setsockopt(server_fd_, IPPROTO_TCP, TCP_QUICKACK,  &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(config_.port);
    addr.sin_addr.s_addr = inet_addr(config_.host.c_str());

    if (::bind(server_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
    }

    if (::listen(server_fd_, static_cast<int>(config_.backlog)) < 0) {
        throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
    }
}

void Server::setup_epoll() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1() failed: ") + std::strerror(errno));
    }

    struct epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd_;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl() failed: ") + std::strerror(errno));
    }
}

void Server::event_loop() {
    constexpr int MAX_EVENTS = 1024;
    std::vector<epoll_event> events(MAX_EVENTS);

    auto last_cleanup = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        int n = ::epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, 1000);

        if (n < 0) {
            if (errno == EINTR) continue;
            Logger::instance().error("epoll_wait() error: {}", std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_fd_) {
                accept_connection();
            } else {
                handle_connection_event(fd, events[i].events);
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_cleanup > std::chrono::seconds(10)) {
            cleanup_timed_out_connections();
            last_cleanup = now;
        }
    }
}

void Server::accept_connection() {
    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);

        int client_fd = ::accept4(server_fd_,
                                  reinterpret_cast<sockaddr*>(&client_addr),
                                  &addrlen,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EMFILE || errno == ENFILE) {
                Logger::instance().warn("Too many open files; dropping connection");
                break;
            }
            Logger::instance().error("accept4() failed: {}", std::strerror(errno));
            break;
        }

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (connections_.size() >= config_.max_connections) {
                Logger::instance().warn("Max connections reached; dropping connection from {}",
                                        inet_ntoa(client_addr.sin_addr));
                ::close(client_fd);
                break;
            }
        }

        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        std::string remote_addr = std::string(inet_ntoa(client_addr.sin_addr))
            + ":" + std::to_string(ntohs(client_addr.sin_port));

        auto conn = std::make_shared<Connection>(
            client_fd,
            remote_addr,
            [this](const HttpRequest& req) { return dispatch_request(req); },
            tls_context_.get(),
            config_.keepalive_timeout_ms,
            config_.keepalive_max_requests,
            config_.max_request_size);

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[client_fd] = conn;
        }

        active_connections_.fetch_add(1, std::memory_order_relaxed);

        struct epoll_event ev{};
        ev.events   = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
        ev.data.fd  = client_fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev);
    }
}

void Server::handle_connection_event(int fd, uint32_t events) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        conn = it->second;
    }

    if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
        remove_connection(fd);
        return;
    }

    thread_pool_->submit([conn, events, fd, this]() {
        if (events & EPOLLIN) {
            conn->handle_read();
        }
        if (events & EPOLLOUT) {
            conn->handle_write();
        }

        if (!conn->is_alive()) {
            remove_connection(fd);
        }
    });

    total_requests_.fetch_add(1, std::memory_order_relaxed);
}

void Server::remove_connection(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(fd);
    if (it != connections_.end()) {
        it->second->close();
        connections_.erase(it);
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Server::cleanup_timed_out_connections() {
    std::vector<int> timed_out_fds;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [fd, conn] : connections_) {
            if (conn->is_timed_out()) {
                timed_out_fds.push_back(fd);
            }
        }
    }

    for (int fd : timed_out_fds) {
        Logger::instance().debug("Closing timed out connection fd={}", fd);
        remove_connection(fd);
    }
}

void Server::register_default_routes() {
    router_->get("/", [](const HttpRequest&) {
        HttpResponse resp;
        resp.status_code    = 200;
        resp.status_message = "OK";
        resp.set_json_body(R"({"message":"High Performance HTTP Server","version":"1.0.0"})");
        return resp;
    });

    router_->get("/health", [this](const HttpRequest&) {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_).count();

        nlohmann::json body = {
            {"status",             "ok"},
            {"uptime_seconds",     uptime},
            {"active_connections", active_connections_.load()},
            {"total_requests",     total_requests_.load()},
            {"thread_pool_size",   thread_pool_->size()},
            {"pending_tasks",      thread_pool_->pending_tasks()}
        };

        HttpResponse resp;
        resp.status_code    = 200;
        resp.status_message = "OK";
        resp.set_json_body(body.dump());
        return resp;
    });

    router_->get("/api/status", [this](const HttpRequest&) {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_).count();

        nlohmann::json body = {
            {"server",   "hphttp"},
            {"version",  "1.0.0"},
            {"uptime",   uptime},
            {"requests", total_requests_.load()},
            {"connections", {
                {"active", active_connections_.load()},
                {"max",    config_.max_connections}
            }},
            {"tls",      config_.tls.enabled},
            {"gzip",     config_.enable_gzip}
        };

        HttpResponse resp;
        resp.status_code    = 200;
        resp.status_message = "OK";
        resp.set_json_body(body.dump());
        return resp;
    });

    router_->post("/api/data", [](const HttpRequest& req) {
        HttpResponse resp;

        if (req.body.empty()) {
            resp.status_code    = 400;
            resp.status_message = "Bad Request";
            resp.set_json_body(R"({"error":"Empty body","code":400})");
            return resp;
        }

        nlohmann::json input;
        try {
            input = nlohmann::json::parse(req.body);
        } catch (const nlohmann::json::exception&) {
            resp.status_code    = 422;
            resp.status_message = "Unprocessable Entity";
            resp.set_json_body(R"({"error":"Invalid JSON","code":422})");
            return resp;
        }

        nlohmann::json body = {
            {"received", input},
            {"processed", true},
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };

        resp.status_code    = 200;
        resp.status_message = "OK";
        resp.set_json_body(body.dump());
        return resp;
    });

    router_->get("/static/*", [this](const HttpRequest& req) -> HttpResponse {
        auto result = static_server_->serve(req);
        if (result) return *result;

        HttpResponse resp;
        resp.status_code    = 404;
        resp.status_message = "Not Found";
        resp.set_json_body(R"({"error":"File not found","code":404})");
        return resp;
    });

    router_->set_not_found_handler([](const HttpRequest& req) {
        HttpResponse resp;
        resp.status_code    = 404;
        resp.status_message = "Not Found";
        nlohmann::json body = {{"error", "Not Found"}, {"path", req.path}, {"code", 404}};
        resp.set_json_body(body.dump());
        return resp;
    });

    router_->set_method_not_allowed_handler([](const HttpRequest& req) {
        HttpResponse resp;
        resp.status_code    = 405;
        resp.status_message = "Method Not Allowed";
        nlohmann::json body = {
            {"error",  "Method Not Allowed"},
            {"method", HttpParser::method_to_string(req.method)},
            {"code",   405}
        };
        resp.set_json_body(body.dump());
        return resp;
    });
}

HttpResponse Server::dispatch_request(const HttpRequest& request) {
    return middleware_->execute(request, [this](const HttpRequest& req) {
        return router_->dispatch(req);
    });
}

}
