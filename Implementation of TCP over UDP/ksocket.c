#include"ksocket.h"

int k_errno;
int semid;
int shmid;
ktp_socket* sockets;

static int initialized = 0; // Flag to check if sem/shm have been initialized
static int seeded = 0;      // Flag to check if random number generator has been seeded
static int DEBUG = 1;      // Debug level for logging

//wait semaphore
void wait_sem(int semid, int id) {
    struct sembuf op = {id, -1, 0};
    if (semop(semid, &op, 1) == -1) {
        exit(0);
    }
}

//signal semaphore
void signal_sem(int semid, int id) {
    struct sembuf op = {id, 1, 0};
    if (semop(semid, &op, 1) == -1) {
        exit(0);
    }
}

// Function to fetch shared memory and semaphore, and attach to them
int get_sem_shm(){

    if(initialized) return 0; //only initialize once

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

    initialized = 1;
    return 0;
}

// Create a KTP socket and return its index in the shared memory array
int k_socket(int domain, int type, int protocol){

    if(!seeded){
        srand(time(NULL));
        seeded = 1;
    }

    if(protocol != SOCK_KTP){
        LOG_FULL("Error: Unsupported protocol. Only SOCK_KTP is supported.\n");
        return -1;
    }

    if(get_sem_shm() == -1){
        LOG_FULL("Error: Failed to fetch sem/shm.\n");
        return -1;
    }

    // Find a free socket slot in shared memory
    for(int i=0; i<MAX_KTP_SOCKETS; i++){
        k_errno = 0;
        wait_sem(semid, i);
        if(sockets[i].is_free){

            // Initialize udp_fd FIRST
            sockets[i].is_bound = 0;
            sockets[i].udp_fd = -1; // Will be set when bound
            
            sockets[i].is_free = 0;
            sockets[i].pid = getpid();

            sockets[i].send_buffer.front = 0;
            sockets[i].send_buffer.back = 0;
            sockets[i].send_buffer.count = 0;

            sockets[i].recv_buffer.front = 0;
            sockets[i].recv_buffer.back = 0;
            sockets[i].recv_buffer.count = 0;

            sockets[i].swnd.wsize = BUFFER_SIZE;
            sockets[i].swnd.front = 0;
            sockets[i].swnd.back = 0;
            sockets[i].swnd.seq_no = 1;

            sockets[i].rwnd.wsize = BUFFER_SIZE;
            sockets[i].rwnd.expected_seq = 1;
            
            sockets[i].no_space = 0;
            sockets[i].bind_called = 0;

            for(int j = 0; j < BUFFER_SIZE; j++) {
                sockets[i].swnd.sent_time[j].tv_sec = 0;
                sockets[i].swnd.sent_time[j].tv_usec = 0;
            }

            for(int j=0; j<BUFFER_SIZE; j++) {
                sockets[i].rwnd.seq_nos[j] = 0;
                sockets[i].rwnd.has_data[j] = 0; 
            }

            for(int j=0;j<BUFFER_SIZE;j++) sockets[i].rwnd.seq_nos[j] = (uint8_t)-1;
            LOG_MIN("Created KTP socket with Index: %d, PID: %d\n", i, getpid());
            signal_sem(semid, i);

            return i;
        }
        signal_sem(semid, i);
    }

    LOG_MIN("Error: Maximum number of KTP sockets reached.\n");
    k_errno = ENOSPACE;
    return -1;
}

// Stores the source and destination addresses in the KTP socket's shared memory segment
int k_bind(int index, struct sockaddr_in *src_addr, struct sockaddr_in *dest_addr){
    if(index < 0 || index >= MAX_KTP_SOCKETS){
        LOG_FULL("Error: Invalid socket index.\n");
        return -1;
    }

    wait_sem(semid, index);

    if(sockets[index].pid != getpid()){
        LOG_FULL("Error: Socket does not belong to the calling process.\n");
        signal_sem(semid, index);
        return -1;
    }

    if(sockets[index].is_free){
        LOG_FULL("Error: Socket is not allocated.\n");
        signal_sem(semid, index);
        return -1;
    }

    if(sockets[index].is_bound){
        LOG_FULL("Error: Socket is already bound.\n");
        signal_sem(semid, index);
        return -1;
    }

    sockets[index].src_addr = *((struct sockaddr_in *)src_addr);
    sockets[index].dest_addr = *((struct sockaddr_in *)dest_addr);
    sockets[index].bind_called = 1;

    LOG_MIN("Socket bound with ksockfd: %d src_port: %d dest_port: %d\n", index, ntohs(src_addr->sin_port), ntohs(dest_addr->sin_port));

    signal_sem(semid, index);
    return 0;
}

