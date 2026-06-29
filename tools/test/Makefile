# Host-side tests. Each suite compiles the real, unmodified library source
# against a Pebble SDK shim and runs natively.
#
#   make test    # build and run all suites
#   make clean
#
# Suites:
#   storage — src/c/lib/storage/storage.c (pebble.h shim + fake phone)
#   locale  — src/c/lib/locale/locale.c + the app's strings.c (pure standard C)

CC      ?= cc
ROOT     = ../..
# -Wno-unused-parameter: the libraries keep unused SDK callback params for ABI
# clarity; that is a property of the shipping code, not the harness.
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g
INCLUDES = -I. -I$(ROOT)/src

STORAGE_SRCS = test_storage.c fake_pebble.c $(ROOT)/src/c/lib/storage/storage.c
STORAGE_BIN  = run_storage_tests

# locale.c and strings.c need no SDK shim; add the app dir so test_locale.c and
# strings.c resolve "strings.h".
LOCALE_INCLUDES = -I. -I$(ROOT)/src -I$(ROOT)/src/c/app
LOCALE_SRCS  = test_locale.c $(ROOT)/src/c/lib/locale/locale.c $(ROOT)/src/c/app/strings.c
LOCALE_BIN   = run_locale_tests

.PHONY: test clean
test: $(STORAGE_BIN) $(LOCALE_BIN)
	./$(STORAGE_BIN)
	./$(LOCALE_BIN)

$(STORAGE_BIN): $(STORAGE_SRCS) pebble.h fake_pebble.h $(ROOT)/src/c/lib/storage/storage.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $(STORAGE_BIN) $(STORAGE_SRCS)

$(LOCALE_BIN): $(LOCALE_SRCS) $(ROOT)/src/c/lib/locale/locale.h $(ROOT)/src/c/app/strings.h
	$(CC) $(CFLAGS) $(LOCALE_INCLUDES) -o $(LOCALE_BIN) $(LOCALE_SRCS)

clean:
	rm -f $(STORAGE_BIN) $(LOCALE_BIN)
