CC=gcc
CFLAGS=-Wall -I. -lvirt

make: memory_coordinator.c
	$(CC) -o ./memory_coordinator ./memory_coordinator.c $(CFLAGS)

clean:
	rm -f ./memory_coordinator
