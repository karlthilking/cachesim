CXXFLAGS=-std=c++23 -g -O3 -Wall -Wextra -Werror
CXXTESTFLAGS=$(CXXFLAGS) -Wno-ignored-qualifiers \
			 			 -Wno-deprecated-enum-float-conversion
LDFLAGS=-std=c++23
PROGRAMS=libcache.so cachetest matmul
MAKEFLAGS += --no-print-directory

SOURCE=./src
INCLUDE=./include
TEST=./test
CXXTEST=$(TEST)/cxxtest

all: $(PROGRAMS)

cachetest: cachetest.o cache.o cache.cxxtest.o
	g++ $(LDFLAGS) $^ -o $@

libcache.so: cache.o
	g++ $(LDFLAGS) -shared -fPIC -o $@ $<

cache.o: $(SOURCE)/cache.cpp
	g++ -I$(INCLUDE) $(CXXFLAGS) -fPIC -c $< -o $@

cachetest.o: cachetest.cpp
	g++ -I$(CXXTEST) $(CXXTESTFLAGS) -c $< -o $@

cache.cxxtest.o: cache.cxxtest.cpp
	g++ -I$(CXXTEST) $(CXXTESTFLAGS) -c $< -o $@

%.cxxtest.cpp: $(TEST)/%.cxxtest.hpp
	python3 -W ignore $(CXXTEST)/bin/cxxtestgen --part --error-printer \
			$< -o $@

%: ./programs/%.cpp libcache.so
	g++ $(CXXFLAGS) -L. -lcache -Wl,-rpath,. -o $@ $<

cachetest.cpp:
	python3 -W ignore $(CXXTEST)/bin/cxxtestgen --root --error-printer \
			-o $@

build: all
	@(rm -f *.o cachetest.cpp *.cxxtest.cpp)

clean:
	rm -f $(PROGRAMS) *cxxtest.cpp cachetest.cpp *.o *.so



