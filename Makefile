CC=clang
ARCHFLAGS=-arch x86_64
#CFLAGS=-O4
CFLAGS=-gfull
CXX=llvm-g++
CXXFLAGS=-gfull
#CXXFLAGS=-O4
LDFLAGS=

gc.o: gc.cpp gc.h
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) -c -o $@ $<

clean:
	rm -rf *.o
