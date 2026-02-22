CCFLAGS=-Og -fPIE -pie -Werror -Wall -Wextra
CXXFLAGS=-O0 -g -Werror -Wall -Wextra -std=gnu++23
LIBS=-luring

all: buildout/dummy_prog1 buildout/server buildout/launcher

buildout:
	mkdir -p buildout

buildout/dummy_prog1: buildout example/dummy_prog1.c
	gcc $(CCFLAGS) -o buildout/dummy_prog1 example/dummy_prog1.c

buildout/trampoline_aarch64.o: buildout trampoline_aarch64.S
	gcc -c -o buildout/trampoline_aarch64.o trampoline_aarch64.S

buildout/server: buildout server.cpp elf_loader.hpp protocol.hpp buildout/trampoline_aarch64.o
	g++ $(CXXFLAGS) -o buildout/server server.cpp buildout/trampoline_aarch64.o $(LIBS)

buildout/launcher: buildout launcher.cpp protocol.hpp
	g++ $(CXXFLAGS) -o buildout/launcher launcher.cpp

test: buildout/dummy_prog1 buildout/server buildout/launcher
	rm -f buildout/test.sock buildout/dummy.sock
	example/dummy_prog1.sh

clean:
	rm -rf buildout
