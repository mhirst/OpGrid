CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc
SRC = src/og_engine.c src/og_plugin.c

.PHONY: test test-c test-ui sanitize arm clean

test: test-c test-ui

test-c: build/engine_test
	./build/engine_test build/demo.wav

build/engine_test: $(SRC) src/og_engine.h test/engine_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/engine_test.c -o $@ -lm

sanitize:
	@mkdir -p build
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/engine_test.c -o build/engine_test_san -lm
	./build/engine_test_san build/demo_san.wav

test-ui:
	node test/ui_test.mjs

arm:
	./scripts/build.sh

clean:
	rm -rf build
