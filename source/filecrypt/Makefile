CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null)
CPPFLAGS += $(OPENSSL_CFLAGS)
LDLIBS += $(OPENSSL_LIBS)

ifeq ($(strip $(OPENSSL_LIBS)),)
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(strip $(OPENSSL_PREFIX)),)
CPPFLAGS += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
LDLIBS += -lcrypto
endif

TARGET = filecrypt
OBJECTS = filecrypt.o crypto.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@ $(LDLIBS)

filecrypt.o: filecrypt.c crypto.h
crypto.o: crypto.c crypto.h

clean:
	rm -f $(TARGET) $(OBJECTS)
