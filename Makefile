CXXFLAGS=-std=c++20 -g3 -O1 -Wall -Wextra -Werror
LDFLAGS=-std=c++20
CXXTESTFLAGS=-Wno-ignored-qualifiers -Wno-deprecated-enum-float-conversion
LLVMFLAGS=-fpass-plugin=./libcachesimpass.so
PROGRAMS=libcache.so libcachesimpass.so cachesim_launch cachetest
MAKEFLAGS += --no-print-directory

SOURCE=./src
INCLUDE=./include
TEST=./test
EXAMPLES=./examples
CXXTEST=$(TEST)/cxxtest

all: $(PROGRAMS)

lib%.so: $(SOURCE)/%.cpp
	clang++ -I$(INCLUDE) $(CXXFLAGS) -shared -fPIC -o $@ $<

cachetest: cachetest.o cache.o cache.cxxtest.o
	clang++ $(LDFLAGS) $^ -o $@

cache.o: $(SOURCE)/cache.cpp
	clang++ -I$(INCLUDE) $(CXXFLAGS) -fPIC -c $< -o $@

cachetest.o: cachetest.cpp
	clang++ -I$(CXXTEST) $(CXXFLAGS) $(CXXTESTFLAGS) -c $< -o $@

cache.cxxtest.o: cache.cxxtest.cpp
	clang++ -I$(CXXTEST) $(CXXFLAGS) $(CXXTESTFLAGS) -c $< -o $@

%.cxxtest.cpp: $(TEST)/%.cxxtest.hpp
	python3 -W ignore $(CXXTEST)/bin/cxxtestgen --part --error-printer \
			$< -o $@

cachesim_launch: $(SOURCE)/cachesim_launch.cpp
	clang++ $(CXXFLAGS) -o $@ $<

cachetest.cpp:
	python3 -W ignore $(CXXTEST)/bin/cxxtestgen --root --error-printer \
			-o $@

build: all
	@(rm -f *.o cachetest.cpp *.cxxtest.cpp)

clean:
	rm -f $(PROGRAMS) callgrind* *cxxtest.cpp cachetest.cpp *.o *.so *.ll *.out



