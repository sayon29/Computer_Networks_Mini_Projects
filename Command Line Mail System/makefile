# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# Target executables
TARGETS = smserver smclient

# Default target 
all: $(TARGETS)

# Rule to build the server
smserver: smserver.c
	$(CC) $(CFLAGS) -o smserver smserver.c

# Rule to build the client
smclient: smclient.c
	$(CC) $(CFLAGS) -o smclient smclient.c

# Clean target
clean:
	rm -f $(TARGETS)

.PHONY: all clean