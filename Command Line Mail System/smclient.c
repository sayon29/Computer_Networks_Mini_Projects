#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>

#define MAX_LINE 1024

// DJB2 Hash function
unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// Helper to strip trailing newline characters
void strip_newline(char *str) {
    int len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[len - 1] = '\0';
        len--;
    }
}

// Read exactly one CRLF-terminated line from the socket
int read_line(int sock, char *buf, int max_len) {
    int count = 0;
    char c;
    while (count < max_len - 1) {
        int n = read(sock, &c, 1);
        if (n <= 0) return -1; // Connection closed or error
        buf[count++] = c;
        if (c == '\n') break;
    }
    buf[count] = '\0';
    strip_newline(buf);
    return count;
}

// Send formatted command with CRLF
void send_cmd(int sock, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    strcat(buf, "\r\n");
    write(sock, buf, strlen(buf));
}

// Connects to the server and returns the socket fd
int connect_to_server(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        printf("Invalid address/ Address not supported\n");
        exit(1);
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        exit(1);
    }
    return sock;
}

// Sending Mail
void handle_send_mail(int sock) {
    char buf[MAX_LINE];
    char from[MAX_LINE], to[MAX_LINE], sub[MAX_LINE];
    int valid_recipients = 0;

    send_cmd(sock, "MODE SEND");
    read_line(sock, buf, sizeof(buf)); // Read OK

    printf("From (your name): ");
    fgets(from, sizeof(from), stdin);
    strip_newline(from);
    send_cmd(sock, "FROM %s", from);
    read_line(sock, buf, sizeof(buf)); // Read OK Sender accepted

    while (1) {
        printf("To (recipient username, empty line to finish): ");
        fgets(to, sizeof(to), stdin);
        strip_newline(to);
        
        if (strlen(to) == 0) break; // Empty line finishes recipient input

        send_cmd(sock, "TO %s", to);
        read_line(sock, buf, sizeof(buf));
        
        if (strncmp(buf, "OK", 2) == 0) {
            printf("-> Recipient '%s' accepted.\n", to);
            valid_recipients++;
        } else {
            printf("-> Error: user '%s' does not exist on this server.\n", to);
        }
    }

    if (valid_recipients == 0) {
        printf("No valid recipients provided. Aborting mail.\n");
        send_cmd(sock, "QUIT");
        return;
    }

    printf("Subject: ");
    fgets(sub, sizeof(sub), stdin);
    strip_newline(sub);
    send_cmd(sock, "SUB %s", sub);
    read_line(sock, buf, sizeof(buf)); // Read OK Subject accepted

    printf("Body (type '.' on a line by itself to finish):\n");
    send_cmd(sock, "BODY");
    read_line(sock, buf, sizeof(buf)); // Read OK Send body

    while (1) {
        char line[MAX_LINE];
        fgets(line, sizeof(line), stdin);
        strip_newline(line);

        if (strcmp(line, ".") == 0) {
            send_cmd(sock, ".");
            break;
        }

        // Byte stuffing: prepend extra dot if line starts with dot
        if (line[0] == '.') {
            send_cmd(sock, ".%s", line);
        } else {
            send_cmd(sock, "%s", line);
        }
    }

    read_line(sock, buf, sizeof(buf)); 
    printf("Mail delivered to %d recipient(s).\n", valid_recipients); 

    send_cmd(sock, "QUIT"); 
    read_line(sock, buf, sizeof(buf)); // Read BYE
}

