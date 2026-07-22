CC         ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr
MULTIARCH  := $(shell $(CC) -print-multiarch 2>/dev/null)
LIBDIR     ?= $(PREFIX)/lib$(if $(MULTIARCH),/$(MULTIARCH))
DRIVERDIR  ?= $(LIBDIR)/dri

CPPFLAGS ?=
CFLAGS   ?= -O2
WARNINGS ?= -Wall -Wextra -Werror
LDFLAGS  ?=
VA_CFLAGS  ?= $(shell $(PKG_CONFIG) --cflags libva 2>/dev/null)
VA_LIBS    ?= $(shell $(PKG_CONFIG) --libs libva 2>/dev/null)
MPP_CFLAGS ?= -I/usr/include/rockchip
MPP_LIBS   ?= -lrockchip_mpp
LDLIBS     ?= $(VA_LIBS) $(MPP_LIBS) -lpthread

TARGET := rockchip_drv_video.so
SRCS   := src/rockchip_drv_video.c src/object_heap.c src/h264.c \
	src/frame_layout.c src/vp9.c
OBJS   := $(SRCS:.c=.o)

UNIT_TESTS := tests/object_heap_test tests/frame_layout_test tests/h264_test \
	tests/vp9_test

VALGRIND        ?= valgrind
VALGRIND_FLAGS  ?= --quiet --error-exitcode=99 --leak-check=full \
	--show-leak-kinds=all --errors-for-leak-kinds=all --track-origins=yes
VALGRIND_PREFIX ?=

SAN_CFLAGS  ?= -O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS ?= -fsanitize=address,undefined
SAN_DIR      := tests/.san-driver
SAN_TARGET   := $(SAN_DIR)/$(TARGET)
SAN_OBJS     := $(SRCS:.c=.san.o)
SAN_TESTS    := tests/object_heap_test.san tests/frame_layout_test.san \
	tests/h264_test.san tests/vp9_test.san
TSAN_CFLAGS  ?= -O1 -g3 -fno-omit-frame-pointer -fsanitize=thread
TSAN_LDFLAGS ?= -fsanitize=thread
TSAN_TESTS   := tests/object_heap_test.tsan

DRIVER_COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fPIC \
	$(VA_CFLAGS) $(MPP_CFLAGS) -Isrc
TEST_COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(VA_CFLAGS) -Isrc
SAN_DRIVER_COMPILE = $(CC) $(CPPFLAGS) $(SAN_CFLAGS) $(WARNINGS) -fPIC \
	$(VA_CFLAGS) $(MPP_CFLAGS) -Isrc
SAN_TEST_COMPILE = $(CC) $(CPPFLAGS) $(SAN_CFLAGS) $(WARNINGS) \
	$(VA_CFLAGS) -Isrc

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(DRIVER_COMPILE) -c $< -o $@

src/rockchip_drv_video.o: src/frame_layout.h src/h264.h src/object_heap.h src/vp9.h
src/object_heap.o: src/object_heap.h
src/frame_layout.o: src/frame_layout.h
src/h264.o: src/h264.h src/bs.h
src/vp9.o: src/vp9.h

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(DRIVERDIR)/$(TARGET)

fetch-vectors:
	tests/fetch-vectors.sh

# Full hardware gates. The conformance gate remains non-green when a required
# risky vector is quarantined; see docs/TESTING.md.
check: $(TARGET) test
	TEST_SET=all tests/validate.sh

check-conformance: $(TARGET) test
	TEST_SET=conformance tests/validate.sh

check-synthetic: $(TARGET) test
	TEST_SET=synthetic tests/validate.sh

# Diagnostic subset for a kernel on which risky vectors cannot safely run.
# This is intentionally not the release gate.
check-safe: $(TARGET) test
	TEST_SET=conformance ALLOW_QUARANTINE=1 tests/validate.sh

tests/object_heap_test: tests/object_heap_test.c src/object_heap.c src/object_heap.h
	$(TEST_COMPILE) tests/object_heap_test.c src/object_heap.c -lpthread -o $@

tests/frame_layout_test: tests/frame_layout_test.c src/frame_layout.c src/frame_layout.h
	$(TEST_COMPILE) tests/frame_layout_test.c src/frame_layout.c -o $@

tests/h264_test: tests/h264_test.c src/h264.c src/h264.h src/bs.h
	$(TEST_COMPILE) tests/h264_test.c src/h264.c -o $@

