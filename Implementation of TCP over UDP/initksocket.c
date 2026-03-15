#include "ksocket.h"     // Include the header file for KTP socket definitions and prototypes
#include <unistd.h>      // For close function
#include <stdlib.h>      // For rand and srand    
#include <arpa/inet.h>   // For inet_pton and related functions
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>    
#include <string.h>      // For memcpy
#include <time.h>        // For time function (seeding random number generator)
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>

#define min(a,b) ((a) < (b) ? (a) : (b))

ktp_socket* sockets;
int shmid, semid;

void* thread_R(void* arg) {

    fd_set readfds;
    struct timeval timeout;
    ktp_packet incoming_pkt;
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    while(1) {
        FD_ZERO(&readfds);
        int max_fd = -1;

        // 1. Populate the file descriptor set with all active UDP sockets 
        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {
            wait(semid, i);
            if(!sockets[i].is_free) {
                FD_SET(sockets[i].udp_fd, &readfds);
                if(sockets[i].udp_fd > max_fd) max_fd = sockets[i].udp_fd;
            }
            signal(semid, i);
        }

        // Set timeout to check for nospace/new sockets periodically
        timeout.tv_sec = 1; 
        timeout.tv_usec = 0;

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        if (activity > 0) {
            for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

                wait(semid, i);

                if(!sockets[i].is_free && FD_ISSET(sockets[i].udp_fd, &readfds)) {
                    
                    ssize_t n = recvfrom(sockets[i].udp_fd, &incoming_pkt, sizeof(ktp_packet), 0, (struct sockaddr*)&sender_addr, &addr_len);
                    
                    // Simulate unreliable link
                    if (n > 0 && dropMessage(P)) {
                        printf("Thread R: Packet dropped (Probability P)\n");
                        signal(semid, i);
                        continue;
                    }

                    if (n > 0) {
                        if (incoming_pkt.type == DATA) {
                            // if packet can be in sequence, add to recv_buffer and update rwnd

                        } 
                        else if (incoming_pkt.type == ACK || incoming_pkt.type == DUP_ACK) {
                            // Update sender window size based on piggybacked rwnd 
                            sockets[i].swnd.wsize = incoming_pkt.rwnd_size;

                            if (incoming_pkt.type == ACK) {
                                
                            }
                        }
                    }
                }
                signal(semid, i);
            }
        }
        else if (activity == 0) {
            // Timeout: Check for recovered space (nospace flag logic)
            for(int i = 0; i < MAX_KTP_SOCKETS; i++) {
                wait(semid, i);
                if (!sockets[i].is_free && sockets[i].no_space && sockets[i].recv_buffer.count < BUFFER_SIZE) {
                    // Space became available! Send Duplicate ACK with new rwnd 
                    ktp_packet dup_ack;
                    dup_ack.type = DUP_ACK;
                    dup_ack.seq_no = sockets[i].rwnd.expected_seq - 1;
                    dup_ack.rwnd_size = BUFFER_SIZE - sockets[i].recv_buffer.count;
                    
                    sendto(sockets[i].udp_fd, &dup_ack, sizeof(ktp_packet), 0, (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));
                    sockets[i].no_space = 0; // Reset flag 
                }
                signal(semid, i);
            }
        }
    }
}

int check_timeout(int index, ktp_socket* sock) {
    struct timeval now;
    gettimeofday(&now, NULL);

    if(sock->swnd.count == 0) return 0; // No messages in sender window

    int pos = sock->swnd.front; // Check the front of the sender window for timeout
    double elapsed = (now.tv_sec - sock->swnd.sent_time[pos].tv_sec) + (now.tv_usec - sock->swnd.sent_time[pos].tv_usec) / 1000000.0;

    if (elapsed >= T) {
        // Timeout occurred for this message
        printf("Timeout occured on socket %d\n", index);
        return 1;
    }

    return 0;
}

