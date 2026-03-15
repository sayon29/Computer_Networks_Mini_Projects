#include"ksocket.h"

#include <unistd.h>      // For close function
#include <stdlib.h>      // For rand and srand    
#include <arpa/inet.h>   // For inet_pton and related functions
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>    
#include <string.h>      // For memcpy
#include <time.h>        // For time function (seeding random number generator)
#include <stdio.h>

int semid;
int shmid;
ktp_socket* sockets;

int k_errno;

//wait for semaphore
static inline void wait(int semid, int id) {
    struct sembuf op = {id, -1, 0};
    if (semop(semid, &op, 1) == -1) {
        exit(0);
    }
}

//signal semaphore
static inline void signal(int semid, int id) {
    struct sembuf op = {id, 1, 0};
    if (semop(semid, &op, 1) == -1) {
        exit(0);
    }
}

int get_sem_shm(){

    shmid = shmget(KEY, sizeof(ktp_socket)*MAX_KTP_SOCKETS, 0666);

    if (shmid == -1) {
        perror("shmget failed");
        return -1;
    }

    sockets = (ktp_socket*)shmat(shmid, NULL, 0);
    if (sockets == (void*)-1) {
        perror("shmat failed");
        return -1;
    }

    semid = semget(KEY, MAX_KTP_SOCKETS, 0666);

    if (semid == -1) {
        perror("semget failed");
        return -1;
    }

    return 0;
}

int create_UDP_socket(int index){

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(udp_fd == -1){
        perror("UDP socket creation failed");
        return -1;
    }

    sockets[index].udp_fd = udp_fd;
    return 0;
}

int k_socket(int domain, int type, int protocol){

    srand(time(NULL)); // Seed the random number generator

    if(protocol != SOCK_KTP){
        fprintf(stderr, "Error: Unsupported protocol. Only SOCK_KTP is supported.\n");
        return -1;
    }

    if(get_sem_shm() == -1){
        fprintf(stderr, "Error: Failed to fetch sem/shm.\n");
        return -1;
    }

    for(int i=0; i<MAX_KTP_SOCKETS; i++){
        wait(semid, i);
        if(sockets[i].is_free){

            sockets[i].is_free = 0;
            sockets[i].pid = getpid();

            sockets[i].send_buffer.front = 0;
            sockets[i].send_buffer.back = 0;
            sockets[i].send_buffer.count = 0;

            sockets[i].recv_buffer.front = 0;
            sockets[i].recv_buffer.back = 0;
            sockets[i].recv_buffer.count = 0;

            sockets[i].swnd.wsize = 10;
            sockets[i].swnd.front = 0;
            sockets[i].swnd.back = 0;
            sockets[i].swnd.seq_no = 1;

            sockets[i].rwnd.wsize = 10;
            sockets[i].rwnd.expected_seq = 1;
            sockets[i].rwnd.ptr = 0;

            sockets[i].no_space = 0;

            create_UDP_socket(i);

            signal(semid, i);

            return i;
        }
        signal(semid, i);
    }

    k_errno = ENOSPACE;
    return -1;

}

int k_bind(int index, const struct sockaddr *source_addr, const struct sockaddr *dest_addr){
    if(index < 0 || index >= MAX_KTP_SOCKETS){
        fprintf(stderr, "Error: Invalid socket index.\n");
        return -1;
    }

    wait(semid, index);

    if(sockets[index].pid != getpid()){
        fprintf(stderr, "Error: Socket does not belong to the calling process.\n");
        signal(semid, index);
        return -1;
    }

    // Bind the UDP socket to the source address
    if (bind(sockets[index].udp_fd, source_addr, sizeof(struct sockaddr_in)) == -1) {
        perror("UDP socket bind failed");
        signal(semid, index);
        return -1;
    }

    // Store the destination address in the shared memory
    memcpy(&sockets[index].dest_addr, dest_addr, sizeof(struct sockaddr_in));

    signal(semid, index);
    return 0;
}

ssize_t k_sendto(int index, const char* message, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen){

    if(index < 0 || index >= MAX_KTP_SOCKETS){
        fprintf(stderr, "Error: Invalid socket index.\n");
        return -1;
    }

    if(len != MESSAGE_SIZE){
        fprintf(stderr, "Error: Message length must be exactly %d bytes.\n", MESSAGE_SIZE);
        return -1;
    }

    wait(semid, index);

    if(sockets[index].pid != getpid()){
        fprintf(stderr, "Error: Socket does not belong to the calling process.\n");
        signal(semid, index);
        return -1;
    }

    if(sockets[index].dest_addr.sin_port != ((struct sockaddr_in *)dest_addr)->sin_port || sockets[index].dest_addr.sin_addr.s_addr != ((struct sockaddr_in *)dest_addr)->sin_addr.s_addr){
        fprintf(stderr, "Error: Destination address does not match the bound address.\n");
        signal(semid, index);
        k_errno = ENOTBOUND;
        return -1;
    }

    if(sockets[index].send_buffer.count == BUFFER_SIZE){
        fprintf(stderr, "Error: Send buffer is full.\n");
        signal(semid, index);
        k_errno = ENOSPACE;
        return -1;
    }

    sockets[index].send_buffer.count++;
    memcpy(sockets[index].send_buffer.buffer[sockets[index].send_buffer.back], message, MESSAGE_SIZE);
    sockets[index].send_buffer.back = (sockets[index].send_buffer.back + 1) % BUFFER_SIZE;

    signal(semid, index);
    return MESSAGE_SIZE;
}

ssize_t k_recvfrom(int index, void *message, size_t len,  int flags, struct sockaddr *source_addr, socklen_t *addrlen){

    if(index < 0 || index >= MAX_KTP_SOCKETS){
        fprintf(stderr, "Error: Invalid socket index.\n");
        return -1;
    }

    if(len != MESSAGE_SIZE){
        fprintf(stderr, "Error: Message length must be exactly %d bytes.\n", MESSAGE_SIZE);
        return -1;
    }

    wait(semid, index);

    if(sockets[index].pid != getpid()){
        fprintf(stderr, "Error: Socket does not belong to the calling process.\n");
        signal(semid, index);
        return -1;
    }

    if(sockets[index].recv_buffer.count == 0){
        fprintf(stderr, "Error: No messages to receive.\n");
        signal(semid, index);
        k_errno = ENOMESSAGE;
        return -1;
    }

    sockets[index].recv_buffer.count--;
    memcpy(message, sockets[index].recv_buffer.buffer[sockets[index].recv_buffer.front], MESSAGE_SIZE);
    sockets[index].recv_buffer.front = (sockets[index].recv_buffer.front + 1) % BUFFER_SIZE;
    sockets[index].rwnd.wsize++; // Update rwnd size as we have consumed a message

    signal(semid, index);
    return MESSAGE_SIZE;
}

int k_close(int index){
    if(index < 0 || index >= MAX_KTP_SOCKETS){
        fprintf(stderr, "Error: Invalid socket index.\n");
        return -1;
    }

    wait(semid, index);

    if(sockets[index].pid != getpid()){
        fprintf(stderr, "Error: Socket does not belong to the calling process.\n");
        signal(semid, index);
        return -1;
    }

    // Close the UDP socket
    close(sockets[index].udp_fd);

    // Mark the KTP socket as free
    sockets[index].is_free = 1;

    signal(semid, index);

    return 0;
}

int dropMessage(float p) {
    float r = (float)rand()/RAND_MAX;
    return r < p;
}