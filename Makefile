CCFLAGS=-Og -g -fPIE -pie -Werror -Wall -Wextra
CXXFLAGS=-Og -g -Werror -Wall -Wextra -std=gnu++23
LIBS=-luring

BUILD_DIR=buildout
SERVER=$(BUILD_DIR)/server
LAUNCHER=$(BUILD_DIR)/launcher
LIBTPROC=$(BUILD_DIR)/libtproc.a

.PHONY: all clean test examples

all: examples $(SERVER) $(LAUNCHER) $(LIBTPROC)

examples: $(BUILD_DIR)/examples/dummy_server $(BUILD_DIR)/examples/dummy_client $(BUILD_DIR)/examples/allocstr $(BUILD_DIR)/examples/printstr $(BUILD_DIR)/examples/detect

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

$(SERVER): $(BUILD_DIR) src/server.cpp src/elf_loader.hpp src/elf_loader.cpp src/protocol.hpp src/trampoline_aarch64.S libtproc/tproc.h
	g++ $(CXXFLAGS) -I libtproc -o $(SERVER) src/server.cpp src/elf_loader.cpp src/trampoline_aarch64.S $(LIBS)

$(LAUNCHER): $(BUILD_DIR) src/launcher.cpp src/protocol.hpp
	g++ $(CXXFLAGS) -o $(LAUNCHER) src/launcher.cpp

$(LIBTPROC): $(BUILD_DIR) libtproc/tproc.c libtproc/tproc.h
	gcc -Og -g -fPIC -Werror -Wall -Wextra -c libtproc/tproc.c -o $(BUILD_DIR)/tproc.o
	ar rcs $(LIBTPROC) $(BUILD_DIR)/tproc.o

$(BUILD_DIR)/examples/dummy_server: $(BUILD_DIR) example/dummy/dummy_server.c example/dummy/dummy_helper.h
	mkdir -p $(BUILD_DIR)/examples
	gcc $(CCFLAGS) -o $(BUILD_DIR)/examples/dummy_server example/dummy/dummy_server.c

$(BUILD_DIR)/examples/dummy_client: $(BUILD_DIR) example/dummy/dummy_client.c example/dummy/dummy_helper.h
	mkdir -p $(BUILD_DIR)/examples
	gcc $(CCFLAGS) -o $(BUILD_DIR)/examples/dummy_client example/dummy/dummy_client.c

$(BUILD_DIR)/examples/allocstr: $(BUILD_DIR) example/sharedstr/allocstr.cpp
	mkdir -p $(BUILD_DIR)/examples
	g++ $(CXXFLAGS) -o $(BUILD_DIR)/examples/allocstr example/sharedstr/allocstr.cpp

$(BUILD_DIR)/examples/printstr: $(BUILD_DIR) example/sharedstr/printstr.cpp
	mkdir -p $(BUILD_DIR)/examples
	g++ $(CXXFLAGS) -o $(BUILD_DIR)/examples/printstr example/sharedstr/printstr.cpp

$(BUILD_DIR)/examples/detect: $(BUILD_DIR) $(LIBTPROC) example/registry/detect.c libtproc/tproc.h
	mkdir -p $(BUILD_DIR)/examples
	gcc $(CCFLAGS) -I libtproc -o $(BUILD_DIR)/examples/detect example/registry/detect.c -L $(BUILD_DIR) -ltproc

test: examples $(SERVER) $(LAUNCHER) test/dummy_prog1.sh test/stringshare.sh test/registry.sh
	test/dummy_prog1.sh
	test/stringshare.sh
	test/registry.sh