void* thread_S(void* arg) {

    while(1) {
        // Sleep for T/2 
        usleep(T*1000*1000/2);

        // Check for timeouts in all active sockets and retransmit if necessary
        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

            wait(semid, i); // Lock entry 

            if (!sockets[i].is_free && check_timeout(i, &sockets[i])) {

                // Retransmit all the messages in swnd
                printf("Retransmitting messages for socket %d\n", i);
                fflush(stdout);

                for(int j = 0; j < sockets[i].swnd.count; j++) {

                    int msg_pos = (sockets[i].swnd.front + j) % BUFFER_SIZE;

                    //Create KTP packet for retransmission
                    ktp_packet pkt;
                    pkt.type = DATA;
                    pkt.seq_no = sockets[i].swnd.seq_nos[msg_pos];
                    memcpy(pkt.data, sockets[i].send_buffer.buffer[msg_pos], MESSAGE_SIZE);

                    // Update sent_time for this message
                    gettimeofday(&sockets[i].swnd.sent_time[msg_pos], NULL);

                    sendto(sockets[i].udp_fd, &pkt, sizeof(ktp_packet), 0, 
                           (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));
                }
            }

            signal(semid, i);

        }
        
        // New Transmissions

        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

            wait(semid, i); // Lock entry 

            if (!sockets[i].is_free && sockets[i].swnd.count < sockets[i].send_buffer.count) {

                int c = min(sockets[i].swnd.wsize - sockets[i].swnd.count,
                    sockets[i].send_buffer.count - sockets[i].swnd.count);

                for(int j=0; j<c; j++) {

                    int msg_pos = (sockets[i].swnd.back + j) % BUFFER_SIZE;

                    //Create KTP packet for transmission
                    ktp_packet pkt;
                    pkt.type = DATA;
                    pkt.seq_no = sockets[i].swnd.seq_no++;
                    pkt.rwnd_size = sockets[i].rwnd.wsize; // Piggyback rwnd size
                    memcpy(pkt.data, sockets[i].send_buffer.buffer[msg_pos], MESSAGE_SIZE);

                    sockets[i].swnd.seq_nos[msg_pos] = pkt.seq_no; // Store seq_no in swnd for timeout handling

                    // Update sent_time for this message
                    gettimeofday(&sockets[i].swnd.sent_time[msg_pos], NULL);

                    sendto(sockets[i].udp_fd, &pkt, sizeof(ktp_packet), 0, 
                           (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));

                }
                sockets[i].swnd.back = (sockets[i].swnd.back + c) % BUFFER_SIZE;
                sockets[i].swnd.count += c;
            }

            signal(semid, i);
        }
    }
}

void garbage_collector() {

    while(1) {

        sleep(T*3); // Wake up periodically

        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

            wait(semid, i); // Lock entry 

            if (!sockets[i].is_free) {
                // kill(pid, 0) checks if process exists without sending a signal
                if (kill(sockets[i].pid, 0) == -1 && errno == ESRCH) {
                    printf("GC: Cleaning up orphaned socket %d (PID %d)\n", i, sockets[i].pid);
                    close(sockets[i].udp_fd);
                    sockets[i].is_free = 1;
                }
            }

            signal(semid, i);
        }
    }
}

int main() {

    //Create Shared Memory using KEY
    shmid = shmget(KEY, sizeof(ktp_socket) * MAX_KTP_SOCKETS, IPC_CREAT | 0666);
    sockets = (ktp_socket*)shmat(shmid, NULL, 0);

    //Initialize shm (set all is_free = 1)
    for(int i=0; i<MAX_KTP_SOCKETS; i++) sockets[i].is_free = 1;

    //Create Semaphores for mutual exclusion 
    semid = semget(KEY, MAX_KTP_SOCKETS, IPC_CREAT | 0666);
    
    // Use semctl to initialize semaphore values to 1
    semctl(semid, 0, SETALL, (int[]){1, 1, 1, 1, 1, 1, 1, 1, 1, 1});

    //Create Thread R and Thread S
    pthread_t tid_R, tid_S;
    pthread_create(&tid_R, NULL, thread_R, NULL);
    pthread_create(&tid_S, NULL, thread_S, NULL);

    //Fork the Garbage Collector process
    if (fork() == 0) {
        garbage_collector();
        exit(0);
    }

    pthread_join(tid_R, NULL);
    pthread_join(tid_S, NULL);

    return 0;
}