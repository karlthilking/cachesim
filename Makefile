CXXFLAGS = -std=c++23 -g3 -O0 -fPIC

SOURCE 		= ./src
INCLUDE 	= ./include
BIN			= ./bin
TEST		= ./test
PROGRAMS 	= libcache.so inject

MAKEFLAGS += --no-print-directory

all: $(PROGRAMS)

libcache.so: $(SOURCE)/cache.cpp
	g++ $(CXXFLAGS) -shared -o $@ $<

%: $(TEST)/%.cpp
	g++ $(CXXFLAGS) -L. -lcache -Wl,-rpath,. -o $@ $<

build:
	$(BIN)/gen.sh
	@(make all)

clean:
	rm -f $(PROGRAMS) *.o *.so
