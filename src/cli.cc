#include "db.h"
#include <iostream>
#include <string>
#include <sstream>
#include <vector>

using namespace lsm;

// Helper to split string into command parts
std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> args;
    std::stringstream ss(line);
    std::string arg;
    while (ss >> arg) {
        args.push_back(arg);
    }
    return args;
}

// ANSI Escape Codes for CLI Colors
const std::string kColorReset = "\033[0m";
const std::string kColorGreen = "\033[32m";
const std::string kColorRed   = "\033[31m";
const std::string kColorCyan  = "\033[36m";
const std::string kColorYellow = "\033[33m";
const std::string kColorBold   = "\033[1m";

void PrintHelp() {
    std::cout << kColorCyan << "Available Commands:" << kColorReset << "\n"
              << "  " << kColorBold << "put <key> <value>" << kColorReset << " - Insert or update a key-value pair\n"
              << "  " << kColorBold << "get <key>" << kColorReset << "         - Retrieve value for a key\n"
              << "  " << kColorBold << "del <key>" << kColorReset << "         - Delete a key-value pair (tombstone)\n"
              << "  " << kColorBold << "scan <start> <end>" << kColorReset << " - Perform a sorted range query\n"
              << "  " << kColorBold << "stats" << kColorReset << "              - Print internal database level/memory stats\n"
              << "  " << kColorBold << "help" << kColorReset << "               - Show this list\n"
              << "  " << kColorBold << "exit / quit" << kColorReset << "        - Close the database shell\n";
}

int main(int argc, char* argv[]) {
    std::string db_path = "db_data";
    if (argc > 1) {
        db_path = argv[1];
    }

    std::cout << kColorCyan << kColorBold 
              << "========================================\n"
              << "     LSM-Tree Storage Engine Shell      \n"
              << "========================================\n"
              << kColorReset;
    std::cout << "Opening database at: " << kColorYellow << db_path << kColorReset << " ...\n";

    DB* db = nullptr;
    Status status = DB::Open(db_path, &db);
    if (!status.ok()) {
        std::cerr << kColorRed << "Error opening database: " << status.ToString() << kColorReset << std::endl;
        return 1;
    }
    std::cout << kColorGreen << "Database open. Type 'help' for commands." << kColorReset << "\n\n";

    std::string line;
    while (true) {
        std::cout << kColorGreen << kColorBold << "lsm-db> " << kColorReset;
        if (!std::getline(std::cin, line)) {
            break; // EOF
        }

        std::vector<std::string> args = SplitArgs(line);
        if (args.empty()) {
            continue;
        }

        std::string cmd = args[0];
        // Convert to lowercase
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) {
            return std::tolower(c);
        });

        if (cmd == "exit" || cmd == "quit") {
            break;
        } else if (cmd == "help") {
            PrintHelp();
        } else if (cmd == "stats") {
            std::cout << kColorCyan << db->GetStats() << kColorReset;
        } else if (cmd == "put") {
            if (args.size() < 3) {
                std::cerr << kColorRed << "Usage: put <key> <value>" << kColorReset << "\n";
                continue;
            }
            // Join all subsequent args as value if it has spaces
            std::string key = args[1];
            std::string value = args[2];
            for (size_t i = 3; i < args.size(); ++i) {
                value += " " + args[i];
            }
            Status s = db->Put(key, value);
            if (s.ok()) {
                std::cout << kColorGreen << "OK" << kColorReset << "\n";
            } else {
                std::cerr << kColorRed << "Error: " << s.ToString() << kColorReset << "\n";
            }
        } else if (cmd == "get") {
            if (args.size() < 2) {
                std::cerr << kColorRed << "Usage: get <key>" << kColorReset << "\n";
                continue;
            }
            std::string key = args[1];
            std::string value;
            Status s = db->Get(key, &value);
            if (s.ok()) {
                std::cout << kColorYellow << value << kColorReset << "\n";
            } else if (s.IsNotFound()) {
                std::cout << kColorRed << "(nil)" << kColorReset << "\n";
            } else {
                std::cerr << kColorRed << "Error: " << s.ToString() << kColorReset << "\n";
            }
        } else if (cmd == "del") {
            if (args.size() < 2) {
                std::cerr << kColorRed << "Usage: del <key>" << kColorReset << "\n";
                continue;
            }
            std::string key = args[1];
            Status s = db->Delete(key);
            if (s.ok()) {
                std::cout << kColorGreen << "OK" << kColorReset << "\n";
            } else {
                std::cerr << kColorRed << "Error: " << s.ToString() << kColorReset << "\n";
            }
        } else if (cmd == "scan") {
            if (args.size() < 3) {
                std::cerr << kColorRed << "Usage: scan <start_key> <end_key>" << kColorReset << "\n";
                continue;
            }
            std::string start = args[1];
            std::string end = args[2];
            std::vector<std::pair<std::string, std::string>> results;
            Status s = db->Scan(start, end, results);
            if (s.ok()) {
                std::cout << kColorCyan << "Found " << results.size() << " entries:\n" << kColorReset;
                for (const auto& pair : results) {
                    std::cout << "  " << pair.first << " => " << kColorYellow << pair.second << kColorReset << "\n";
                }
            } else {
                std::cerr << kColorRed << "Error: " << s.ToString() << kColorReset << "\n";
            }
        } else {
            std::cerr << kColorRed << "Unknown command: '" << cmd << "'. Type 'help' for list." << kColorReset << "\n";
        }
    }

    std::cout << "\nClosing database cleanly...\n";
    delete db;
    std::cout << kColorGreen << "Done! Goodbye." << kColorReset << "\n";
    return 0;
}
