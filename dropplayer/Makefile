CC = gcc
CFLAGS = -O2 -Wall -Wextra $(shell pkg-config --cflags gtk+-3.0) -pthread
LDFLAGS = $(shell pkg-config --libs gtk+-3.0) -lm -lpthread -ldl -lpulse -lasound

TARGET = dropplayer

all: $(TARGET)

$(TARGET): main.c miniaudio.h
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
