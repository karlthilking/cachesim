CXXFLAGS = -g3 -O0

SOURCE 		= ./src
INCLUDE 	= ./include
PROGRAMS 	= libcachesim.so cachesim
all 		= $(PROGRAMS)

%.so: $(SOURCE)/%.cpp
	g++ $(CXXFLAGS) -shared -fPIC -o $@ $<

%: $(SOURCE)/%.cpp
	g++ $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMS) *.o
