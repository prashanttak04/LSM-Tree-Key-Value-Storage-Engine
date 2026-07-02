CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2
SRC_DIR = src
OBJ_DIR = obj

# Core Source Files
SRCS = $(SRC_DIR)/db.cc $(SRC_DIR)/wal.cc $(SRC_DIR)/sstable.cc $(SRC_DIR)/manifest.cc $(SRC_DIR)/compaction.cc
OBJS = $(SRCS:$(SRC_DIR)/%.cc=$(OBJ_DIR)/%.o)

.PHONY: all clean test

all: lsm_cli lsm_benchmark

lsm_cli: $(SRC_DIR)/cli.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

lsm_benchmark: $(SRC_DIR)/benchmark.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cc | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

test: test_phase1 test_phase2 test_phase3
	@echo "Running Phase 1 tests..."
	./test_phase1
	@echo "Running Phase 2 tests..."
	./test_phase2
	@echo "Running Phase 3 tests..."
	./test_phase3
	@echo "All tests ran successfully!"

test_phase1: $(SRC_DIR)/test_phase1.cc $(OBJ_DIR)/wal.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test_phase2: $(SRC_DIR)/test_phase2.cc $(OBJ_DIR)/sstable.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test_phase3: $(SRC_DIR)/test_phase3.cc $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -rf $(OBJ_DIR) lsm_cli lsm_benchmark test_phase1 test_phase2 test_phase3 *.dSYM test.wal test.sst test_db_manifest test_db_compact db_data
