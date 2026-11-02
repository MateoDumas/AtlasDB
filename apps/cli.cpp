// atlasdb: interactive shell for AtlasDB.
//
//   atlasdb [DATA_DIR]                 embedded mode: open (or create) a local database
//   atlasdb --connect HOST[:PORT]      client mode: talk to a running atlasdb-server

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "common/error.h"
#include "common/result.h"
#include "common/version.h"
#include "engine/database.h"
#include "engine/session.h"
#include "net/client.h"
#include "sql/parser.h"

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace atlas;

namespace {

using ResultSink = std::function<void(const QueryResult&)>;

class Backend {
public:
    virtual ~Backend() = default;
    // Runs SQL, reporting each statement's result as it completes. Throws on the first error.
    virtual void run(const std::string& sql, const ResultSink& sink) = 0;
    virtual bool in_transaction() const = 0;
    virtual std::string description() const = 0;
};

class EmbeddedBackend : public Backend {
public:
    explicit EmbeddedBackend(const std::string& dir) : db_(Database::open(dir)), session_(*db_), dir_(dir) {}

    void run(const std::string& sql, const ResultSink& sink) override {
        auto stmts = parse_sql(sql);
        for (auto& s : stmts) sink(session_.execute_statement(s));
    }
    bool in_transaction() const override { return session_.in_transaction(); }
    std::string description() const override {
        std::string s = "local database '" + dir_ + "'";
        if (db_->recovered_transactions())
            s += " (recovered " + std::to_string(db_->recovered_transactions()) + " transactions from the WAL)";
        return s;
    }

private:
    std::unique_ptr<Database> db_;
    Session session_;
    std::string dir_;
};

class RemoteBackend : public Backend {
public:
    RemoteBackend(const std::string& host, uint16_t port)
        : client_(Client::connect(host, port, "atlasdb-cli")), addr_(host + ":" + std::to_string(port)) {}

    void run(const std::string& sql, const ResultSink& sink) override {
        client_.query_stream(sql, [&](QueryResult&& r) { sink(r); });
    }
    bool in_transaction() const override { return client_.in_transaction(); }
    std::string description() const override { return client_.server_name() + " at " + addr_; }

private:
    Client client_;
    std::string addr_;
};

void print_usage() {
    std::cout << "Usage:\n"
                 "  atlasdb [DATA_DIR]               open or create a local database (default: ./atlas_data)\n"
                 "  atlasdb --connect HOST[:PORT]    connect to a running atlasdb-server\n"
                 "\nOptions:\n"
                 "  -c, --command SQL   execute SQL and exit\n"
                 "  -f, --file PATH     execute a SQL script and exit\n"
                 "  -v, --version       print the version\n"
                 "  -h, --help          show this help\n";
}

void print_help() {
    std::cout << "Meta commands:\n"
                 "  .help               show this message\n"
                 "  .tables             list tables\n"
                 "  .schema TABLE       describe a table\n"
                 "  .read FILE          execute a SQL script\n"
                 "  .timer on|off       show execution time\n"
                 "  .quit               exit (also .exit or Ctrl-D)\n"
                 "\nSQL statements end with ';'. Supported: CREATE/DROP TABLE, CREATE/DROP INDEX,\n"
                 "INSERT, SELECT (joins, GROUP BY, ORDER BY, LIMIT), UPDATE, DELETE,\n"
                 "BEGIN/COMMIT/ROLLBACK, EXPLAIN, SHOW TABLES, DESCRIBE, CHECKPOINT.\n";
}

// True when `buf` holds a complete statement: the last significant character
// outside quotes and comments is ';'.
bool statement_complete(const std::string& buf) {
    char quote = 0;
    char last = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        char c = buf[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '-' && i + 1 < buf.size() && buf[i + 1] == '-') {
            while (i < buf.size() && buf[i] != '\n') ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            last = c;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) last = c;
    }
    return !quote && last == ';';
}

struct Shell {
    Backend& backend;
    bool timer = false;

