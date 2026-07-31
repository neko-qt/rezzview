CC ?= gcc
PKG_CONFIG ?= pkg-config

CFLAGS = -std=c11 \
         -O3 \
         -flto \
         -march=native \
         -ffast-math \
         -fdata-sections \
         -ffunction-sections \
         -Wall -Wextra -Wpedantic \
         $(shell $(PKG_CONFIG) --cflags gtk+-3.0)

LDFLAGS = -flto \
          -Wl,--gc-sections \
          -s \
          $(shell $(PKG_CONFIG) --libs gtk+-3.0) \
          -lm

TARGET = rezzview
SRC = rezzview.c

.PHONY: all clean check-hash

all: $(TARGET)

$(TARGET): $(SRC)
	@echo "  CCLD    $@"
	@$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

check-hash: $(TARGET)
	@echo "SHA256 Hash of $(TARGET):"
	@sha256sum $(TARGET)

clean:
	@echo "  CLEAN"
	@rm -f $(TARGET)
