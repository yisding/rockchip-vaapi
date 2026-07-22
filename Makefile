CC        ?= gcc
PREFIX    ?= /usr
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
LIBDIR    ?= $(PREFIX)/lib$(if $(MULTIARCH),/$(MULTIARCH))
DRIVERDIR ?= $(LIBDIR)/dri

CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC \
           $(shell pkg-config --cflags libva 2>/dev/null) \
           -I/usr/include/rockchip
LDLIBS  := $(shell pkg-config --libs libva 2>/dev/null) \
           -lrockchip_mpp -lpthread

TARGET  := rockchip_drv_video.so
SRCS    := src/rockchip_drv_video.c src/h264.c src/frame_layout.c
OBJS    := $(SRCS:.c=.o)
UNIT_TEST := tests/frame_layout_test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -shared $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

src/rockchip_drv_video.o: src/frame_layout.h src/h264.h
src/frame_layout.o: src/frame_layout.h
src/h264.o: src/h264.h

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(DRIVERDIR)/$(TARGET)

# Software-vs-VAAPI bit-exactness gate; needs an ffmpeg with vaapi support
# and Rockchip MPP hardware. See tests/validate.sh for options.
check: $(TARGET) test
	tests/validate.sh

$(UNIT_TEST): tests/frame_layout_test.c src/frame_layout.c src/frame_layout.h
	$(CC) $(CFLAGS) -Isrc tests/frame_layout_test.c src/frame_layout.c -o $@

test: $(UNIT_TEST)
	./$(UNIT_TEST)

clean:
	rm -f $(OBJS) $(TARGET) $(UNIT_TEST)

.PHONY: all install check test clean
