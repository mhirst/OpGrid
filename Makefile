CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc
SRC = src/og_engine.c src/og_seq.c src/og_fx.c src/og_rec.c src/og_song.c src/og_plugin.c

.PHONY: test test-c test-ui sanitize arm clean

test: test-c test-ui

test-c: build/engine_test build/seq_test build/fx_test build/clip_test build/rec_test build/song_test
	./build/engine_test build/demo.wav
	./build/seq_test build/seq_demo.wav
	./build/fx_test
	./build/clip_test
	./build/rec_test
	./build/song_test

build/song_test: $(SRC) src/og_engine.h src/og_internal.h test/song_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/song_test.c -o $@ -lm

build/rec_test: $(SRC) src/og_engine.h src/og_internal.h test/rec_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/rec_test.c -o $@ -lm

build/clip_test: $(SRC) src/og_engine.h src/og_internal.h test/clip_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/clip_test.c -o $@ -lm

build/fx_test: $(SRC) src/og_engine.h src/og_internal.h test/fx_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/fx_test.c -o $@ -lm

build/seq_test: $(SRC) src/og_engine.h src/og_internal.h test/seq_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/seq_test.c -o $@ -lm

build/engine_test: $(SRC) src/og_engine.h src/og_internal.h test/engine_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) test/engine_test.c -o $@ -lm

sanitize:
	@mkdir -p build
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/engine_test.c -o build/engine_test_san -lm
	./build/engine_test_san build/demo_san.wav
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/seq_test.c -o build/seq_test_san -lm
	./build/seq_test_san
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/fx_test.c -o build/fx_test_san -lm
	./build/fx_test_san
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/clip_test.c -o build/clip_test_san -lm
	./build/clip_test_san
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/rec_test.c -o build/rec_test_san -lm
	./build/rec_test_san
	$(CC) -O1 -g -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(SRC) test/song_test.c -o build/song_test_san -lm
	./build/song_test_san

test-ui:
	node test/ui_test.mjs

arm:
	./scripts/build.sh

clean:
	rm -rf build