    // Returns false if an error occurred.
    bool execute(const std::string& sql) {
        auto start = std::chrono::steady_clock::now();
        bool ok = true;
        try {
            backend.run(sql, [](const QueryResult& r) { std::cout << format_result(r); });
        } catch (const DbError& e) {
            std::cout << "ERROR (" << error_code_name(e.code()) << "): " << e.what() << "\n";
            ok = false;
        }
        if (timer) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("Time: %.3f ms\n", ms);
        }
        std::cout.flush();
        return ok;
    }

    bool run_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cout << "ERROR: cannot open file '" << path << "'\n";
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        return execute(ss.str());
    }

    // Returns false when the shell should exit.
    bool meta(const std::string& line) {
        std::istringstream in(line);
        std::string cmd, arg;
        in >> cmd >> arg;
        if (cmd == ".quit" || cmd == ".exit") return false;
        if (cmd == ".help") print_help();
        else if (cmd == ".tables") execute("SHOW TABLES;");
        else if (cmd == ".schema") {
            if (arg.empty()) execute("SHOW TABLES;");
            else execute("DESCRIBE " + arg + ";");
        } else if (cmd == ".read") run_file(arg);
        else if (cmd == ".timer") {
            timer = arg == "on";
            std::cout << "Timer " << (timer ? "on" : "off") << "\n";
        } else {
            std::cout << "Unknown command " << cmd << " (try .help)\n";
        }
        return true;
    }

    // Reads statements line by line. Interactive mode shows a banner and prompts;
    // otherwise (piped input) it stays quiet. Returns false if any statement failed.
    bool repl(bool interactive) {
        if (interactive)
            std::cout << PRODUCT << " - connected to " << backend.description() << "\n"
                      << "Type .help for help, .quit to exit.\n";
        bool all_ok = true;
        std::string buffer, line;
        while (true) {
            if (interactive) {
                const char* prompt = buffer.empty() ? (backend.in_transaction() ? "atlas*> " : "atlas> ") : "   ...> ";
                std::cout << prompt << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                if (!buffer.empty() && buffer.find_first_not_of(" \t\r\n") != std::string::npos)
                    all_ok = execute(buffer) && all_ok;
                if (interactive) std::cout << "\n";
                break;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (buffer.empty()) {
                size_t first = line.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                if (line[first] == '.') {
                    if (!meta(line.substr(first))) break;
                    continue;
                }
            }
            buffer += line + "\n";
            if (statement_complete(buffer)) {
                all_ok = execute(buffer) && all_ok;
                buffer.clear();
            }
        }
        if (backend.in_transaction()) std::cout << "Open transaction rolled back.\n";
        return all_ok;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir = "atlas_data";
    std::string connect;
    std::string command;
    std::string file;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << opt << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else if (a == "-v" || a == "--version") {
            std::cout << PRODUCT << "\n";
            return 0;
        } else if (a == "--connect") {
            connect = need("--connect");
        } else if (a == "-c" || a == "--command") {
            command = need("--command");
        } else if (a == "-f" || a == "--file") {
            file = need("--file");
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option " << a << "\n";
            print_usage();
            return 2;
        } else {
            data_dir = a;
        }
    }

    try {
        std::unique_ptr<Backend> backend;
        if (!connect.empty()) {
            std::string host = connect;
            uint16_t port = proto::DEFAULT_PORT;
            if (auto colon = connect.rfind(':'); colon != std::string::npos) {
                host = connect.substr(0, colon);
                port = static_cast<uint16_t>(std::stoi(connect.substr(colon + 1)));
            }
            backend = std::make_unique<RemoteBackend>(host, port);
        } else {
            backend = std::make_unique<EmbeddedBackend>(data_dir);
        }

        Shell shell{*backend};
        if (!command.empty()) return shell.execute(command) ? 0 : 1;
        if (!file.empty()) return shell.run_file(file) ? 0 : 1;
        return shell.repl(isatty(fileno(stdin)) != 0) ? 0 : 1;
    } catch (const DbError& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
