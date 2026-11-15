cached: stringBuffers.o sockets.o threading.o cached.o
	gcc -g -fsanitize=address,undefined -o cached $^

verify: cached.c
	verifast $(VFFLAGS) stringBuffers.o sockets.o threading.o cached.c
