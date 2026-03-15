#ifndef KSOCKET_H
#define KSOCKET_H

#include <netinet/in.h>   // For sockaddr_in and related network structures
#include <sys/socket.h>   // For socket functions and types
#include <sys/time.h>     // For struct timeval
#include <stdint.h>       // For fixed-width integer types

// Constants
#define MAX_KTP_SOCKETS 10      // Maximum number of active KTP sockets
#define MESSAGE_SIZE 512        // Fixed message size in bytes 
#define BUFFER_SIZE 10          // Size of send and receive buffers (in messages)
#define T 5                     // Timeout in seconds
#define P 0.3                   // Drop probability 
#define SOCK_KTP 5              // KTP socket type
#define KEY 12345               // Shared memory key for KTP socket information

// Error Codes
#define ENOSPACE 1
#define ENOTBOUND 2
#define ENOMESSAGE 3

// Message Types for KTP Header
#define DATA 0
#define ACK 1
#define DUP_ACK 2

extern int k_errno;

// KTP Packet Structure
typedef struct {
    uint8_t seq_no;          // 8-bit sequence number
    uint8_t type;            // DATA, ACK, or DUP_ACK
    uint8_t rwnd_size;       // Piggybacked receiver window size (rwnd)
    char data[MESSAGE_SIZE]; // 512 byte payload
} ktp_packet;

// Window Data Structure

// Sender Window
typedef struct {
    int wsize;                             // Current window size (in messages)
    uint8_t seq_nos[BUFFER_SIZE];          // Sequence numbers in the window which are sent but not yet acknowledged
    struct timeval sent_time[BUFFER_SIZE]; // Time when each message in the window was sent (for timeout handling)
    uint8_t front;                         
    uint8_t back;
    uint8_t count;                           // Number of messages currently in the sender window
    uint8_t seq_no;
} ktp_swnd;

// Receiver Window
typedef struct {
    int wsize;                       // Free space in receiver buffer
    uint8_t seq_nos[BUFFER_SIZE];    // Sequence number of messages which are recieved out of order
    uint8_t expected_seq;            // Next expected in-order sequence
    uint8_t ptr;                     // Pointer to the position in seq_nos where the seq_no of the packet with exxpected_seq should be stored 
} ktp_rwnd;

// Buffer Data Structure
typedef struct {
    char buffer[BUFFER_SIZE][MESSAGE_SIZE]; // Circular buffer for messages
    uint8_t front;                                 
    uint8_t back;
    uint8_t count;                             // Number of messages currently in the buffer
} ktp_buffer;

// Shared Memory Segment for one KTP Socket
typedef struct {

    int is_free;                        // 1 if free, 0 if allotted 
    pid_t pid;                          // PID of the creating process 
    int udp_fd;                         // UDP socket descriptor
    struct sockaddr_in dest_addr;       // Destination IP and Port
    
    ktp_buffer send_buffer;
    ktp_buffer recv_buffer;

    ktp_swnd swnd;
    ktp_rwnd rwnd;
    
    int no_space;                      // Flag if rwnd is 0

} ktp_socket;

// Function Prototypes
int k_socket(int domain, int type, int protocol);

int k_bind(int index, const struct sockaddr *src_addr, const struct sockaddr *dest_addr);

ssize_t k_sendto(int index, const char* message, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);

ssize_t k_recvfrom(int index, void *message, size_t len,  int flags, struct sockaddr *source_addr, socklen_t *addrlen);

int k_close(int index);

int dropMessage(float p);

#endif