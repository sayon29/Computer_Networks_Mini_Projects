# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# Target executables
TARGETS = smserver smclient

# Default target builds both executables
all: $(TARGETS)

# Rule to build the server
smserver: smserver.c
	$(CC) $(CFLAGS) -o smserver smserver.c

# Rule to build the client
smclient: smclient.c
	$(CC) $(CFLAGS) -o smclient smclient.c

# Clean target to remove compiled binaries
clean:
	rm -f $(TARGETS)

# Phony targets to prevent conflicts with files named 'all' or 'clean'
.PHONY: all clean