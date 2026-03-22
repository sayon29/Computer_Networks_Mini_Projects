#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "ksocket.h"

#define MSG_SIZE 512

static int DEBUG = 1;

int main(int argc, char* argv[]) {

    if(argc != 5) {
        printf("Usage: %s <src_port> <dest_port> <src_ip> <dest_ip>\n", argv[0]);
        exit(1);
    }

    int src_port  = atoi(argv[1]);
    int dest_port = atoi(argv[2]);
    char* src_ip  = argv[3];
    char* dest_ip = argv[4];

    int sock = k_socket(0, 0, SOCK_KTP);
    if(sock < 0){
        printf("k_socket failed\n");
        exit(1);
    }

    struct sockaddr_in src_addr, dest_addr;

    memset(&src_addr,0,sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(src_port);
    src_addr.sin_addr.s_addr = inet_addr(src_ip);

    memset(&dest_addr,0,sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    dest_addr.sin_addr.s_addr = inet_addr(dest_ip);

    if(k_bind(sock,(struct sockaddr_in*)&src_addr,(struct sockaddr_in*)&dest_addr) < 0){
        LOG_FULL("k_bind failed\n");
        exit(1);
    }

    sleep(3);
    LOG_MIN("Sender ready\n");

    FILE *fptr = fopen("input.txt", "r");
    if(!fptr){
        LOG_FULL("Failed to open input.txt\n");
        exit(1);
    }

    char msg[MSG_SIZE];

    int line_no = 1;

    while(fgets(msg, MSG_SIZE, fptr) != NULL){

        msg[strcspn(msg, "\n")] = 0;

        int n;
        do{
            n = k_sendto(sock, msg, MSG_SIZE, 0,
                         (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            if(n < 0){
                LOG_FULL("k_sendto error %d\n", k_errno);
                if(k_errno == ENOTBOUND){
                    LOG_MIN("Destination address not bound. Exiting.\n");
                    fclose(fptr);
                    k_close(sock);
                    exit(1);
                }
                sleep(1);
            }
        }while(n < 0);

        LOG_MIN("Sent line no: %d\n", line_no);
        LOG_FULL("Sent Message: %s\n", msg);

        line_no++;
    }

    // send end signal
    char end_msg[] = "~";

    int n;
    do{
        n = k_sendto(sock, end_msg, MSG_SIZE, 0,
                     (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if(n < 0){
            LOG_FULL("k_sendto error %d\n", k_errno);
            sleep(1);
        }
    }while(n < 0);

    LOG_FULL("End signal sent\n");

    sleep(15); // Sleep to allow for retransmissions if needed before cleaning up and exiting

    LOG_MIN("Sender exiting\n");

    fclose(fptr);
    k_close(sock);
}