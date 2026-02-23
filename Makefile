CCFLAGS=-Og -fPIE -pie -Werror -Wall -Wextra
CXXFLAGS=-O0 -g -Werror -Wall -Wextra -std=gnu++23
LIBS=-luring

all: buildout/dummy_server buildout/dummy_client buildout/server buildout/launcher

buildout:
	mkdir -p buildout

buildout/dummy_server: buildout example/dummy_server.c example/dummy_helper.h
	gcc $(CCFLAGS) -o buildout/dummy_server example/dummy_server.c

buildout/dummy_client: buildout example/dummy_client.c example/dummy_helper.h
	gcc $(CCFLAGS) -o buildout/dummy_client example/dummy_client.c

buildout/trampoline_aarch64.o: buildout src/trampoline_aarch64.S
	gcc -c -o buildout/trampoline_aarch64.o src/trampoline_aarch64.S

buildout/server: buildout src/server.cpp src/elf_loader.hpp src/protocol.hpp buildout/trampoline_aarch64.o
	g++ $(CXXFLAGS) -o buildout/server src/server.cpp buildout/trampoline_aarch64.o $(LIBS)

buildout/launcher: buildout src/launcher.cpp src/protocol.hpp
	g++ $(CXXFLAGS) -o buildout/launcher src/launcher.cpp

test: buildout/dummy_server buildout/dummy_client buildout/server buildout/launcher
	rm -f buildout/test.sock buildout/dummy.sock
	example/dummy_prog1.sh

clean:
	rm -rf buildout
