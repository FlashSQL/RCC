CC=g++
CFLAGS=-Wall -g -std=c++0x -fno-omit-frame-pointer

.SUFFIXES: .o .cpp .h

SRC_DIRS = ./ ./benchmarks/ ./concurrency_control/ ./storage/ ./system/
INCLUDE = -I. -I./benchmarks -I./concurrency_control -I./storage -I./system

CFLAGS += $(INCLUDE) -I./tbb/include -D NOGRAPHITE=1 -O3 -Wno-unused-variable
LDFLAGS = -Wall -L. -L./libs -L./tbb/lib -Wl,-rpath,'$$ORIGIN/tbb/lib' -pthread -g -lrt -std=c++0x -O3 -ljemalloc -ltbb -lnuma
LDFLAGS += $(CFLAGS)

CPPS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)*.cpp))
OBJS = $(CPPS:.cpp=.o)
EXTRA_CC = silo/allocator.cc silo/core.cc silo/counter.cc silo/rcu.cc silo/ticker.cc masstree/straccum.cc masstree/string.cc
EXTRA_OBJS = $(EXTRA_CC:.cc=.o)
DEPS = $(CPPS:.cpp=.d)

all:rundb

rundb : $(OBJS) $(EXTRA_OBJS)
	$(CC) -no-pie -o $@ $^ $(LDFLAGS)
	#$(CC) -o $@ $^ $(LDFLAGS)

-include $(OBJS:%.o=%.d)

%.d: %.cpp
	$(CC) -MM -MT $*.o -MF $@ $(CFLAGS) $<

%.o: %.cpp
	$(CC) -c $(CFLAGS) -o $@ $<

%.o: %.cc
	$(CC) -c $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f rundb $(OBJS) $(EXTRA_OBJS) $(DEPS)
