CXXFLAGS = -std=c++23 -g3 -O0 -fPIC

SOURCE 		= ./src
INCLUDE 	= ./include
BIN			= ./bin
PROGRAMS 	= libcache.so

MAKEFLAGS += --no-print-directory

all: $(PROGRAMS)

libcache.so: $(SOURCE)/libcache.cpp
	g++ $(CXXFLAGS) -c $(SOURCE)/cache.cpp -o cache.o
	g++ $(CXXFLAGS) -c $< -o libcache.o
	g++ $(CXXFLAGS) -shared libcache.o cache.o -o $@
	@(rm -f *.o)

build:
	$(BIN)/gen.sh
	@(make all)

clean:
	rm -f $(PROGRAMS) *.o *.so
