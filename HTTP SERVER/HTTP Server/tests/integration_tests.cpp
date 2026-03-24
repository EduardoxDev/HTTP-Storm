#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <chrono>

static int connect_to(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static std::string send_request(int fd, const std::string& request) {
    ::send(fd, request.data(), request.size(), 0);
    char buf[16384] = {};
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
}

static std::string make_get(const std::string& path, const std::string& host = "localhost") {
    return "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
}

static std::string make_post(const std::string& path,
                              const std::string& body,
                              const std::string& host = "localhost") {
    return "POST " + path + " HTTP/1.1\r\n"
           "Host: " + host + "\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

static int extract_status(const std::string& response) {
    if (response.size() < 12) return -1;
    return std::stoi(response.substr(9, 3));
}

static bool contains_header(const std::string& response, const std::string& header) {
    return response.find(header) != std::string::npos;
}

TEST(Integration, ServerRespondsToRoot) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0) << "Could not connect to server. Is it running?";

    std::string resp = send_request(fd, make_get("/"));
    ::close(fd);

    EXPECT_EQ(extract_status(resp), 200);
    EXPECT_TRUE(contains_header(resp, "Content-Type: application/json"));
}

TEST(Integration, HealthEndpoint) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_get("/health"));
    ::close(fd);

    EXPECT_EQ(extract_status(resp), 200);
    EXPECT_NE(resp.find("\"status\""), std::string::npos);
    EXPECT_NE(resp.find("\"uptime_seconds\""), std::string::npos);
}

TEST(Integration, ApiStatusEndpoint) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_get("/api/status"));
    ::close(fd);

    EXPECT_EQ(extract_status(resp), 200);
    EXPECT_NE(resp.find("\"server\""), std::string::npos);
    EXPECT_NE(resp.find("\"version\""), std::string::npos);
}

TEST(Integration, NotFoundReturns404) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_get("/does_not_exist_xyz"));
    ::close(fd);

    EXPECT_EQ(extract_status(resp), 404);
}

TEST(Integration, PostApiData) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string body = R"({"key":"value","number":42})";
    std::string resp = send_request(fd, make_post("/api/data", body));
    ::close(fd);

    EXPECT_EQ(extract_status(resp), 200);
    EXPECT_NE(resp.find("\"processed\""), std::string::npos);
}

TEST(Integration, PostApiDataEmptyBodyReturns400) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_post("/api/data", ""));
    ::close(fd);

    EXPECT_EQ(extract_status(resp), 400);
}

TEST(Integration, SecurityHeadersPresent) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_get("/health"));
    ::close(fd);

    EXPECT_TRUE(contains_header(resp, "X-Content-Type-Options"));
    EXPECT_TRUE(contains_header(resp, "X-Frame-Options"));
}

TEST(Integration, ServerHeaderPresent) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_get("/"));
    ::close(fd);

    EXPECT_TRUE(contains_header(resp, "Server: hphttp"));
}

TEST(Integration, RequestIdHeader) {
    int fd = connect_to("127.0.0.1", 8080);
    ASSERT_GE(fd, 0);

    std::string resp = send_request(fd, make_get("/health"));
    ::close(fd);

    EXPECT_TRUE(contains_header(resp, "X-Request-ID"));
}

TEST(Integration, ConcurrentConnections) {
    constexpr int NUM_THREADS = 20;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&]() {
            int fd = connect_to("127.0.0.1", 8080);
            if (fd < 0) return;

            std::string resp = send_request(fd, make_get("/health"));
            ::close(fd);

            if (extract_status(resp) == 200) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(success_count.load(), NUM_THREADS);
}
