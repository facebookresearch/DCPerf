all: syscall

SOURCES:=syscall.cpp
OBJS:=$(patsubst %.cpp, %.o, $(SOURCES))

CXXFLAGS=-O2
LDFLAGS=
LDLIBS=-pthread
LDLIBS+=$(shell pkg-config --libs gflags)
TARGET=syscall

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)


clean:
	rm -rf $(TARGET) *.o
