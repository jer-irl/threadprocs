CCFLAGS=-Og -Werror -Wall -Wextra
CXXFLAGS=-O0 -g -Werror -Wall -Wextra -std=gnu++23
LIBS=-luring

all: buildout/dummy_prog1 buildout/server buildout/launcher

buildout:
	mkdir -p buildout

buildout/dummy_prog1: buildout example/dummy_prog1.c
	gcc $(CCFLAGS) -o buildout/dummy_prog1 example/dummy_prog1.c

buildout/server: buildout server.cpp
	g++ $(CXXFLAGS) -o buildout/server server.cpp $(LIBS)

buildout/launcher: buildout launcher.cpp
	g++ $(CXXFLAGS) -o buildout/launcher launcher.cpp $(LIBS)

clean:
	rm -r buildout