// Checking Mailbox (SMP)
void handle_mailbox(int sock) {
    char buf[MAX_LINE], nonce[20];
    char username[MAX_LINE], password[MAX_LINE];
    int authenticated = 0;

    send_cmd(sock, "MODE RECV");
    read_line(sock, buf, sizeof(buf)); // Read OK

    read_line(sock, buf, sizeof(buf)); // Read AUTH REQUIRED <nonce>
    sscanf(buf, "AUTH REQUIRED %s", nonce);

    // Authentication Loop
    for (int attempts = 0; attempts < 3; attempts++) {
        printf("Username: ");
        fgets(username, sizeof(username), stdin);
        strip_newline(username);

        printf("Password: ");
        fgets(password, sizeof(password), stdin);
        strip_newline(password);

        char auth_str[MAX_LINE];
        snprintf(auth_str, sizeof(auth_str), "%s%s", password, nonce); 
        unsigned long hash = djb2(auth_str); 

        send_cmd(sock, "AUTH %s %lu", username, hash); 
        read_line(sock, buf, sizeof(buf));

        if (strncmp(buf, "OK", 2) == 0) {
            printf("Welcome, %s!\n", username);
            authenticated = 1;
            break;
        } else {
            printf("Authentication failed. Try again.\n");
        }
    }

    if (!authenticated) {
        printf("Max authentication attempts reached.\n");
        return;
    }

    // Mailbox Menu Loop
    while (1) {
        int msg_count = 0;
        send_cmd(sock, "COUNT"); 
        read_line(sock, buf, sizeof(buf));
        sscanf(buf, "OK %d", &msg_count);

        printf("\nMailbox for %s (%d messages)\n", username, msg_count); 
        printf("1. List all messages\n");
        printf("2. Read a message\n");
        printf("3. Delete a message\n");
        printf("4. Logout\n> ");
        
        char choice[10];
        fgets(choice, sizeof(choice), stdin);
        int opt = atoi(choice);

        if (opt == 1) {
            send_cmd(sock, "LIST"); 
            read_line(sock, buf, sizeof(buf)); // Read OK X messages
            
            printf("\n%-5s %-20s %-25s %s\n", "ID", "From", "Subject", "Date");
            printf("-----------------------------------------------------------------------\n");
            
            while (read_line(sock, buf, sizeof(buf)) > 0) {
                if (strcmp(buf, ".") == 0) break; // Dot-termination
                char id[10], from[50], sub[100], date[30];
                sscanf(buf, "%[^\t]\t%[^\t]\t%[^\t]\t%[^\t]", id, from, sub, date); 
                printf("%-5s %-20s %-25s %s\n", id, from, sub, date);
            }
        } 
        else if (opt == 2) {
            char id[10];
            printf("Enter message ID: ");
            fgets(id, sizeof(id), stdin);
            strip_newline(id);

            send_cmd(sock, "READ %s", id); 
            read_line(sock, buf, sizeof(buf));

            if (strncmp(buf, "OK", 2) == 0) {
                printf("\n");
                while (read_line(sock, buf, sizeof(buf)) > 0) {
                    if (strcmp(buf, ".") == 0) break; // Dot-termination 
                    
                    // Byte un-stuffing
                    if (buf[0] == '.' && buf[1] == '.') {
                        printf("%s\n", buf + 1);
                    } else {
                        printf("%s\n", buf);
                    }
                }
            } else {
                printf("Error: No such message.\n"); 
            }
        } 
        else if (opt == 3) {
            char id[10];
            printf("Enter message ID: ");
            fgets(id, sizeof(id), stdin);
            strip_newline(id);

            send_cmd(sock, "DELETE %s", id); 
            read_line(sock, buf, sizeof(buf));

            if (strncmp(buf, "OK", 2) == 0) {
                printf("Message %s deleted.\n", id); 
            } else {
                printf("Error: No such message.\n"); 
            }
        } 
        else if (opt == 4) {
            send_cmd(sock, "QUIT"); 
            read_line(sock, buf, sizeof(buf));
            printf("Logged out.\n"); 
            break;
        } else {
            printf("Invalid choice.\n");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]); 
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    while (1) {
        // Connect fresh for each main menu selection as per spec 
        int sock = connect_to_server(ip, port);
        char buf[MAX_LINE];
        
        read_line(sock, buf, sizeof(buf)); // Read WELCOME SimpleMail v1.0
        printf("Connected to SimpleMail server.\n"); 
        
        printf("1. Send a mail\n"); 
        printf("2. Check my mailbox\n"); 
        printf("3. Quit\n> "); 
        
        char choice[10];
        if (!fgets(choice, sizeof(choice), stdin)) break;
        int opt = atoi(choice);

        if (opt == 1) {
            handle_send_mail(sock);
        } else if (opt == 2) {
            handle_mailbox(sock);
        } else if (opt == 3) {
            printf("Goodbye.\n"); 
            close(sock);
            break;
        } else {
            printf("Invalid choice. Try again.\n");
            close(sock);
        }
        
        // Ensure connection is fully closed before restarting menu loop
        close(sock);
        printf("\n");
    }

    return 0;
}