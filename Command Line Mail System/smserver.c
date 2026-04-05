#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdarg.h>

#define MAX_CONCURRENT_CLIENTS 50
#define MAX_USERNAME_LEN 35
#define MAX_PASS_LEN 35
#define MAX_USERS 30
#define MAX_SUBJECT_LEN 512
#define MAX_BODY_LEN 65536
#define TIMEOUT_SEC 30

typedef enum {
    // SMTP2 States
    INIT,
    FROM,
    TO,
    TO_OR_SUB,
    BODY,
    BODY_CONT,
    // SMP States
    WAIT_AUTH,
    SMP_IDLE
} State;

typedef struct {
    char username[MAX_USERNAME_LEN];
    char pass[MAX_PASS_LEN];
    int nextid;
} User;

typedef struct {
    int fd;
    State state;
    time_t init_time;
    int mode; // 0 for SMTP2, 1 for SMP
    
    char in_buf[1024];
    int in_len;
    
    // SMTP2 State Variables
    char from_name[MAX_SUBJECT_LEN];
    int to_users[MAX_USERS];            //index of recipients in users[]
    int num_to;                         //number of recipients
    char subject[MAX_SUBJECT_LEN];
    char body[MAX_BODY_LEN];
    int body_len;
    
    // SMP State Variables
    char nonce[9];
    int auth_attempts;
    int user_idx; // Authenticated user index
} Client;

int listen_fd = -1;
Client clients[MAX_CONCURRENT_CLIENTS];
User users[MAX_USERS];
int numusers = 0;

void log_event(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buffer[25];
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", t);
    printf("[%s] %s\n", time_buffer, msg);
    fflush(stdout);
}

void sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTSTP) {
        if (listen_fd > 0) close(listen_fd);
        for (int j = 0; j < MAX_CONCURRENT_CLIENTS; j++) {
            if (clients[j].fd > 0) close(clients[j].fd);
        }
        log_event("Server shutting down.");
        exit(0);
    }
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Helper to send responses
int send_resp(int client_idx, const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 3, format, args);
    va_end(args);
    
    strcat(buffer, "\r\n");
    int fd = clients[client_idx].fd;
    int total_sent = 0, len = strlen(buffer);
    
    while (total_sent < len) {
        int x = send(fd, buffer + total_sent, len - total_sent, 0);
        if (x < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        total_sent += x;
    }
    return 1;
}

unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

void disconnect_client(int i, fd_set *fds) {
    if (clients[i].fd > 0) {
        char msg[100];
        snprintf(msg, sizeof(msg), "Client disconnected");
        log_event(msg);
        close(clients[i].fd);
        FD_CLR(clients[i].fd, fds);
        memset(&clients[i], 0, sizeof(Client));
    }
}

// Ensure mailboxes directory and user subdirectories exist, and determine next mail ID for each user
void ensure_mailboxes() {
    mkdir("mailboxes", 0700);
    for (int i = 0; i < numusers; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "mailboxes/%.*s", MAX_USERNAME_LEN, users[i].username);
        mkdir(path, 0700);
        
        DIR *d = opendir(path);
        int max_id = 0;
        if(d){
            struct dirent *dir;
            while ((dir = readdir(d)) != NULL) {
                int id = atoi(dir->d_name);
                if (id > max_id) max_id = id;
            }
            closedir(d);
        }
        users[i].nextid = max_id + 1;
    }
}

void read_userfile(const char *uf) {
    FILE *fptr = fopen(uf, "r");
    if (!fptr) {
        perror("Failed to open userfile");
        exit(1);
    }
    numusers = 0;
    while (numusers < MAX_USERS) {
        char uname[MAX_USERNAME_LEN], pass[MAX_PASS_LEN];
        if (fscanf(fptr, "%34s %34s", uname, pass) != 2) break;
        strcpy(users[numusers].username, uname);
        strcpy(users[numusers].pass, pass);
        numusers++;
    }
    fclose(fptr);
    char msg[100];
    snprintf(msg, sizeof(msg), "Loaded %d users from %s", numusers, uf);
    log_event(msg);
}

int find_user(const char *uname) {
    for (int i = 0; i < numusers; i++) {
        if (strcmp(users[i].username, uname) == 0) return i;
    }
    return -1;
}

