#pragma once

#include "config_loader.hpp"
#include "connection.hpp"
#include "middleware.hpp"
#include "router.hpp"
#include "static_file_server.hpp"
#include "thread_pool.hpp"
#include "tls_context.hpp"

#include <atomic>
#include <csignal>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace hphttp {

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void stop();
    void wait();

    [[nodiscard]] Router&          router() noexcept;
    [[nodiscard]] MiddlewareChain& middleware() noexcept;
    [[nodiscard]] bool             is_running() const noexcept;

    static void install_signal_handlers(Server* server);

private:
    void setup_server_socket();
    void setup_epoll();
    void event_loop();
    void accept_connection();
    void handle_connection_event(int fd, uint32_t events);
    void remove_connection(int fd);
    void cleanup_timed_out_connections();
    void register_default_routes();

    HttpResponse dispatch_request(const HttpRequest& request);

    ServerConfig                                            config_;
    std::unique_ptr<ThreadPool>                             thread_pool_;
    std::unique_ptr<Router>                                 router_;
    std::unique_ptr<MiddlewareChain>                        middleware_;
    std::unique_ptr<StaticFileServer>                       static_server_;
    std::unique_ptr<TlsContext>                             tls_context_;

    int                                                     server_fd_{-1};
    int                                                     epoll_fd_{-1};

    std::unordered_map<int, std::shared_ptr<Connection>>    connections_;
    mutable std::mutex                                      connections_mutex_;

    std::atomic<bool>                                       running_{false};
    std::atomic<uint64_t>                                   total_requests_{0};
    std::atomic<uint64_t>                                   active_connections_{0};

    std::chrono::steady_clock::time_point                   start_time_;
};

}
