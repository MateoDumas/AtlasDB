// atlasdb-server: serves a database directory over the AtlasDB wire protocol.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "engine/database.h"
#include "net/server.h"

using namespace atlas;

namespace {

std::atomic<bool> stop_requested{false};

void on_signal(int) { stop_requested = true; }

void print_usage() {
    std::cout << "Usage: atlasdb-server [options]\n"
                 "  --data DIR              database directory (default: ./atlas_data)\n"
                 "  --host HOST             address to listen on (default: 127.0.0.1)\n"
                 "  --port PORT             TCP port (default: 5480)\n"
                 "  --max-connections N     concurrent client limit (default: 64)\n"
                 "  --no-sync               do not fsync the WAL on commit (faster, less durable)\n"
                 "  --idle-txn-timeout SEC  roll back transactions idle this long (default: 60, 0 = never)\n"
                 "  -h, --help              show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "atlas_data";
    ServerOptions sopts;
    DatabaseOptions dopts;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else if (a == "--data") {
            dir = need();
        } else if (a == "--host") {
            sopts.host = need();
        } else if (a == "--port") {
            sopts.port = static_cast<uint16_t>(std::stoi(need()));
        } else if (a == "--max-connections") {
            sopts.max_connections = static_cast<size_t>(std::stoul(need()));
        } else if (a == "--idle-txn-timeout") {
            sopts.idle_transaction_timeout = std::chrono::seconds(std::stoul(need()));
        } else if (a == "--no-sync") {
            dopts.sync_commits = false;
        } else {
            std::cerr << "unknown option " << a << "\n";
            print_usage();
            return 2;
        }
    }

    try {
        auto db = Database::open(dir, dopts);
        if (db->recovered_transactions())
            std::cout << "Recovered " << db->recovered_transactions() << " committed transactions from the WAL\n";
        Server server(*db, sopts);
        server.start();
        std::cout << sopts.name << " listening on " << sopts.host << ":" << server.port() << " (data: " << dir
                  << ")\nPress Ctrl+C to stop.\n"
                  << std::flush;

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        while (!stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::cout << "Shutting down..." << std::endl;
        server.stop();
        db->checkpoint();
        std::cout << "Checkpoint complete. Bye." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
