# Host-side tests for the synced storage library. Compiles the real, unmodified
# src/c/lib/storage/storage.c against a Pebble SDK shim + fake phone (see
# pebble.h / fake_pebble.c) and runs the pump scenarios in test_storage.c.
#
#   make test    # build and run
#   make clean

CC      ?= cc
ROOT     = ../..
# -Wno-unused-parameter: the library keeps unused SDK callback params for ABI
# clarity; that is a property of the shipping code, not the harness.
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g
INCLUDES = -I. -I$(ROOT)/src

SRCS = test_storage.c fake_pebble.c $(ROOT)/src/c/lib/storage/storage.c
BIN  = run_storage_tests

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) pebble.h fake_pebble.h $(ROOT)/src/c/lib/storage/storage.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN) $(SRCS)

clean:
	rm -f $(BIN)