tests/vp9_test: tests/vp9_test.c src/vp9.c src/vp9.h
	$(TEST_COMPILE) tests/vp9_test.c src/vp9.c -o $@

test: $(UNIT_TESTS)
	@set -e; for test_binary in $(UNIT_TESTS); do ./$$test_binary; done

test-valgrind: $(UNIT_TESTS)
	@command -v $(firstword $(VALGRIND)) >/dev/null || { \
		echo "valgrind is required" >&2; exit 1; \
	}
	@set -e; for test_binary in $(UNIT_TESTS); do \
		$(VALGRIND) $(VALGRIND_FLAGS) $(VALGRIND_PREFIX) ./$$test_binary; \
	done

$(SAN_TARGET): $(SAN_OBJS)
	mkdir -p $(SAN_DIR)
	$(CC) $(LDFLAGS) $(SAN_LDFLAGS) -shared -o $@ $^ $(LDLIBS)

src/%.san.o: src/%.c
	$(SAN_DRIVER_COMPILE) -c $< -o $@

src/rockchip_drv_video.san.o: src/frame_layout.h src/h264.h src/object_heap.h src/vp9.h
src/object_heap.san.o: src/object_heap.h
src/frame_layout.san.o: src/frame_layout.h
src/h264.san.o: src/h264.h src/bs.h
src/vp9.san.o: src/vp9.h

tests/object_heap_test.san: tests/object_heap_test.c src/object_heap.c src/object_heap.h
	$(SAN_TEST_COMPILE) tests/object_heap_test.c src/object_heap.c \
		$(SAN_LDFLAGS) -lpthread -o $@

tests/frame_layout_test.san: tests/frame_layout_test.c src/frame_layout.c src/frame_layout.h
	$(SAN_TEST_COMPILE) tests/frame_layout_test.c src/frame_layout.c \
		$(SAN_LDFLAGS) -o $@

tests/h264_test.san: tests/h264_test.c src/h264.c src/h264.h src/bs.h
	$(SAN_TEST_COMPILE) tests/h264_test.c src/h264.c $(SAN_LDFLAGS) -o $@

tests/vp9_test.san: tests/vp9_test.c src/vp9.c src/vp9.h
	$(SAN_TEST_COMPILE) tests/vp9_test.c src/vp9.c $(SAN_LDFLAGS) -o $@

test-sanitize: $(SAN_TESTS)
	@set -e; for test_binary in $(SAN_TESTS); do \
		ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 ./$$test_binary; \
	done

tests/object_heap_test.tsan: tests/object_heap_test.c src/object_heap.c src/object_heap.h
	$(CC) $(CPPFLAGS) $(TSAN_CFLAGS) $(WARNINGS) -Isrc \
		tests/object_heap_test.c src/object_heap.c $(TSAN_LDFLAGS) \
		-lpthread -o $@

test-tsan: $(TSAN_TESTS)
	@set -e; for test_binary in $(TSAN_TESTS); do ./$$test_binary; done

sanitize: $(SAN_TARGET) test-sanitize

check-sanitize: sanitize
	LD_PRELOAD="$(shell $(CC) -print-file-name=libasan.so)" \
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
	DRIVER_DIR="$(abspath $(SAN_DIR))" TEST_SET=all tests/validate.sh

check-sanitize-safe: sanitize
	LD_PRELOAD="$(shell $(CC) -print-file-name=libasan.so)" \
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
	DRIVER_DIR="$(abspath $(SAN_DIR))" TEST_SET=conformance \
	ALLOW_QUARANTINE=1 tests/validate.sh

lint:
	@command -v clang-tidy >/dev/null || { echo "clang-tidy is required" >&2; exit 1; }
	clang-tidy $(SRCS) -- $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fPIC \
		$(VA_CFLAGS) $(MPP_CFLAGS) -Isrc

clean:
	rm -f $(OBJS) $(SAN_OBJS) $(TARGET) $(UNIT_TESTS) $(SAN_TESTS) \
		$(TSAN_TESTS)
	rm -rf $(SAN_DIR)

.PHONY: all install fetch-vectors check check-conformance check-synthetic \
	check-safe test test-valgrind test-sanitize sanitize check-sanitize \
	test-tsan check-sanitize-safe lint clean
