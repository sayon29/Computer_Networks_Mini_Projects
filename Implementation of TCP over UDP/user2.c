#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "ksocket.h"

#define MSG_SIZE 512
static int DEBUG = 1;

int main(int argc, char* argv[]) {

    if(argc != 6) {
        LOG_FULL("Usage: %s <dest_port> <src_port> <dest_ip> <src_ip> <output_index>\n", argv[0]);
        exit(1);
    }

    int dest_port = atoi(argv[1]);
    int src_port  = atoi(argv[2]);
    char* dest_ip = argv[3];
    char* src_ip  = argv[4];
    int index     = atoi(argv[5]);

    int sock = k_socket(0, 0, SOCK_KTP);
    if(sock < 0){
        LOG_FULL("k_socket failed\n");
        exit(1);
    }

    struct sockaddr_in src_addr, dest_addr;
    socklen_t addrlen = sizeof(dest_addr);

    memset(&src_addr,0,sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(dest_port);
    src_addr.sin_addr.s_addr = inet_addr(dest_ip);

    memset(&dest_addr,0,sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(src_port);
    dest_addr.sin_addr.s_addr = inet_addr(src_ip);

    if(k_bind(sock,(struct sockaddr_in*)&src_addr,(struct sockaddr_in*)&dest_addr) < 0){
        LOG_FULL("k_bind failed\n");
        exit(1);
    }

    sleep(3);
    LOG_MIN("Receiver ready\n");

    char filename[32];
    sprintf(filename, "output%d.txt", index);
    FILE *fptr = fopen(filename, "w");

    char buf[MSG_SIZE];
    int line_no = 1;

    while(1){

        memset(buf,0,MSG_SIZE);

        int n;
        do{
            n = k_recvfrom(sock,buf,MSG_SIZE,0,(struct sockaddr*)&dest_addr,&addrlen); 
            if(n > 0){
                LOG_MIN("Received line no: %d\n", line_no++);
                LOG_FULL("Received message: %s\n", buf);
                break;
            } else {
                LOG_FULL("k_recvfrom error %d\n", k_errno);
                if(k_errno == ENOTBOUND){
                    LOG_MIN("Destination address not bound. Exiting.\n");
                    fclose(fptr);
                    k_close(sock);
                    exit(1);
                }
                sleep(1);
            }
        }while(k_errno == ENOMESSAGE);

        if(strcmp(buf,"~") == 0){
            LOG_MIN("End signal received\n");
            break;
        }

        fprintf(fptr, "%s\n", buf);
        fflush(fptr);
    }

    fclose(fptr);
    k_close(sock);

    LOG_MIN("Receiver exiting\n");
}