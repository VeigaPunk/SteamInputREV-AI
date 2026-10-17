PREFIX  ?= /usr/local
CXX     ?= g++
CC      ?= cc
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
TARGET  := steaminput-harness
MIC     := tools/ds5-mic/ds5-mic
INPUT   := tools/ds5-input/ds5-input

MIC_CFLAGS := $(CFLAGS) $(shell pkg-config --cflags opus libpulse libpulse-simple)
MIC_LIBS   := $(shell pkg-config --libs opus libpulse libpulse-simple)

.PHONY: all clean install uninstall mic input

all: $(TARGET)

mic: $(MIC)

input: $(INPUT)

$(TARGET): src/steaminput_harness.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -ldl

$(MIC): tools/ds5-mic/ds5-mic.c
	$(CC) $(MIC_CFLAGS) -o $@ $< $(MIC_LIBS)

$(INPUT): tools/ds5-input/ds5-input.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@if [ -x $(MIC) ]; then install -Dm755 $(MIC) $(DESTDIR)$(PREFIX)/bin/ds5-mic; else echo "note: build 'make mic' to install ds5-mic (needs libopus, libpulse)"; fi
	@if [ -x $(INPUT) ]; then install -Dm755 $(INPUT) $(DESTDIR)$(PREFIX)/bin/ds5-input; else echo "note: build 'make input' to install ds5-input"; fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
