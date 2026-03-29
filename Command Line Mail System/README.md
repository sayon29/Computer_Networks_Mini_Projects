# SimpleMail - Command-Line Mail System (CS39006 Mini Project 2)

SimpleMail is a custom command-line email system built from scratch using C socket programming. It features a concurrent server (`smserver`) and an interactive client (`smclient`) that communicate over custom text-based TCP protocols: SMTP2 for sending mail and SMP for retrieving mail.

## 1. Concurrency Handling
To handle multiple clients simultaneously without blocking or freezing, the server uses a single-threaded event loop powered by the `select()` system call. 
* **Non-Blocking Sockets:** The listening socket and all connected client sockets are set to non-blocking mode using `fcntl()`. 
* **State Machines:** Because `select()` multiplexes I/O, the server cannot halt execution to wait for a specific client's input. Instead, each connected client is assigned a `Client` struct containing a state variable (e.g., `FROM`, `WAIT_AUTH`). This allows the server to remember exactly where each client is in the protocol sequence and juggle multiple connections concurrently.

## 2. Robustness Against Partial Reads (TCP Streams)
TCP guarantees data delivery, but it does not guarantee that data arrives in neat, line-by-line chunks. 
* **Line Buffering:** To handle fragmented packets and partial reads safely, the server maintains an internal character buffer (`in_buf`) for each client. 
* As data arrives via `read()`, it is appended to the buffer. The server only processes a command when it explicitly detects a newline character (`\n`), stripping out any carriage returns (`\r`). This ensures the server never attempts to parse a half-received command.

## 3. Mailbox and File ID Management
Mailboxes are stored persistently as flat text files inside `mailboxes/<username>/`.
* **Monotonically Increasing IDs:** The specification requires that message IDs never repeat, even if messages are deleted. On server startup, the `ensure_mailboxes()` function iterates through every user's directory using `opendir()` and `readdir()`.
* It parses the numeric filenames (e.g., `1.txt`, `2.txt`) to find the highest existing ID. The user's `nextid` variable is initialized to `max_id + 1`. Whenever a new mail is delivered, this ID is used for the filename and immediately incremented.

## 4. Protocol & Edge Case Implementation
Both SMTP2 and SMP were strictly implemented to handle edge cases gracefully:
* **Byte-Stuffing (Dot-Stuffing):** Handled transparently. The client prepends an extra `.` if a user's body line starts with a dot. The server strips this leading dot before saving to disk, preventing premature body termination. The server reapplies it when sending file contents during the `READ` command.
* **Out-of-Sequence Commands:** If a client sends a command at the wrong time (e.g., sending `BODY` before a valid `TO`), the server state machine catches it and returns `ERR Bad sequence`.
* **Empty Subjects:** If the `SUB` command is sent with no trailing text, the server intercepts it and automatically stores the subject as `(no subject)`.
* **Authentication:** Uses the DJB2 challenge-response hashing mechanism. Passwords are concatenated with a server-generated nonce and hashed before being sent over the network, ensuring plaintext passwords are never transmitted.

## 5. Assumptions & Design Choices
* **Max Clients:** The server restricts concurrent connections to a maximum of 50 (`MAX_CONCURRENT_CLIENTS = 50`) to prevent resource exhaustion. Additional connection attempts are immediately closed.
* **Buffer Limits:** The maximum length of a single protocol command line is capped at 1024 bytes (safely above the 512-byte specification). The maximum total email body size is strictly checked against a 65536-byte limit.
* **Timeouts:** If a client connects but fails to send a `MODE` declaration within 30 seconds, the server automatically disconnects them.
* **Client Reconnection:** As per the client interactive menu flow, `smclient` establishes a fresh TCP connection for every main menu action (Send or Check Mailbox), sending `QUIT` and fully closing the socket upon completion of the sub-menu tasks.

## 6. Included Files
* `smserver.c`: The SimpleMail server program.
* `smclient.c`: The SimpleMail client program.
* `Makefile`: Builds both executables (`make all`).
* `users.txt`: Sample text file containing registered users and passwords.
* `README.md`: This documentation file.