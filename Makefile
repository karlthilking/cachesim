CXXFLAGS=-std=c++23 -g -O3 -Wall -Wextra -Werror
CXXTESTFLAGS=-Wno-ignored-qualifiers -Wno-deprecated-enum-float-conversion
LDFLAGS=-std=c++23
LLVMFLAGS=-fpass-plugin=./libcachesimpass.so
PROGRAMS=libcache.so cachetest matmul scan
MAKEFLAGS += --no-print-directory

SOURCE=./src
INCLUDE=./include
TEST=./test
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

%: ./programs/%.cpp libcache.so libcachesimpass.so
	clang++ $(CXXFLAGS) -L. -lcache -Wl,-rpath,. $(LLVMFLAGS) -o $@ $<

cachetest.cpp:
	python3 -W ignore $(CXXTEST)/bin/cxxtestgen --root --error-printer \
			-o $@

build: all
	@(rm -f *.o cachetest.cpp *.cxxtest.cpp)

clean:
	rm -f $(PROGRAMS) *cxxtest.cpp cachetest.cpp *.o *.so *.ll



