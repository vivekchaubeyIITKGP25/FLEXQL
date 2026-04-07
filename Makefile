# FlexQL Makefile
# Usage:
#   make            - Build server, client, tests, and benchmark
#   make test       - Build and run tests
#   make benchmark  - Build the benchmark binary
#   make clean      - Remove build artifacts

CXX      = g++
CC       = gcc
AR       = ar
CXXFLAGS = -std=c++17 -O3 -march=native -DNDEBUG -Wall -Wextra \
           -funroll-loops -fomit-frame-pointer
CFLAGS   = -std=c11 -O2 -Wall
CXXDEPFLAGS = -MMD -MP
CDEPFLAGS = -MMD -MP
LDFLAGS  = -lpthread -lstdc++

ifeq ($(BUILD),debug)
    CXXFLAGS = -std=c++17 -g -O0 -fsanitize=address,undefined -Wall -Wextra
    LDFLAGS  = -lpthread -lstdc++ -fsanitize=address,undefined
endif

INCS = \
    -Iinclude/common \
    -Iinclude/network \
    -Iinclude/parser \
    -Iinclude/query \
    -Iinclude/storage \
    -Iinclude/index \
    -Iinclude/cache \
    -Iinclude/server

ENGINE_SRCS = \
    src/storage/storage.cpp \
    src/storage/durable_log.cpp \
    src/storage/persistence.cpp \
    src/index/hash_index.cpp \
    src/cache/lru_cache.cpp \
    src/parser/parser.cpp \
    src/query/executor.cpp \
    src/network/network.cpp \
    src/server/session.cpp

ENGINE_OBJS = $(ENGINE_SRCS:.cpp=.o)
TEST_OBJS = tests/test_main.o tests/test_all_cases.o
APP_OBJS = src/server/server.o src/client/repl.o src/client/flexql_client.o benchmarks/benchmark_flexql.o
CLIENT_LIB_OBJS = src/client/flexql_client.o src/network/network.o
PIC_CLIENT_OBJS = build/pic/src/client/flexql_client.o build/pic/src/network/network.o
ALL_OBJS = $(ENGINE_OBJS) $(TEST_OBJS) $(APP_OBJS)

.PHONY: all clean test benchmark dirs

all: dirs bin/flexql-server bin/flexql-client bin/flexql-test bin/flexql-test-all bin/flexql-benchmark bin/libflexql.a bin/libflexql.so

dirs:
	@mkdir -p bin build/pic

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CXXDEPFLAGS) $(INCS) -c $< -o $@

build/pic/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC $(INCS) -c $< -o $@

src/client/repl.o: src/client/repl.c
	$(CC) $(CFLAGS) $(CDEPFLAGS) $(INCS) -c $< -o $@

bin/flexql-server: $(ENGINE_OBJS) src/server/server.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

bin/flexql-client: src/client/repl.o src/client/flexql_client.o src/network/network.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

bin/flexql-test: $(ENGINE_OBJS) src/client/flexql_client.o tests/test_main.o
	$(CXX) $(CXXFLAGS) -o $@ tests/test_main.o src/client/flexql_client.o $(ENGINE_OBJS) $(LDFLAGS)

bin/flexql-test-all: $(ENGINE_OBJS) src/client/flexql_client.o tests/test_all_cases.o
	$(CXX) $(CXXFLAGS) -o $@ tests/test_all_cases.o src/client/flexql_client.o $(ENGINE_OBJS) $(LDFLAGS)

bin/flexql-benchmark: src/client/flexql_client.o src/network/network.o benchmarks/benchmark_flexql.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

bin/libflexql.a: $(CLIENT_LIB_OBJS)
	$(AR) rcs $@ $^

bin/libflexql.so: $(PIC_CLIENT_OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

test: bin/flexql-test bin/flexql-test-all
	./bin/flexql-test
	./bin/flexql-test-all

benchmark: bin/flexql-benchmark

clean:
	find src benchmarks tests \( -name "*.o" -o -name "*.d" \) -delete
	find build/pic \( -name "*.o" -o -name "*.d" \) -delete 2>/dev/null || true
	rm -f bin/flexql-server bin/flexql-client bin/flexql-test bin/flexql-test-all bin/flexql-benchmark
	rm -f bin/libflexql.a bin/libflexql.so

-include $(ALL_OBJS:.o=.d)
