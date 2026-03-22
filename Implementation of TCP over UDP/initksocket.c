#include "ksocket.h"           
#include <signal.h>
#include <pthread.h>    

char *packet_types[] = {"DATA", "ACK", "DUP_ACK"};

static int DEBUG = 1;

#define min(a,b) ((a) < (b) ? (a) : (b))

//Receiver thread
void* thread_R(void* arg) {

    fd_set readfds;
    struct timeval sel_timeout;
    struct timeval last_nospace_check;
    ktp_packet incoming_pkt;
    struct sockaddr_in sender_addr;

    last_nospace_check.tv_sec = 0;

    while(1) {

        //Bind UDP sockets for all active KTP sockets that are not yet bound
        for(int i = 0; i < MAX_KTP_SOCKETS; i++){
            wait_sem(semid, i);

            //create a socket and bind it if k_socket and k_bind have been called
            if(!sockets[i].is_free && !sockets[i].is_bound && sockets[i].bind_called==1) {

                int x;
                if((x = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                    LOG_MIN("Thread_R: Failed to create UDP socket for KTP Socket: %d\n", i);
                    sockets[i].is_free = 1;
                    signal_sem(semid, i);
                    continue;
                }
                
                sockets[i].udp_fd = x;

                if(bind(sockets[i].udp_fd, (struct sockaddr*)&sockets[i].src_addr, sizeof(sockets[i].src_addr)) < 0){

                    sockets[i].is_free = 1;
                    sockets[i].dest_addr = (struct sockaddr_in){0}; 
                    sockets[i].src_addr = (struct sockaddr_in){0};
                    sockets[i].pid = 0;
                    sockets[i].is_free = 1;
                    sockets[i].swnd.count = 0;
                    sockets[i].swnd.front = 0;
                    sockets[i].swnd.back = 0;
                
                    sockets[i].recv_buffer.count = 0;
                    sockets[i].send_buffer.count = 0;
                
                    sockets[i].no_space = 0;

                    LOG_MIN("Thread_R: Failed to bind UDP socket for KTP Socket: %d\n", i);
                    close(sockets[i].udp_fd);
                    signal_sem(semid, i);
                    continue;
                }
                
                sockets[i].is_bound = 1;
                LOG_MIN("Thread_R: Bound UDP socket for KTP Socket: %d\n", i);
            }

            signal_sem(semid, i);
        }

        FD_ZERO(&readfds);
        int max_fd = -1;

        // Populate the file descriptor set with all active UDP sockets 
        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {
            wait_sem(semid, i);
            if(!sockets[i].is_free && sockets[i].udp_fd >= 0 && sockets[i].is_bound) {
                // Verify fd is valid with fcntl
                if(fcntl(sockets[i].udp_fd, F_GETFD) != -1) {
                    FD_SET(sockets[i].udp_fd, &readfds);
                    if(sockets[i].udp_fd > max_fd) max_fd = sockets[i].udp_fd;
                    LOG_FULL("Thread_R: Added socket %d (fd=%d) to readfds\n", i, sockets[i].udp_fd);
                }
                else {
                    LOG_FULL("Thread_R: UDP socket for KTP Socket: %d is not valid\n", i);
                } 
            }
            signal_sem(semid, i);
        }

        // Set timeout to check for nospace/new sockets periodically
        sel_timeout.tv_sec = SELECT_TIMEOUT; 
        sel_timeout.tv_usec = 0;

        if (max_fd == -1) {
            LOG_FULL("Thread_R: No active UDP sockets to monitor\n");
            continue;  // Skip select() in this iteration
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &sel_timeout);

        if (activity == -1) {
            LOG_FULL("Thread_R: select failed");
            continue;
        }

        LOG_FULL("Thread_R: select() returned %d, max_fd=%d\n", activity, max_fd);

        // If activity > 0, it means one or more UDP sockets have incoming packets to read
        if (activity > 0) {
            for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

                wait_sem(semid, i);

                if(!sockets[i].is_free && sockets[i].is_bound && FD_ISSET(sockets[i].udp_fd, &readfds)) {
                    
                    socklen_t addr_len = sizeof(sender_addr);

                    ssize_t n = recvfrom(sockets[i].udp_fd, &incoming_pkt, sizeof(ktp_packet), 0, (struct sockaddr*)&sender_addr, &addr_len);

                    if(n>0) LOG_FULL("Thread_R: UDP packet arrived. Type: %s, Seq: %d, Socket: %d\n", packet_types[incoming_pkt.type], incoming_pkt.seq_no, i);

                    // Simulate unreliable link
                    if (n > 0 && dropMessage(P)) {
                        LOG_FULL("Thread_R: Packet with Type: %s, Seq: %d, Socket: %d dropped\n", packet_types[incoming_pkt.type], incoming_pkt.seq_no, i);
                        signal_sem(semid, i);
                        continue;
                    }

                    if (n > 0) {

                        if (incoming_pkt.type == DATA) {

                            uint8_t seq = incoming_pkt.seq_no;
                            uint8_t expected = sockets[i].rwnd.expected_seq;

                            // CASE 1: In-Order Data
                            if (seq == expected) {

                                LOG_MIN("Thread_R: Received in-order data packet. Seq: %d, Socket: %d\n", seq, i);
                                fflush(stdout);

                                // Store the message at the correct position in the circular buffer
                                int pos = (sockets[i].recv_buffer.back + (seq - expected)) % BUFFER_SIZE;
                                memcpy(sockets[i].recv_buffer.buffer[pos], incoming_pkt.data, MESSAGE_SIZE);
                                sockets[i].rwnd.seq_nos[pos] = seq;

                                LOG_FULL("Thread_R: Stored data for Seq: %d, Buffer Position: %d, Socket: %d\n", seq, pos, i);

                                // Slide the window and increment count for all consecutive messages now available 
                                do { 
                                    sockets[i].rwnd.has_data[sockets[i].recv_buffer.back] = 0; 
                                    sockets[i].recv_buffer.back = (sockets[i].recv_buffer.back + 1) % BUFFER_SIZE;
                                    sockets[i].rwnd.expected_seq++; 
                                    sockets[i].recv_buffer.count++; 
                                    if (sockets[i].recv_buffer.count == BUFFER_SIZE) break;
                                }while(sockets[i].rwnd.has_data[sockets[i].recv_buffer.back]);

                                sockets[i].rwnd.wsize = BUFFER_SIZE - sockets[i].recv_buffer.count; // Update rwnd size

                                if (sockets[i].rwnd.wsize == 0){
                                    sockets[i].no_space = 1; // Set flag if buffer is full 
                                    LOG_FULL("Thread_R: Receiver buffer full on Socket: %d, setting nospace flag\n", i);
                                }

                                LOG_FULL("Thread_R: Updated RecvBuffer.back: %d, Expected Seq: %d, RecvBuffer.Count: %d, Socket: %d\n", 
                                    sockets[i].recv_buffer.back, sockets[i].rwnd.expected_seq, sockets[i].recv_buffer.count, i);

                                // Send ACK for the last in-order message
                                ktp_packet ack;
                                ack.type = ACK;
                                ack.seq_no = sockets[i].rwnd.expected_seq - 1;
                                ack.rwnd_size = sockets[i].rwnd.wsize; // Piggyback rwnd size
                                
                                LOG_FULL("Thread_R: Sending ACK for seq no %d with rwnd size %d on socket %d\n", ack.seq_no, ack.rwnd_size, i);

                                int x = sendto(sockets[i].udp_fd, &ack, sizeof(ktp_packet), 0, 
                                    (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));

                                if(x < 0) {
                                    LOG_FULL("Thread_R: Failed to send ACK for Seq : %d, Socket: %d\n", ack.seq_no, i);
                                }
                            }

                            // CASE 2: Out-of-Order but within receiver window
                            else if ((uint8_t)(seq - expected) < (BUFFER_SIZE - sockets[i].recv_buffer.count)) {

                                int offset = (uint8_t)(seq - expected);
                                int pos = (sockets[i].recv_buffer.back + offset) % BUFFER_SIZE;

                                LOG_MIN("Thread_R: Received out-of-order data packet, Seq: %d, Socket: %d, ExpectedSeq: %d\n", seq, i, expected);

                                // Store the message but don't ACK or increment count yet
                                // If condition is true, it means this seq no has not been received before (if false, it's a duplicate packet which we can ignore)
                                if (sockets[i].rwnd.has_data[pos] == 0) {
                                    memcpy(sockets[i].recv_buffer.buffer[pos], incoming_pkt.data, MESSAGE_SIZE);
                                    sockets[i].rwnd.seq_nos[pos] = seq;
                                    sockets[i].rwnd.has_data[pos] = 1;
                                }

                                LOG_FULL("Thread_R: Stored out-of-order data for Seq: %d, Buffer Position: %d, Socket: %d\n", seq, pos, i);

                            }
                        } 
                        // ACK and DUP_ACK handling
                        else if (incoming_pkt.type == ACK || incoming_pkt.type == DUP_ACK) {

                            // Update sender's view of receiver's window size
                            sockets[i].swnd.wsize = incoming_pkt.rwnd_size;

                            // Cumulative ACK: Remove all messages covered by this ACK/DUP_ACK
                            LOG_FULL("Thread_R: Received ACK, Seq: %d, RwndSize: %d, Socket: %d\n", incoming_pkt.seq_no, incoming_pkt.rwnd_size, i);

                            //Slide the sender window for all messages acknowledged by this ACK
                            while (sockets[i].swnd.count > 0) {
                                int f = sockets[i].swnd.front;
                                uint8_t diff = (uint8_t)(incoming_pkt.seq_no - sockets[i].swnd.seq_nos[f]);

                                if (diff <= 128) { 
                                    sockets[i].swnd.front = (sockets[i].swnd.front + 1) % BUFFER_SIZE;
                                    sockets[i].swnd.count--;
                                    sockets[i].send_buffer.front = (sockets[i].send_buffer.front + 1) % BUFFER_SIZE;
                                    sockets[i].send_buffer.count--;
                                } else {
                                    break;
                                }
                            }

                            LOG_FULL("Thread_R: Updated, Swnd.front: %d, Swnd.count: %d, Socket: %d\n", sockets[i].swnd.front, sockets[i].swnd.count, i);
                            
                        }
                    }

                }
                signal_sem(semid, i);
            }
        }

        struct timeval now;
        gettimeofday(&now, NULL);

        double elapsed = (now.tv_sec - last_nospace_check.tv_sec) +
                 (now.tv_usec - last_nospace_check.tv_usec) / 1000000.0;

        if(elapsed >= NO_SPACE_TIMEOUT) {
            last_nospace_check = now;
            //Check for recovered space (nospace flag logic)
            for(int i = 0; i < MAX_KTP_SOCKETS; i++) {
                wait_sem(semid, i);
                // Send dupacks when recv buffer is empty 
                if (!sockets[i].is_free && sockets[i].no_space && sockets[i].recv_buffer.count == 0) {
                    // Space became available! Send Duplicate ACK with new rwnd 
                    ktp_packet dup_ack;
                    dup_ack.type = DUP_ACK;
                    dup_ack.seq_no = sockets[i].rwnd.expected_seq - 1;
                    dup_ack.rwnd_size = BUFFER_SIZE - sockets[i].recv_buffer.count;

                    LOG_FULL("Thread_R: Sending DUP_ACK, RwndSize: %d, Socket: %d\n", dup_ack.rwnd_size, i);
                    
                    int x = sendto(sockets[i].udp_fd, &dup_ack, sizeof(ktp_packet), 0, (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));

                    if(x < 0) {
                        LOG_FULL("Thread_R: Failed to send DUP_ACK for Socket: %d\n", i);
                    }
                }
                signal_sem(semid, i);
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
        LOG_FULL("Thread_S: Timeout occured on packet with, Front: %d, Elapsed: %f, Socket: %d\n", sock->swnd.front, elapsed, index);
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

            wait_sem(semid, i); // Lock entry

            if (!sockets[i].is_free && sockets[i].is_bound && check_timeout(i, &sockets[i])) {

                // Retransmit all the messages in swnd
                LOG_MIN("Thread_S: Retransmitting messages with, Front: %d, Count: %d, Socket: %d\n", sockets[i].swnd.front, sockets[i].swnd.count, i);
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

                    LOG_FULL("Thread_S: Retransmitting packet with, Seq: %d, Socket: %d\n", pkt.seq_no, i);

                    int x = sendto(sockets[i].udp_fd, &pkt, sizeof(ktp_packet), 0, 
                           (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));

                    if(x<0) {
                        LOG_FULL("Thread_S: sendto failed during retransmission\n");
                    }
                }
            }

            signal_sem(semid, i);

        }
        
        // New Transmissions

        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

            wait_sem(semid, i); // Lock entry 

            if (!sockets[i].is_free && sockets[i].swnd.count < sockets[i].send_buffer.count && sockets[i].is_bound) {

                //Only send new messages 
                int c = min(sockets[i].swnd.wsize - sockets[i].swnd.count,
                    sockets[i].send_buffer.count - sockets[i].swnd.count);

                LOG_MIN("Thread_S: Sending new messages, SendBufferCount: %d, SendBufferFront: %d, SendBufferBack: %d, Socket: %d\n", 
                    sockets[i].send_buffer.count, sockets[i].send_buffer.front, sockets[i].send_buffer.back, i);

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

                    LOG_FULL("Thread_S: Sending packet with, Seq: %d, Socket: %d, Port: %d\n", 
                        pkt.seq_no, i, ntohs(sockets[i].dest_addr.sin_port));

                    int x = sendto(sockets[i].udp_fd, &pkt, sizeof(ktp_packet), 0, 
                           (struct sockaddr*)&sockets[i].dest_addr, sizeof(sockets[i].dest_addr));

                    if(x < 0) {
                        LOG_FULL("Thread_S: sendto failed during new transmission\n");
                    }

                }
                sockets[i].swnd.back = (sockets[i].swnd.back + c) % BUFFER_SIZE;
                sockets[i].swnd.count += c;
            }

            signal_sem(semid, i);
        }
    }
}

