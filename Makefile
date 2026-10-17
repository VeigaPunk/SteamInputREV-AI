PREFIX  ?= /usr/local
CXX     ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
TARGET  := steaminput-harness

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): src/steaminput_harness.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -ldl

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
