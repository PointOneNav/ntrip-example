# NTRIP Client Makefile
# Works on Linux, macOS, and Windows (with MinGW)

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS = -pthread

TARGET = ntrip_client
SRC = ntrip_client.cpp

BUILD_DIR = build
TEST_DIR = test

# Detect OS
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Darwin)
    # macOS
    CXXFLAGS += -D__APPLE__
    PTY_LIB =
else ifeq ($(UNAME_S),Linux)
    # Linux
    PTY_LIB = -lutil
else
    # Windows (MinGW)
    LDFLAGS = -lws2_32
    PTY_LIB =
    TARGET = ntrip_client.exe
endif

# Test binaries
TEST_SERVER = $(BUILD_DIR)/mock_ntrip_server
TEST_GPS = $(BUILD_DIR)/gps_simulator

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Test targets
test-server: $(BUILD_DIR) $(TEST_DIR)/mock_ntrip_server.cpp
	$(CXX) $(CXXFLAGS) -o $(TEST_SERVER) $(TEST_DIR)/mock_ntrip_server.cpp $(LDFLAGS)

test-gps: $(BUILD_DIR) $(TEST_DIR)/gps_simulator.cpp
	$(CXX) $(CXXFLAGS) -o $(TEST_GPS) $(TEST_DIR)/gps_simulator.cpp $(LDFLAGS) $(PTY_LIB)

test-binaries: test-server test-gps $(TARGET)
	cp $(TARGET) $(BUILD_DIR)/

# Run tests
test: test-binaries
	cd $(TEST_DIR) && ./run_tests.sh all

test-unit: test-binaries
	cd $(TEST_DIR) && ./run_tests.sh unit

test-integration: test-binaries
	cd $(TEST_DIR) && ./run_tests.sh integration

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all clean install test test-unit test-integration test-binaries test-server test-gps
