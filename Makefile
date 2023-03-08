BIN=udpbd-server
OBJS=main.o

udpbd-server: $(OBJS)
	g++ -o $@ $^

all: $(BIN)

clean:
	rm -f $(BIN) $(OBJS)

install: $(BIN)
	cp $(BIN) $(PS2DEV)/bin
