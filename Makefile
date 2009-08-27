CC=clang
CFLAGS=-O4 -arch x86_64
#CFLAGS=-gfull -arch x86_64
CXX=llvm-g++
#CXXFLAGS=-gfull -arch x86_64
CXXFLAGS=-O4 -arch x86_64
LDFLAGS=-arch x86_64

test: test.o gc.o
	$(CXX) $(LDFLAGS) -o $@ $^

test.o: test.c gc.h
	$(CC) $(CFLAGS) -c -o $@ $<

gc.o: gc.cpp gc.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf test *.o