void garbage_collector() {

    while(1) {

        sleep(T); // Wake up periodically

        for(int i = 0; i < MAX_KTP_SOCKETS; i++) {

            wait_sem(semid, i); // Lock entry 

            if (!sockets[i].is_free && sockets[i].pid > 0) {
                // kill(pid, 0) checks if process exists without sending a signal
                if (kill(sockets[i].pid, 0) == -1 && errno == ESRCH) {
                    LOG_MIN("GC: Cleaning up orphaned Socket: %d, PID: %d\n", i, sockets[i].pid);
                
                    close(sockets[i].udp_fd);
                
                    // FULL RESET
                    sockets[i].pid = 0;
                    sockets[i].is_free = 1;
                    sockets[i].swnd.count = 0;
                    sockets[i].swnd.front = 0;
                    sockets[i].swnd.back = 0;
                
                    sockets[i].recv_buffer.count = 0;
                    sockets[i].send_buffer.count = 0;
                
                    sockets[i].no_space = 0;

                    sockets[i].dest_addr = (struct sockaddr_in){0}; 
                    sockets[i].src_addr = (struct sockaddr_in){0};
                }
            }

            signal_sem(semid, i);
        }
    }
}

int main() {

    shmctl(shmget(KEY, 0, 0), IPC_RMID, NULL);
    semctl(semget(KEY, 0, 0), 0, IPC_RMID);

    //Create Shared Memory using KEY
    shmid = shmget(KEY, sizeof(ktp_socket) * MAX_KTP_SOCKETS, IPC_CREAT | 0666);

    if(shmid < 0) {
        perror("shmget failed");
        exit(1);
    }

    sockets = (ktp_socket*)shmat(shmid, NULL, 0);

    if(sockets == (void*)-1) {
        perror("shmat failed");
        exit(1);
    }

    //Initialize shm (set all is_free = 1)
    for(int i=0; i<MAX_KTP_SOCKETS; i++) sockets[i].is_free = 1;

    //Create Semaphores for mutual exclusion 
    semid = semget(KEY, MAX_KTP_SOCKETS, IPC_CREAT | 0666);

    if(semid < 0) {
        perror("semget failed");
        exit(1);
    }

    // Use semctl to initialize semaphore values to 1
    union semun arg;
    unsigned short values[MAX_KTP_SOCKETS] = {1,1,1,1,1,1,1,1,1,1};
    arg.array = values;
    semctl(semid, 0, SETALL, arg);

    if(semctl(semid, 0, GETALL, arg) < 0) {
        perror("semctl GETALL failed");
        exit(1);
    }

    for(int i = 0; i < MAX_KTP_SOCKETS; i++) {
        wait_sem(semid, i);
        
        // Mark as free
        sockets[i].is_free = 1;
        sockets[i].pid = 0;
        
        // Initialize all sent_time values to 0
        for(int j = 0; j < BUFFER_SIZE; j++) {
            sockets[i].swnd.sent_time[j].tv_sec = 0;
            sockets[i].swnd.sent_time[j].tv_usec = 0;
        }
        
        signal_sem(semid, i);
    }
    
    LOG_MIN("Shared memory and semaphores initialized. Starting threads and garbage collector.\n");

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