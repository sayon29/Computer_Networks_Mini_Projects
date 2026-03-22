CC = gcc
CFLAGS = -Wall -g -pthread

LIB = libksocket.a
LIB_OBJ = ksocket.o

# Default network config
SRC_IP = 127.0.0.1
DEST_IP = 127.0.0.1

SRC_PORT = 40115
DEST_PORT = 40215

all: initksocket user1 user2

# Build static library
$(LIB): $(LIB_OBJ)
	ar rcs $(LIB) $(LIB_OBJ)

# Compile library object
ksocket.o: ksocket.c ksocket.h
	$(CC) $(CFLAGS) -c ksocket.c

# Build init program
initksocket: initksocket.c $(LIB)
	$(CC) $(CFLAGS) initksocket.c -o initksocket -L. -lksocket

# Build user1 (formerly sender)
user1: user1.c $(LIB)
	$(CC) $(CFLAGS) user1.c -o user1 -L. -lksocket

# Build user2 (formerly receiver)
user2: user2.c $(LIB)
	$(CC) $(CFLAGS) user2.c -o user2 -L. -lksocket

# Run initialization
runinit: initksocket
	./initksocket

# Run one user1 and one user2
runuser: user1 user2
	gnome-terminal -- bash -c "./user2 $(DEST_PORT) $(SRC_PORT) $(DEST_IP) $(SRC_IP) 1; exec bash"
	gnome-terminal -- bash -c "./user1 $(SRC_PORT) $(DEST_PORT) $(SRC_IP) $(DEST_IP); exec bash"

# Run 3 user1s and 3 user2s with incremented ports and output files
runuser_3: user1 user2
	# Start receivers (user2)
	for i in 0 1 2; do \
		gnome-terminal -- bash -c "./user2 $$(( $(DEST_PORT) + $$i )) $$(( $(SRC_PORT) + $$i )) $(DEST_IP) $(SRC_IP) $$(( $$i + 1 )); exec bash"; \
	done

	# Start senders (user1)
	for i in 0 1 2; do \
		gnome-terminal -- bash -c "./user1 $$(( $(SRC_PORT) + $$i )) $$(( $(DEST_PORT) + $$i )) $(SRC_IP) $(DEST_IP); exec bash"; \
	done

# Clean build files and outputs
clean:
	rm -f *.o $(LIB) user1 user2 initksocket output*.txt