void deliver_mail(int c_idx) {
    Client *c = &clients[c_idx];
    char date_str[30];
    char to_str[256] = "";
    time_t now = time(NULL);
    strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    for (int i = 0; i < c->num_to; i++) {
        int u_idx = c->to_users[i];
        char filepath[2500];
        snprintf(filepath, sizeof(filepath), "mailboxes/%s/%d.txt", users[u_idx].username, users[u_idx].nextid++);
        
        FILE *f = fopen(filepath, "w");
        if (!f) continue;
        
        fprintf(f, "From: %s\nTo: ", c->from_name);
        for (int j = 0; j < c->num_to; j++) {
            fprintf(f, "%s%s", users[c->to_users[j]].username, j == c->num_to - 1 ? "" : ",");
            sprintf(to_str + strlen(to_str), "[%s] ", users[c->to_users[j]].username);
        }
        fprintf(f, "\nSubject: %s\nDate: %s\n\n", c->subject, date_str);
        fwrite(c->body, 1, c->body_len, f);
        fclose(f);
    }
    
    char msg[1024];
    snprintf(msg, sizeof(msg), "Mail delivered from %s to %s (%d recipients)", c->from_name, to_str, c->num_to);
    log_event(msg);
}

void handle_smtp2_command(int i, char *cmd, fd_set *fds) {
    Client *c = &clients[i];

    if (strncmp(cmd, "QUIT", 4) == 0) {
        send_resp(i, "BYE");
        disconnect_client(i, fds);
        return;
    }

    if (c->state == FROM && strncmp(cmd, "FROM ", 5) == 0) {
        strncpy(c->from_name, cmd + 5, MAX_SUBJECT_LEN - 1);
        c->num_to = 0;
        c->state = TO;
        send_resp(i, "OK Sender accepted");
    } 
    else if ((c->state == TO || c->state == TO_OR_SUB) && strncmp(cmd, "TO ", 3) == 0) {
        int u_idx = find_user(cmd + 3);
        if (u_idx != -1) {
            c->to_users[c->num_to++] = u_idx;
            c->state = TO_OR_SUB;
            send_resp(i, "OK Recipient accepted");
        }
        else {
            send_resp(i, "ERR No such user");
        }
    } 
    else if (c->state == TO_OR_SUB && strncmp(cmd, "SUB", 3) == 0) {
        if (strlen(cmd) > 4) strncpy(c->subject, cmd + 4, MAX_SUBJECT_LEN - 1);
        else strcpy(c->subject, "(no subject)");
        c->state = BODY;
        send_resp(i, "OK Subject accepted");
    } 
    else if (c->state == BODY && strncmp(cmd, "BODY", 4) == 0) {
        if (c->num_to == 0) {
            send_resp(i, "ERR No valid recipients");
            c->state = FROM; // Reset
        } else {
            c->body_len = 0;
            c->state = BODY_CONT;
            send_resp(i, "OK Send body, end with CRLF.CRLF");
        }
    } 
    else {
        send_resp(i, "ERR Bad sequence");
    }
}

