CXX = g++
CXXFLAGS = -std=c++14 -O2 -I./inc
LDFLAGS = -lpthread

all:
	$(CXX) $(CXXFLAGS) -o optronic_node main.cpp $(LDFLAGS)

clean:
	rm -f optronic_node *.o
