CCFLAGS=-Og -g -fPIE -pie -Werror -Wall -Wextra
CXXFLAGS=-Og -g -Werror -Wall -Wextra -std=gnu++23
LIBS=-luring

BUILD_DIR=buildout
SERVER=$(BUILD_DIR)/server
LAUNCHER=$(BUILD_DIR)/launcher

.PHONY: all clean test examples

all: examples $(SERVER) $(LAUNCHER)

examples: $(BUILD_DIR)/examples/dummy_server $(BUILD_DIR)/examples/dummy_client

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

$(SERVER): $(BUILD_DIR) src/server.cpp src/elf_loader.hpp src/protocol.hpp src/trampoline_aarch64.S
	g++ $(CXXFLAGS) -o $(SERVER) src/server.cpp src/trampoline_aarch64.S $(LIBS)

$(LAUNCHER): $(BUILD_DIR) src/launcher.cpp src/protocol.hpp
	g++ $(CXXFLAGS) -o $(LAUNCHER) src/launcher.cpp

$(BUILD_DIR)/examples/dummy_server: $(BUILD_DIR) example/dummy_server.c example/dummy_helper.h
	mkdir -p $(BUILD_DIR)/examples
	gcc $(CCFLAGS) -o $(BUILD_DIR)/examples/dummy_server example/dummy_server.c

$(BUILD_DIR)/examples/dummy_client: $(BUILD_DIR) example/dummy_client.c example/dummy_helper.h
	mkdir -p $(BUILD_DIR)/examples
	gcc $(CCFLAGS) -o $(BUILD_DIR)/examples/dummy_client example/dummy_client.c

test: examples $(SERVER) $(LAUNCHER)
	example/dummy_prog1.sh
