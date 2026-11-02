// Example of an external application using AtlasDB through its wire protocol.
//
// Start a server first:   atlasdb-server --data demo_data
// Then run:               client_example [HOST] [PORT]

#include <iostream>
#include <string>

#include "net/client.h"

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port = argc > 2 ? static_cast<uint16_t>(std::stoi(argv[2])) : atlas::proto::DEFAULT_PORT;

    try {
        auto db = atlas::Client::connect(host, port, "client-example");
        std::cout << "Connected to " << db.server_name() << "\n";

        db.query(
            "CREATE TABLE IF NOT EXISTS accounts ("
            "  id INTEGER PRIMARY KEY,"
            "  owner TEXT NOT NULL,"
            "  balance REAL NOT NULL CHECK (balance >= 0));");

        // Values go as parameters, never spliced into the SQL text.
        db.query("DELETE FROM accounts;");
        auto add = db.prepare("INSERT INTO accounts VALUES ($1, $2, $3)");
        add.execute(atlas::params(1, "alice", 100.0));
        add.execute(atlas::params(2, "bob", 50.0));
        add.close();

        // A transfer is atomic: either both updates happen or neither does.
        auto move = db.prepare("UPDATE accounts SET balance = balance + $1 WHERE id = $2");
        db.query("BEGIN;");
        move.execute(atlas::params(-30, 1));
        move.execute(atlas::params(30, 2));
        db.query("COMMIT;");

        // This transfer violates the CHECK constraint and is rolled back.
        try {
            db.query("BEGIN; UPDATE accounts SET balance = balance + 500 WHERE id = 2;"
                     "UPDATE accounts SET balance = balance - 500 WHERE id = 1;");
        } catch (const atlas::DbError& e) {
            std::cout << "Transfer rejected: " << e.what() << "\n";
            db.query("ROLLBACK;");
        }

        atlas::QueryResult r = db.query_one("SELECT id, owner, balance FROM accounts ORDER BY id;");
        std::cout << atlas::format_result(r);
    } catch (const atlas::DbError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
