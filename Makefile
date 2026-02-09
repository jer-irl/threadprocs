CCFLAGS=-Og -Werror -Wall -Wextra
CXXFLAGS=-Og -Werror -Wall -Wextra -std=gnu++23

all: buildout/dummy_prog1 buildout/server buildout/launcher

buildout:
	mkdir -p buildout

buildout/dummy_prog1: buildout example/dummy_prog1.c
	gcc $(CCFLAGS) -o buildout/dummy_prog1 example/dummy_prog1.c

buildout/server: buildout server.c
	g++ $(CXXFLAGS) -o buildout/server server.c

buildout/launcher: buildout launcher.c
	gcc $(CCFLAGS) -o buildout/launcher launcher.c

clean:
	rm -r buildout
