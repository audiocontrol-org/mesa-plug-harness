CC        = gcc
# MUSASHI_CNF path is relative to musashi/ source directory
CFLAGS    = -Wall -Wextra -g -I. -DMUSASHI_CNF='"../m68kconf.h"'
LFLAGS    = -lm

MUSASHI   = musashi
TARGET    = plug-harness

# Musashi source files
MUSASHI_SRC = $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kdasm.c $(MUSASHI)/softfloat/softfloat.c
MUSASHI_GEN = $(MUSASHI)/m68kops.c
MUSASHI_HDR = $(MUSASHI)/m68kops.h

# Our source files
MAIN_SRC = main.c

# All objects
OBJECTS = main.o m68kcpu.o m68kdasm.o m68kops.o softfloat.o

PLUG_BIN  = ../sheepshaver-data/mesa_scsi_plug.bin

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LFLAGS) -o $@ $^

main.o: main.c m68kconf.h $(MUSASHI_HDR)
	$(CC) $(CFLAGS) -c -o $@ main.c

m68kcpu.o: $(MUSASHI)/m68kcpu.c $(MUSASHI_HDR) m68kconf.h
	$(CC) $(CFLAGS) -c -o $@ $<

m68kdasm.o: $(MUSASHI)/m68kdasm.c m68kconf.h
	$(CC) $(CFLAGS) -c -o $@ $<

m68kops.o: $(MUSASHI_GEN) m68kconf.h
	$(CC) $(CFLAGS) -c -o $@ $<

softfloat.o: $(MUSASHI)/softfloat/softfloat.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Generate Musashi opcode tables
$(MUSASHI_GEN) $(MUSASHI_HDR): $(MUSASHI)/m68kmake
	cd $(MUSASHI) && ./m68kmake

$(MUSASHI)/m68kmake: $(MUSASHI)/m68kmake.c
	$(CC) -o $@ $<

clean:
	rm -f $(TARGET) $(OBJECTS) $(MUSASHI)/m68kmake $(MUSASHI_GEN) $(MUSASHI_HDR)

run: $(TARGET)
	./$(TARGET) $(PLUG_BIN)
