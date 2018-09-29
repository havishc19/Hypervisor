CC=gcc
CFLAGS=-Wall -I. -lvirt

make: vcpu_scheduler.c
	$(CC) -o ./vcpu_scheduler ./vcpu_scheduler.c $(CFLAGS)

clean:
	rm -f ./vcpu_scheduler