void handle_smp_command(int i, char *cmd, fd_set *fds) {
    Client *c = &clients[i];

    if (strncmp(cmd, "QUIT", 4) == 0) {
        send_resp(i, "BYE");
        disconnect_client(i, fds);
        return;
    }

    // Authentication Phase
    if (c->state == WAIT_AUTH) {
        char uname[MAX_USERNAME_LEN];
        unsigned long recv_hash;
        if (sscanf(cmd, "AUTH %s %lu", uname, &recv_hash) == 2) {
            int u_idx = find_user(uname);
            if (u_idx != -1) {
                char concat[100];
                snprintf(concat, sizeof(concat), "%s%s", users[u_idx].pass, c->nonce);
                if (djb2(concat) == recv_hash) {
                    c->user_idx = u_idx;
                    c->state = SMP_IDLE;
                    send_resp(i, "OK Welcome %s", uname);
                    char msg[100];
                    snprintf(msg, sizeof(msg), "Authentication successful for user %s", uname);
                    log_event(msg);
                    return;
                }
            }
        }
        c->auth_attempts++;
        if (c->auth_attempts >= 3) {
            send_resp(i, "ERR Too many failures");
            disconnect_client(i, fds);
        } else {
            send_resp(i, "ERR Authentication failed");
        }
    } 
    // Post-authentication commands
    else if (c->state == SMP_IDLE) {
        char path[1024];
        snprintf(path, sizeof(path), "mailboxes/%s", users[c->user_idx].username);

        if (strncmp(cmd, "COUNT", 5) == 0) {
            int count = 0;
            DIR *d = opendir(path);
            if (d) {
                struct dirent *dir;
                while ((dir = readdir(d)) != NULL) {
                    if (strstr(dir->d_name, ".txt")) count++;
                }
                closedir(d);
            }
            send_resp(i, "OK %d", count);
        } 
        else if (strncmp(cmd, "LIST", 4) == 0) {
            int count = 0;
            DIR *d = opendir(path);
            if (d) {
                struct dirent *dir;
                while ((dir = readdir(d)) != NULL) {
                    if (strstr(dir->d_name, ".txt")) count++;
                }
                rewinddir(d);
                send_resp(i, "OK %d messages", count);
                
                while ((dir = readdir(d)) != NULL) {
                    if (strstr(dir->d_name, ".txt")) {
                        char filepath[2500];
                        snprintf(filepath, sizeof(filepath), "%s/%s", path, dir->d_name);
                        FILE *f = fopen(filepath, "r");
                        if (f) {
                            char from[256]="", sub[256]="", date[256]="";
                            char line[512];
                            while(fgets(line, sizeof(line), f) && line[0] != '\n' && line[0] != '\r') {
                                if (strncmp(line, "From: ", 6)==0) sscanf(line+6, "%[^\n\r]", from);
                                if (strncmp(line, "Subject: ", 9)==0) sscanf(line+9, "%[^\n\r]", sub);
                                if (strncmp(line, "Date: ", 6)==0) sscanf(line+6, "%[^\n\r]", date);
                            }
                            fclose(f);
                            int id = atoi(dir->d_name);
                            send_resp(i, "%d\t%s\t%s\t%s", id, from, sub, date);
                        }
                    }
                }
                closedir(d);
            }
            send_resp(i, ".");
        } 
        else if (strncmp(cmd, "READ ", 5) == 0) {
            int id = atoi(cmd + 5);
            char filepath[2500];
            snprintf(filepath, sizeof(filepath), "%s/%d.txt", path, id);
            FILE *f = fopen(filepath, "r");
            if (f) {
                send_resp(i, "OK");
                char line[1024];
                while(fgets(line, sizeof(line), f)) {
                    line[strcspn(line, "\n")] = 0;
                    if (line[0] == '.') send_resp(i, ".%s", line); // Byte stuffing
                    else send_resp(i, "%s", line);
                }
                send_resp(i, ".");
                fclose(f);
                char msg[100];
                snprintf(msg, sizeof(msg), "User %s READ message %d", users[c->user_idx].username, id);
                log_event(msg);
            } else {
                send_resp(i, "ERR No such message");
            }
        } 
        else if (strncmp(cmd, "DELETE ", 7) == 0) {
            int id = atoi(cmd + 7);
            char filepath[2500];
            snprintf(filepath, sizeof(filepath), "%s/%d.txt", path, id);
            if (remove(filepath) == 0) {
                send_resp(i, "OK Deleted");
                char msg[100];
                snprintf(msg, sizeof(msg), "User %s DELETE message %d", users[c->user_idx].username, id);
                log_event(msg);
            } else {
                send_resp(i, "ERR No such message");
            }
        } 
        else {
            send_resp(i, "ERR Unknown command");
        }
    }
}

