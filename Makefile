BIN=udpbd-server
OBJS=main.o

udpbd-server: $(OBJS)
	
	g++ -static -static-libgcc -static-libstdc++ -o $@ $^ -lws2_32

main.o: main.cpp
	g++ -Wall -c -Os main.cpp -o main.o


all: $(BIN)

clean:
	rm -f $(BIN) $(OBJS)

install: $(BIN)
	cp $(BIN) $(PS2DEV)/bin
