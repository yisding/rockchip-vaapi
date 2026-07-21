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
SRCS    := src/rockchip_drv_video.c src/h264.c
OBJS    := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -shared $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(DRIVERDIR)/$(TARGET)

# Software-vs-VAAPI bit-exactness gate; needs an ffmpeg with vaapi support
# and Rockchip MPP hardware. See tests/validate.sh for options.
check: $(TARGET)
	tests/validate.sh

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install check clean