// Adds the message to the send buffer of the KTP socket if the destination address matches and there is space in the buffer
ssize_t k_sendto(int index, const char* message, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen){

    if(index < 0 || index >= MAX_KTP_SOCKETS){
        LOG_FULL("Error: Invalid socket index.\n");
        return -1;
    }

    if(len != MESSAGE_SIZE){
        LOG_FULL("Error: Message length must be exactly %d bytes.\n", MESSAGE_SIZE);
        return -1;
    }

    wait_sem(semid, index);

    if(sockets[index].dest_addr.sin_port != ((struct sockaddr_in *)dest_addr)->sin_port || sockets[index].dest_addr.sin_addr.s_addr != ((struct sockaddr_in *)dest_addr)->sin_addr.s_addr){
        LOG_MIN("Error: Destination address does not match the bound address.\n");
        signal_sem(semid, index);
        k_errno = ENOTBOUND;
        return -1;
    }

    if(sockets[index].pid != getpid()){
        LOG_FULL("Error: Socket does not belong to the calling process.\n");
        signal_sem(semid, index);
        return -1;
    }

    if(sockets[index].send_buffer.count == BUFFER_SIZE){
        LOG_FULL("Error: Send buffer is full.\n");
        signal_sem(semid, index);
        k_errno = ENOSPACE;
        return -1;
    }

    sockets[index].send_buffer.count++;
    memcpy(sockets[index].send_buffer.buffer[sockets[index].send_buffer.back], message, MESSAGE_SIZE);

    LOG_MIN("Added message to send buffer of Socket: %d, Position: %d, SendBuffer.count: %d\n", index, sockets[index].send_buffer.back, sockets[index].send_buffer.count);

    sockets[index].send_buffer.back = (sockets[index].send_buffer.back + 1) % BUFFER_SIZE;

    signal_sem(semid, index);
    return MESSAGE_SIZE;
}

// Retrieves a message from the recv buffer of the KTP socket if there is a message available
ssize_t k_recvfrom(int index, void *message, size_t len,  int flags, struct sockaddr *source_addr, socklen_t *addrlen){

    if(index < 0 || index >= MAX_KTP_SOCKETS){
        LOG_FULL("Error: Invalid socket index.\n");
        return -1;
    }

    if(len != MESSAGE_SIZE){
        LOG_FULL("Error: Message length must be exactly %d bytes.\n", MESSAGE_SIZE);
        return -1;
    }

    wait_sem(semid, index);

    if(sockets[index].is_bound != 1){
        LOG_FULL("Error: Socket is not bound to an address.\n");
        signal_sem(semid, index);
        k_errno = ENOTBOUND;
        return -1;
    }

    if(sockets[index].recv_buffer.count == 0){
        k_errno = ENOMESSAGE;
        LOG_FULL("No messages in recv buffer of Socket: %d\n", index);
        signal_sem(semid, index);
        return -1;
    }

    k_errno = 0;
    
    sockets[index].recv_buffer.count--;
    memcpy(message, sockets[index].recv_buffer.buffer[sockets[index].recv_buffer.front], MESSAGE_SIZE);

    LOG_MIN("Read message from recv buffer of Socket: %d, Position: %d, RecvBuffer.count: %d\n", index, sockets[index].recv_buffer.front, sockets[index].recv_buffer.count);

    sockets[index].recv_buffer.front = (sockets[index].recv_buffer.front + 1) % BUFFER_SIZE;
    sockets[index].rwnd.wsize = BUFFER_SIZE - sockets[index].recv_buffer.count;

    LOG_FULL("Updated RwndSize: %d, Socket: %d\n", sockets[index].rwnd.wsize, index);

    signal_sem(semid, index);
    return MESSAGE_SIZE;
}

// Frees up the KTP socket by marking it as free in the shared memory segment
int k_close(int index){
    if(index < 0 || index >= MAX_KTP_SOCKETS){
        LOG_FULL("Error: Invalid socket index.\n");
        return -1;
    }

    wait_sem(semid, index);

    if(sockets[index].pid != getpid()){
        LOG_FULL("Error: Socket does not belong to the calling process.\n");
        signal_sem(semid, index);
        return -1;
    }

    // Mark the KTP socket as free
    sockets[index].is_free = 1;

    LOG_MIN("Closed KTP socket with Index: %d, PID: %d\n", index, getpid());

    signal_sem(semid, index);

    return 0;
}

int dropMessage(float p) {
    float r = (float)rand()/RAND_MAX;
    return r < p;
}