void process_line(int i, fd_set *fds) {
    Client *c = &clients[i];
    char *cmd = c->in_buf;

    // Initial MODE selection
    if (c->state == INIT) {
        if (strncmp(cmd, "MODE SEND", 9) == 0) {
            c->mode = 0;
            c->state = FROM;
            send_resp(i, "OK");
            log_event("Client selected MODE SEND");
        } else if (strncmp(cmd, "MODE RECV", 9) == 0) {
            c->mode = 1;
            c->state = WAIT_AUTH;
            c->auth_attempts = 0;
            snprintf(c->nonce, sizeof(c->nonce), "%08x", rand());
            send_resp(i, "OK");
            send_resp(i, "AUTH REQUIRED %s", c->nonce);
            log_event("Client selected MODE RECV");
        } else {
            send_resp(i, "ERR Unknown mode");
        }
        return;
    }

    // Middle of SMTP2 body input
    if (c->state == BODY_CONT) {
        if (strcmp(cmd, ".") == 0) {
            deliver_mail(i);
            send_resp(i, "OK Delivered to %d mailboxes", c->num_to);
            c->state = FROM;
        } else {
            char *write_ptr = cmd;
            if (cmd[0] == '.') write_ptr++; // Byte un-stuffing

            int line_len = strlen(write_ptr);
            if (c->body_len + line_len + 1 < MAX_BODY_LEN) {
                strcpy(c->body + c->body_len, write_ptr);
                c->body_len += line_len;
                c->body[c->body_len++] = '\n';
                c->body[c->body_len] = '\0';
            } else {
                send_resp(i, "ERR Body too large");
                c->state = FROM;
            }
        }
        return;
    }

    if (c->mode == 0) handle_smtp2_command(i, cmd, fds);
    else if (c->mode == 1) handle_smp_command(i, cmd, fds);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTSTP, sig_handler);
    srand(time(NULL));

    if (argc < 3) {
        printf("Usage: %s <PORT> <USERFILE>\n", argv[0]);
        return 1;
    }

    read_userfile(argv[2]);
    ensure_mailboxes();

    struct sockaddr_in address, client_addr;
    socklen_t len = sizeof(client_addr);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(listen_fd);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(atoi(argv[1]));

    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        return 1;
    }

    memset(clients, 0, sizeof(clients));
    fd_set fds, readfds;
    FD_ZERO(&fds);
    FD_SET(listen_fd, &fds);
    int max_fd = listen_fd;

    listen(listen_fd, 5);
    char msg[100];
    snprintf(msg, sizeof(msg), "Server started on port %s", argv[1]);
    log_event(msg);

    while (1) {
        readfds = fds;
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        select(max_fd + 1, &readfds, NULL, NULL, &tv);
        time_t now = time(NULL);

        // New Connection
        if (FD_ISSET(listen_fd, &readfds)) {
            while (1) {
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
                if (client_fd < 0) break;

                set_nonblocking(client_fd);
                int ind = -1;
                for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
                    if (clients[i].fd == 0) { ind = i; break; }
                }

                if (ind != -1) {
                    clients[ind].fd = client_fd;
                    clients[ind].state = INIT;
                    clients[ind].init_time = now;
                    clients[ind].in_len = 0;
                    FD_SET(client_fd, &fds);
                    if (client_fd > max_fd) max_fd = client_fd;

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
                    snprintf(msg, sizeof(msg), "New connection from %s : %d", ip_str, ntohs(client_addr.sin_port));
                    log_event(msg);

                    send_resp(ind, "WELCOME SimpleMail v1.0");
                } else {
                    close(client_fd);
                }
            }
        }

        // Existing Clients
        for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
            if (clients[i].fd == 0) continue;

            // Timeout Check
            if (clients[i].state == INIT && (now - clients[i].init_time > TIMEOUT_SEC)) {
                log_event("Client timed out waiting for MODE.");
                disconnect_client(i, &fds);
                continue;
            }

            if (FD_ISSET(clients[i].fd, &readfds)) {
                char buf[512];
                int n = recv(clients[i].fd, buf, sizeof(buf), 0);
                
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    disconnect_client(i, &fds);
                    continue;
                }

                for (int j = 0; j < n; j++) {
                    if (buf[j] == '\n') {
                        clients[i].in_buf[clients[i].in_len] = '\0';
                        // Strip trailing \r if present
                        if (clients[i].in_len > 0 && clients[i].in_buf[clients[i].in_len - 1] == '\r') {
                            clients[i].in_buf[clients[i].in_len - 1] = '\0';
                        }
                        process_line(i, &fds);
                        clients[i].in_len = 0; 
                    } else if (clients[i].in_len < (int)sizeof(clients[i].in_buf) - 1) {
                        clients[i].in_buf[clients[i].in_len++] = buf[j];
                    }
                }
            }
        }
    }
    return 0;
}