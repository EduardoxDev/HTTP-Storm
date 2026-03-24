#include <gtest/gtest.h>
#include "http_parser.hpp"
#include "router.hpp"

using namespace hphttp;

TEST(HttpParser, ParseSimpleGetRequest) {
    HttpParser parser;
    std::string raw = "GET /api/test HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";

    auto result = parser.feed(raw);
    EXPECT_EQ(result, HttpParser::FeedResult::COMPLETE);
    EXPECT_EQ(parser.request().method,  HttpMethod::GET);
    EXPECT_EQ(parser.request().path,    "/api/test");
    EXPECT_EQ(parser.request().version, HttpVersion::HTTP_1_1);
    EXPECT_TRUE(parser.request().keep_alive);
}

TEST(HttpParser, ParsePostRequestWithBody) {
    HttpParser parser;
    std::string body = R"({"key":"value"})";
    std::string raw  = "POST /api/data HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "\r\n" + body;

    auto result = parser.feed(raw);
    EXPECT_EQ(result, HttpParser::FeedResult::COMPLETE);
    EXPECT_EQ(parser.request().method,  HttpMethod::POST);
    EXPECT_EQ(parser.request().body,    body);
    EXPECT_EQ(parser.request().content_length, body.size());
}

TEST(HttpParser, ParseQueryString) {
    HttpParser parser;
    std::string raw = "GET /search?q=hello+world&page=2 HTTP/1.1\r\nHost: localhost\r\n\r\n";

    auto result = parser.feed(raw);
    EXPECT_EQ(result, HttpParser::FeedResult::COMPLETE);
    EXPECT_EQ(parser.request().path,         "/search");
    EXPECT_EQ(parser.request().query_params.at("q"),    "hello world");
    EXPECT_EQ(parser.request().query_params.at("page"), "2");
}

TEST(HttpParser, ParseHeadersCaseInsensitive) {
    HttpParser parser;
    std::string raw = "GET / HTTP/1.1\r\nContent-Type: application/json\r\nX-Custom-Header: value123\r\n\r\n";

    parser.feed(raw);
    EXPECT_NE(parser.request().headers.find("content-type"),    parser.request().headers.end());
    EXPECT_NE(parser.request().headers.find("x-custom-header"), parser.request().headers.end());
}

TEST(HttpParser, IncrementalParsing) {
    HttpParser parser;
    std::string part1 = "GET /test HTTP/1.1\r\n";
    std::string part2 = "Host: localhost\r\n\r\n";

    EXPECT_EQ(parser.feed(part1), HttpParser::FeedResult::NEED_MORE_DATA);
    EXPECT_EQ(parser.feed(part2), HttpParser::FeedResult::COMPLETE);
    EXPECT_EQ(parser.request().path, "/test");
}

TEST(HttpParser, InvalidRequestLine) {
    HttpParser parser;
    std::string raw = "INVALID\r\n\r\n";

    auto result = parser.feed(raw);
    EXPECT_EQ(result, HttpParser::FeedResult::ERROR);
}

TEST(HttpParser, UrlDecode) {
    EXPECT_EQ(HttpParser::url_decode("hello%20world"),  "hello world");
    EXPECT_EQ(HttpParser::url_decode("foo%3Dbar"),      "foo=bar");
    EXPECT_EQ(HttpParser::url_decode("a+b"),            "a b");
    EXPECT_EQ(HttpParser::url_decode("no+encoding"),    "no encoding");
}

TEST(HttpParser, ParseMethodStrings) {
    EXPECT_EQ(HttpParser::parse_method("GET"),     HttpMethod::GET);
    EXPECT_EQ(HttpParser::parse_method("POST"),    HttpMethod::POST);
    EXPECT_EQ(HttpParser::parse_method("PUT"),     HttpMethod::PUT);
    EXPECT_EQ(HttpParser::parse_method("DELETE"),  HttpMethod::DELETE);
    EXPECT_EQ(HttpParser::parse_method("PATCH"),   HttpMethod::PATCH);
    EXPECT_EQ(HttpParser::parse_method("OPTIONS"), HttpMethod::OPTIONS);
    EXPECT_EQ(HttpParser::parse_method("INVALID"), HttpMethod::UNKNOWN);
}

TEST(HttpParser, Reset) {
    HttpParser parser;
    parser.feed("GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(parser.request().path, "/first");

    parser.reset();
    parser.feed("GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(parser.request().path, "/second");
}

TEST(HttpResponse, Serialize) {
    HttpResponse resp;
    resp.status_code    = 200;
    resp.status_message = "OK";
    resp.set_json_body(R"({"hello":"world"})");

    std::string serialized = resp.serialize();
    EXPECT_NE(serialized.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(serialized.find("Content-Type: application/json"), std::string::npos);
    EXPECT_NE(serialized.find(R"({"hello":"world"})"), std::string::npos);
}

TEST(Router, ExactMatch) {
    Router router;
    bool called = false;

    router.get("/api/test", [&called](const HttpRequest&) {
        called = true;
        HttpResponse resp;
        resp.status_code = 200;
        return resp;
    });

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.path   = "/api/test";

    auto result = router.dispatch(req);
    EXPECT_TRUE(called);
    EXPECT_EQ(result.status_code, 200);
}

TEST(Router, ParametricRoute) {
    Router router;
    std::string captured_id;

    router.get("/users/:id", [&captured_id](const HttpRequest& req) {
        auto it = req.headers.find("x-param-id");
        HttpResponse resp;
        resp.status_code = 200;
        return resp;
    });

    auto match = router.match(HttpMethod::GET, "/users/42");
    EXPECT_TRUE(match.has_value());
    EXPECT_EQ(match->params["id"], "42");
}

TEST(Router, WildcardRoute) {
    Router router;
    router.get("/static/*", [](const HttpRequest&) {
        HttpResponse resp;
        resp.status_code = 200;
        return resp;
    });

    auto match1 = router.match(HttpMethod::GET, "/static/js/app.js");
    auto match2 = router.match(HttpMethod::GET, "/static/css/style.css");
    EXPECT_TRUE(match1.has_value());
    EXPECT_TRUE(match2.has_value());
}

TEST(Router, MethodNotAllowed) {
    Router router;
    router.get("/test", [](const HttpRequest&) {
        HttpResponse resp;
        resp.status_code = 200;
        return resp;
    });

    auto match = router.match(HttpMethod::POST, "/test");
    EXPECT_FALSE(match.has_value());
}

TEST(Router, NotFoundHandler) {
    Router router;
    router.set_not_found_handler([](const HttpRequest&) {
        HttpResponse resp;
        resp.status_code = 404;
        return resp;
    });

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.path   = "/nonexistent";

    auto result = router.dispatch(req);
    EXPECT_EQ(result.status_code, 404);
}

TEST(Router, MultipleRoutes) {
    Router router;

    router.get("/a",  [](const HttpRequest&) { HttpResponse r; r.status_code = 1; return r; });
    router.get("/b",  [](const HttpRequest&) { HttpResponse r; r.status_code = 2; return r; });
    router.post("/c", [](const HttpRequest&) { HttpResponse r; r.status_code = 3; return r; });

    HttpRequest req;
    req.path = "/a"; req.method = HttpMethod::GET;
    EXPECT_EQ(router.dispatch(req).status_code, 1);

    req.path = "/b"; req.method = HttpMethod::GET;
    EXPECT_EQ(router.dispatch(req).status_code, 2);

    req.path = "/c"; req.method = HttpMethod::POST;
    EXPECT_EQ(router.dispatch(req).status_code, 3);
}
