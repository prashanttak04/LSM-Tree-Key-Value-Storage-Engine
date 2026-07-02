#include "db.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <algorithm>
#include <iomanip>

using namespace lsm;

// ANSI colors
const std::string kColorReset  = "\033[0m";
const std::string kColorGreen  = "\033[32m";
const std::string kColorRed    = "\033[31m";
const std::string kColorCyan   = "\033[36m";
const std::string kColorYellow = "\033[33m";
const std::string kColorBold   = "\033[1m";

struct ThreadMetrics {
    std::vector<uint64_t> latencies_us; // Operation latency in microseconds
    size_t write_count = 0;
    size_t read_count = 0;
    size_t not_found_count = 0;
};

void RunWorker(DB* db, int thread_id, size_t ops_per_thread, int read_percent, 
               size_t key_range, ThreadMetrics* metrics) {
    
    // Seed generator specifically for this thread to avoid contention
    std::mt19937 rng(std::random_device{}() + thread_id);
    std::uniform_int_distribution<size_t> key_dist(0, key_range - 1);
    std::uniform_int_distribution<int> op_dist(0, 99);

    metrics->latencies_us.reserve(ops_per_thread);

    for (size_t i = 0; i < ops_per_thread; ++i) {
        bool is_read = (op_dist(rng) < read_percent);
        size_t key_num = key_dist(rng);
        std::string key = "key_" + std::to_string(key_num);
        
        auto start = std::chrono::high_resolution_clock::now();

        if (is_read) {
            std::string val;
            Status s = db->Get(key, &val);
            metrics->read_count++;
            if (s.IsNotFound()) {
                metrics->not_found_count++;
            }
        } else {
            std::string val = "value_" + std::to_string(key_num) + "_" + std::to_string(i);
            db->Put(key, val);
            metrics->write_count++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        uint64_t latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        metrics->latencies_us.push_back(latency);
    }
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    size_t ops_per_thread = 10000;
    int read_percent = 50; // Default: 50% read, 50% write
    size_t key_range = 50000;

    if (argc > 1) num_threads = std::stoi(argv[1]);
    if (argc > 2) ops_per_thread = std::stoull(argv[2]);
    if (argc > 3) read_percent = std::stoi(argv[3]);
    if (argc > 4) key_range = std::stoull(argv[4]);

    std::cout << kColorCyan << kColorBold 
              << "==================================================\n"
              << "   LSM-Tree Key-Value Engine Performance Bench   \n"
              << "==================================================\n"
              << kColorReset;
    std::cout << "Threads:       " << kColorYellow << num_threads << kColorReset << "\n"
              << "Ops/Thread:    " << kColorYellow << ops_per_thread << kColorReset << "\n"
              << "Read Ratio:    " << kColorYellow << read_percent << "%" << kColorReset << "\n"
              << "Key Range:     " << kColorYellow << key_range << " distinct keys" << kColorReset << "\n\n";

    std::string db_path = "db_bench_data";
    // Clear old bench data for clean run
    std::string clean_cmd = "rm -rf " + db_path;
    ::system(clean_cmd.c_str());

    DB* db = nullptr;
    Status s = DB::Open(db_path, &db);
    if (!s.ok()) {
        std::cerr << "Error opening database: " << s.ToString() << std::endl;
        return 1;
    }

    std::vector<std::thread> threads;
    std::vector<ThreadMetrics> metrics(num_threads);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(RunWorker, db, i, ops_per_thread, read_percent, key_range, &metrics[i]);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time_s = std::chrono::duration<double>(end_time - start_time).count();

    // Aggregate metrics
    std::vector<uint64_t> all_latencies;
    all_latencies.reserve(num_threads * ops_per_thread);
    size_t total_writes = 0;
    size_t total_reads = 0;
    size_t total_not_found = 0;

    for (const auto& m : metrics) {
        all_latencies.insert(all_latencies.end(), m.latencies_us.begin(), m.latencies_us.end());
        total_writes += m.write_count;
        total_reads += m.read_count;
        total_not_found += m.not_found_count;
    }

    size_t total_ops = total_writes + total_reads;
    double throughput = static_cast<double>(total_ops) / total_time_s;

    // Calculate latency percentiles
    std::sort(all_latencies.begin(), all_latencies.end());
    double sum = 0;
    for (uint64_t lat : all_latencies) {
        sum += lat;
    }
    double avg_latency = sum / all_latencies.size();
    
    uint64_t p50 = all_latencies[all_latencies.size() / 2];
    uint64_t p90 = all_latencies[static_cast<size_t>(all_latencies.size() * 0.90)];
    uint64_t p95 = all_latencies[static_cast<size_t>(all_latencies.size() * 0.95)];
    uint64_t p99 = all_latencies[static_cast<size_t>(all_latencies.size() * 0.99)];
    uint64_t p999 = all_latencies[static_cast<size_t>(all_latencies.size() * 0.999)];

    std::cout << kColorGreen << "Benchmark Completed in " << std::fixed << std::setprecision(3) 
              << total_time_s << " seconds." << kColorReset << "\n\n";

    std::cout << kColorBold << "Throughput Stats:" << kColorReset << "\n"
              << "  Total Operations: " << total_ops << "\n"
              << "  Writes:           " << total_writes << "\n"
              << "  Reads:            " << total_reads << " (" << total_not_found << " not found)\n"
              << "  Throughput:       " << kColorGreen << kColorBold << std::fixed << std::setprecision(2)
              << throughput << " ops/sec" << kColorReset << "\n\n";

    std::cout << kColorBold << "Latency Stats (Microseconds):" << kColorReset << "\n"
              << "  Average Latency:  " << std::fixed << std::setprecision(1) << avg_latency << " us\n"
              << "  p50 (Median):     " << p50 << " us\n"
              << "  p90:              " << p90 << " us\n"
              << "  p95:              " << p95 << " us\n"
              << "  p99 (Tail):       " << kColorYellow << p99 << " us" << kColorReset << "\n"
              << "  p99.9 (Worst):    " << kColorRed << p999 << " us" << kColorReset << "\n\n";

    std::cout << "Closing database and cleaning up files...\n";
    delete db;
    ::system(clean_cmd.c_str());
    std::cout << kColorGreen << "Benchmark Done!" << kColorReset << "\n";

    return 0;
}
