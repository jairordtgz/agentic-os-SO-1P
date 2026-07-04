CC=gcc

CFLAGS=-Wall -Iinclude

LDFLAGS_WINDOW = -lX11
LDFLAGS_IALEARNER = -lpthread

BIN=bin

all: $(BIN)/launcher \
     $(BIN)/window \
     $(BIN)/ialearner

$(BIN)/launcher: src/launcher.c
	mkdir -p $(BIN)
	$(CC) $(CFLAGS) src/launcher.c -o $(BIN)/launcher

$(BIN)/window: src/window.c src/socket_utils.c
	mkdir -p $(BIN)
	$(CC) $(CFLAGS) src/window.c src/socket_utils.c -o $(BIN)/window $(LDFLAGS_WINDOW)

$(BIN)/ialearner: src/ialearner.c src/classifier.c
	mkdir -p $(BIN)
	$(CC) $(CFLAGS) src/ialearner.c src/classifier.c -o $(BIN)/ialearner $(LDFLAGS_IALEARNER)

clean:
	rm -rf $(BIN)
