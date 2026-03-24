#include "config_loader.hpp"
#include "logger.hpp"
#include "server.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char* argv[]) {
    std::string config_path = "config/server_config.json";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  -c, --config <path>   Path to config file (default: config/server_config.json)\n"
                      << "  -h, --help            Show this help message\n";
            return 0;
        }
    }

    hphttp::ServerConfig config;

    if (std::filesystem::exists(config_path)) {
        hphttp::ConfigLoader loader;
        if (!loader.load(config_path)) {
            std::cerr << "Config error: " << loader.error_message() << '\n';
            return EXIT_FAILURE;
        }
        config = loader.config();
    } else {
        config = hphttp::ConfigLoader::defaults();
        std::cerr << "Config file not found at '" << config_path
                  << "', using defaults\n";
    }

    hphttp::Logger::instance().init(config.log_level, config.log_file);
    hphttp::Logger::instance().info("Starting hphttp server v1.0.0");

    try {
        hphttp::Server server(config);
        hphttp::Server::install_signal_handlers(&server);

        server.router().get("/api/echo", [](const hphttp::HttpRequest& req) {
            hphttp::HttpResponse resp;
            resp.status_code    = 200;
            resp.status_message = "OK";

            nlohmann::json body = {
                {"method",  hphttp::HttpParser::method_to_string(req.method)},
                {"path",    req.path},
                {"headers", nlohmann::json::object()},
                {"query",   req.query_string}
            };
            for (const auto& [k, v] : req.headers) {
                body["headers"][k] = v;
            }
            resp.set_json_body(body.dump());
            return resp;
        });

        server.start();

    } catch (const std::exception& ex) {
        hphttp::Logger::instance().error("Fatal error: {}", ex.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
