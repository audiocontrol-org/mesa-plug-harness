CXX       = g++
CC        = gcc
CXXFLAGS  = -Wall -Wextra -g -std=c++17 -Isrc
CFLAGS    = -Wall -Wextra -g -Isrc

TARGET    = s3k-client

# Client sources
SRC       = src/main.cpp src/s3k_client.cpp src/scsi_midi.cpp \
            src/akai_sysex.cpp src/scsi_bridge.cpp
OBJECTS   = $(SRC:src/%.cpp=build/%.o)

.PHONY: all clean

all: $(TARGET)

build:
	mkdir -p build

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf build $(TARGET)
