# Host-side unit tests (gcc / g++). Run with: make -C tests test
CC       ?= gcc
CXX      ?= g++
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS   := -std=c11 -O1 -g -Wall -Wextra -Werror $(SAN) -I../src
CXXFLAGS := -std=c++20 -O1 -g -Wall -Wextra -Werror $(SAN) -I../src -pthread

.PHONY: test clean

test: test_drift_filter test_sync
	./test_drift_filter
	./test_sync

test_drift_filter: test_drift_filter.c ../src/drift_filter.c ../src/drift_filter.h
	$(CC) $(CFLAGS) -o $@ test_drift_filter.c ../src/drift_filter.c -lm

test_sync: test_sync.cpp ../src/sync.h ../src/drift_filter.h
	$(CXX) $(CXXFLAGS) -o $@ test_sync.cpp

clean:
	rm -f test_drift_filter test_sync
