CXX := g++
CC  := gcc
AR  := ar

CFLAGS        := -Og -g -fPIE -pie -Werror -Wall -Wextra
CXXFLAGS      := -Og -g -Werror -Wall -Wextra -std=gnu++23
LIBTPROC_CFLAGS := -Og -g -fPIC -Werror -Wall -Wextra
DEPFLAGS       = -MMD -MP -MF $(@:.o=.d)

BUILD_DIR := buildout
OBJ_DIR   := $(BUILD_DIR)/obj

SERVER   := $(BUILD_DIR)/server
LAUNCHER := $(BUILD_DIR)/launcher
LIBTPROC := $(BUILD_DIR)/libtproc.a

# --- Server -----------------------------------------------------------
SERVER_SRCS := src/server/server.cpp src/server/elf_loader.cpp
SERVER_ASM  := src/server/trampoline_aarch64.S
SERVER_OBJS := $(SERVER_SRCS:src/server/%.cpp=$(OBJ_DIR)/server/%.o) \
               $(SERVER_ASM:src/server/%.S=$(OBJ_DIR)/server/%.o)

# --- Launcher ---------------------------------------------------------
LAUNCHER_SRCS := src/launcher.cpp
LAUNCHER_OBJS := $(LAUNCHER_SRCS:src/%.cpp=$(OBJ_DIR)/launcher/%.o)

# --- libtproc ---------------------------------------------------------
LIBTPROC_SRCS := libtproc/tproc.c
LIBTPROC_OBJS := $(LIBTPROC_SRCS:libtproc/%.c=$(OBJ_DIR)/libtproc/%.o)

# --- Examples ----------------------------------------------------------
EXAMPLES := $(addprefix $(BUILD_DIR)/examples/, \
              dummy_server dummy_client allocstr printstr detect)

# Collect all dependency files
ALL_DEPS := $(patsubst %.o,%.d,$(SERVER_OBJS) $(LAUNCHER_OBJS) $(LIBTPROC_OBJS))

# ======================================================================

.PHONY: all clean test examples

all: examples $(SERVER) $(LAUNCHER) $(LIBTPROC)

examples: $(EXAMPLES)

clean:
	rm -rf $(BUILD_DIR)

# --- Link rules --------------------------------------------------------

$(SERVER): $(SERVER_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ -luring

$(LAUNCHER): $(LAUNCHER_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(LIBTPROC): $(LIBTPROC_OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

# --- Compile rules (server) --------------------------------------------

$(OBJ_DIR)/server/%.o: src/server/%.cpp | $(OBJ_DIR)/server
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -I src -I libtproc -c -o $@ $<

$(OBJ_DIR)/server/%.o: src/server/%.S | $(OBJ_DIR)/server
	$(CXX) $(DEPFLAGS) -c -o $@ $<

# --- Compile rules (launcher) ------------------------------------------

$(OBJ_DIR)/launcher/%.o: src/%.cpp | $(OBJ_DIR)/launcher
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

# --- Compile rules (libtproc) ------------------------------------------

$(OBJ_DIR)/libtproc/%.o: libtproc/%.c | $(OBJ_DIR)/libtproc
	$(CC) $(LIBTPROC_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# --- Examples (single-file programs) ------------------------------------

$(BUILD_DIR)/examples/dummy_server: example/dummy/dummy_server.c | $(BUILD_DIR)/examples
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/examples/dummy_client: example/dummy/dummy_client.c | $(BUILD_DIR)/examples
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/examples/allocstr: example/sharedstr/allocstr.cpp | $(BUILD_DIR)/examples
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/examples/printstr: example/sharedstr/printstr.cpp | $(BUILD_DIR)/examples
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/examples/detect: example/registry/detect.c $(LIBTPROC) | $(BUILD_DIR)/examples
	$(CC) $(CFLAGS) -I libtproc -o $@ $< -L $(BUILD_DIR) -ltproc

# --- Directory creation -------------------------------------------------

$(BUILD_DIR) $(BUILD_DIR)/examples $(OBJ_DIR)/server $(OBJ_DIR)/launcher $(OBJ_DIR)/libtproc:
	mkdir -p $@

# --- Tests ---------------------------------------------------------------

test: examples $(SERVER) $(LAUNCHER)
	test/dummy_prog1.sh
	test/stringshare.sh
	test/registry.sh

# --- Auto-generated header dependencies ----------------------------------
-include $(ALL_DEPS)
