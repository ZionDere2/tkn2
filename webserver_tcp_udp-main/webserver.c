#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "data.h"
#include "http.h"
#include "util.h"

#define MAX_RESOURCES 100

struct node_info {
    uint16_t id;
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
};

static struct node_info self_info = {0};
static struct node_info pred_info = {0};
static struct node_info succ_info = {0};

struct tuple resources[MAX_RESOURCES] = {
    {"/static/foo", "Foo", sizeof "Foo" - 1},
    {"/static/bar", "Bar", sizeof "Bar" - 1},
    {"/static/baz", "Baz", sizeof "Baz" - 1}};

static bool is_responsible(uint16_t hash) {
    if (pred_info.id < self_info.id) {
        return hash > pred_info.id && hash <= self_info.id;
    }
    return hash > pred_info.id || hash <= self_info.id;
}

/**
 * Sends an HTTP reply to the client based on the received request.
 *
 * @param conn      The file descriptor of the client connection socket.
 * @param request   A pointer to the struct containing the parsed request
 * information.
 */
void send_reply(int conn, struct request *request) {

    // Create a buffer to hold the HTTP reply
    char buffer[HTTP_MAX_SIZE];
    char *reply = buffer;
    size_t offset = 0;

    uint16_t hash =
        pseudo_hash((const unsigned char *)request->uri, strlen(request->uri));

    fprintf(stderr, "Handling %s request for %s (%lu byte payload)\n",
            request->method, request->uri, request->payload_length);

    if (!is_responsible(hash)) {
        offset = sprintf(buffer,
                         "HTTP/1.1 303 See Other\r\nLocation: http://%s:%u%s\r\n"
                         "Content-Length: 0\r\n\r\n",
                         succ_info.ip, succ_info.port, request->uri);
        reply = buffer;
    } else if (strcmp(request->method, "GET") == 0) {
        // Find the resource with the given URI in the 'resources' array.
        size_t resource_length;
        const char *resource =
            get(request->uri, resources, MAX_RESOURCES, &resource_length);

        if (resource) {
            size_t payload_offset =
                sprintf(reply, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n",
                        resource_length);
            memcpy(reply + payload_offset, resource, resource_length);
            offset = payload_offset + resource_length;
        } else {
            reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            offset = strlen(reply);
        }
    } else if (strcmp(request->method, "PUT") == 0) {
        // Try to set the requested resource with the given payload in the
        // 'resources' array.
        if (set(request->uri, request->payload, request->payload_length,
                resources, MAX_RESOURCES)) {
            reply = "HTTP/1.1 204 No Content\r\n\r\n";
        } else {
            reply = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
        }
        offset = strlen(reply);
    } else if (strcmp(request->method, "DELETE") == 0) {
        // Try to delete the requested resource from the 'resources' array
        if (delete (request->uri, resources, MAX_RESOURCES)) {
            reply = "HTTP/1.1 204 No Content\r\n\r\n";
        } else {
            reply = "HTTP/1.1 404 Not Found\r\n\r\n";
        }
        offset = strlen(reply);
    } else {
        reply = "HTTP/1.1 501 Method Not Supported\r\n\r\n";
        offset = strlen(reply);
    }

    // Send the reply back to the client
    if (send(conn, reply, offset, 0) == -1) {
        perror("send");
        close(conn);
    }
}

/**
 * Processes an incoming packet from the client.
 *
 * @param conn The socket descriptor representing the connection to the client.
 * @param buffer A pointer to the incoming packet's buffer.
 * @param n The size of the incoming packet.
 *
 * @return Returns the number of bytes processed from the packet.
 *         If the packet is successfully processed and a reply is sent, the
 * return value indicates the number of bytes processed. If the packet is
 * malformed or an error occurs during processing, the return value is -1.
 *
 */
ssize_t process_packet(int conn, char *buffer, size_t n) {
    struct request request = {
        .method = NULL, .uri = NULL, .payload = NULL, .payload_length = -1};
    ssize_t bytes_processed = parse_request(buffer, n, &request);

    if (bytes_processed > 0) {
        send_reply(conn, &request);

        // Check the "Connection" header in the request to determine if the
        // connection should be kept alive or closed.
        const string connection_header = get_header(&request, "Connection");
        if (connection_header && strcmp(connection_header, "close")) {
            return -1;
        }
    } else if (bytes_processed == -1) {
        // If the request is malformed or an error occurs during processing,
        // send a 400 Bad Request response to the client.
        const string bad_request = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn, bad_request, strlen(bad_request), 0);
        printf("Received malformed request, terminating connection.\n");
        close(conn);
        return -1;
    }

    return bytes_processed;
}

/**
 * Sets up the connection state for a new socket connection.
 *
 * @param state A pointer to the connection_state structure to be initialized.
 * @param sock The socket descriptor representing the new connection.
 *
 */
static void connection_setup(struct connection_state *state, int sock) {
    // Set the socket descriptor for the new connection in the connection_state
    // structure.
    state->sock = sock;

    // Set the 'end' pointer of the state to the beginning of the buffer.
    state->end = state->buffer;

    // Clear the buffer by filling it with zeros to avoid any stale data.
    memset(state->buffer, 0, HTTP_MAX_SIZE);
}

/**
 * Discards the front of a buffer
 *
 * @param buffer A pointer to the buffer to be modified.
 * @param discard The number of bytes to drop from the front of the buffer.
 * @param keep The number of bytes that should be kept after the discarded
 * bytes.
 *
 * @return Returns a pointer to the first unused byte in the buffer after the
 * discard.
 * @example buffer_discard(ABCDEF0000, 4, 2):
 *          ABCDEF0000 ->  EFCDEF0000 -> EF00000000, returns pointer to first 0.
 */
char *buffer_discard(char *buffer, size_t discard, size_t keep) {
    memmove(buffer, buffer + discard, keep);
    memset(buffer + keep, 0, discard); // invalidate buffer
    return buffer + keep;
}

/**
 * Handles incoming connections and processes data received over the socket.
 *
 * @param state A pointer to the connection_state structure containing the
 * connection state.
 * @return Returns true if the connection and data processing were successful,
 * false otherwise. If an error occurs while receiving data from the socket, the
 * function exits the program.
 */
bool handle_connection(struct connection_state *state) {
    // Calculate the pointer to the end of the buffer to avoid buffer overflow
    const char *buffer_end = state->buffer + HTTP_MAX_SIZE;

    // Check if an error occurred while receiving data from the socket
    ssize_t bytes_read =
        recv(state->sock, state->end, buffer_end - state->end, 0);
    if (bytes_read == -1) {
        perror("recv");
        close(state->sock);
        exit(EXIT_FAILURE);
    } else if (bytes_read == 0) {
        return false;
    }

    char *window_start = state->buffer;
    char *window_end = state->end + bytes_read;

    ssize_t bytes_processed = 0;
    while ((bytes_processed = process_packet(state->sock, window_start,
                                             window_end - window_start)) > 0) {
        window_start += bytes_processed;
    }
    if (bytes_processed == -1) {
        return false;
    }

    state->end = buffer_discard(state->buffer, window_start - state->buffer,
                                window_end - window_start);
    return true;
}

/**
 * Derives a sockaddr_in structure from the provided host and port information.
 *
 * @param host The host (IP address or hostname) to be resolved into a network
 * address.
 * @param port The port number to be converted into network byte order.
 *
 * @return A sockaddr_in structure representing the network address derived from
 * the host and port.
 */
static struct sockaddr_in derive_sockaddr(const char *host, const char *port) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
    };
    struct addrinfo *result_info;

    // Resolve the host (IP address or hostname) into a list of possible
    // addresses.
    int returncode = getaddrinfo(host, port, &hints, &result_info);
    if (returncode) {
        fprintf(stderr, "Error parsing host/port");
        exit(EXIT_FAILURE);
    }

    // Copy the sockaddr_in structure from the first address in the list
    struct sockaddr_in result = *((struct sockaddr_in *)result_info->ai_addr);

    // Free the allocated memory for the result_info
    freeaddrinfo(result_info);
    return result;
}

static void initialize_nodes(const char *self_ip, const char *self_port,
                             uint16_t self_id) {
    strncpy(self_info.ip, self_ip, INET_ADDRSTRLEN);
    self_info.ip[INET_ADDRSTRLEN - 1] = '\0';
    self_info.port = safe_strtoul(self_port, NULL, 10, "invalid self port");
    self_info.id = self_id;

    const char *pred_id_env = getenv("PRED_ID");
    const char *pred_ip_env = getenv("PRED_IP");
    const char *pred_port_env = getenv("PRED_PORT");

    const char *succ_id_env = getenv("SUCC_ID");
    const char *succ_ip_env = getenv("SUCC_IP");
    const char *succ_port_env = getenv("SUCC_PORT");

    if (!pred_id_env || !pred_ip_env || !pred_port_env || !succ_id_env ||
        !succ_ip_env || !succ_port_env) {
        fprintf(stderr, "Missing neighborhood configuration\n");
        exit(EXIT_FAILURE);
    }

    pred_info.id = safe_strtoul(pred_id_env, NULL, 10, "invalid predecessor id");
    strncpy(pred_info.ip, pred_ip_env, INET_ADDRSTRLEN);
    pred_info.ip[INET_ADDRSTRLEN - 1] = '\0';
    pred_info.port =
        safe_strtoul(pred_port_env, NULL, 10, "invalid predecessor port");

    succ_info.id = safe_strtoul(succ_id_env, NULL, 10, "invalid successor id");
    strncpy(succ_info.ip, succ_ip_env, INET_ADDRSTRLEN);
    succ_info.ip[INET_ADDRSTRLEN - 1] = '\0';
    succ_info.port =
        safe_strtoul(succ_port_env, NULL, 10, "invalid successor port");
}

/**
 * Sets up a TCP server socket and binds it to the provided sockaddr_in address.
 *
 * @param addr The sockaddr_in structure representing the IP address and port of
 * the server.
 *
 * @return The file descriptor of the created TCP server socket.
 */
static int setup_server_socket(struct sockaddr_in addr) {
    const int enable = 1;
    const int backlog = 1;

    // Create a socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Avoid dead lock on connections that are dropped after poll returns but
    // before accept is called
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    // Set the SO_REUSEADDR socket option to allow reuse of local addresses
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) ==
        -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Bind socket to the provided address
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Start listening on the socket with maximum backlog of 1 pending
    // connection
    if (listen(sock, backlog)) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return sock;
}
static int setup_udp_server_socket(struct sockaddr_in addr) {
    const int enable = 1;
   // const int backlog = 1;

    // Create a socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Avoid dead lock on connections that are dropped after poll returns but
    // before accept is called
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    // Set the SO_REUSEADDR socket option to allow reuse of local addresses
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) ==
        -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
        }

    // Bind socket to the provided address
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Start listening on the socket with maximum backlog of 1 pending
    // connection
    /*if (listen(sock, backlog)) {
        perror("listen");
        exit(EXIT_FAILURE);
    }*/

    return sock;
}   //Aufgabe 1.1


/**
 *  The program expects at least 3 and at most 4 parameters; otherwise, it
 *  returns EXIT_FAILURE.
 *
 *  Call as:
 *
 *  ./build/webserver self.ip self.port
 */
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        return EXIT_FAILURE;
    }

    uint16_t node_id = 0;
    if (argc == 4) {
        node_id = safe_strtoul(argv[3], NULL, 10, "invalid node id");
    }

    struct sockaddr_in addr = derive_sockaddr(argv[1], argv[2]);

    initialize_nodes(argv[1], argv[2], node_id);

    // Set up a server socket.
    int server_socket = setup_server_socket(addr);
    int server_socket_udp = setup_udp_server_socket(addr);  //Aufgabe 1.1

    // Erstelle ein Array von pollfd-Strukturen, um Sockets auf Ereignisse zu überwachen.
    // struct pollfd: Struktur aus <poll.h> für die poll-Funktion; enthält fd (Dateideskriptor),
    // events (gewünschte Ereignisse, z.B. POLLIN für eingehende Daten) und revents (tatsächliche Ereignisse).
    struct pollfd sockets[2] = {
        {.fd = server_socket, .events = POLLIN},        // Überwache TCP-Server-Socket auf eingehende Verbindungen
        {.fd = server_socket_udp, .events = POLLIN}     // Überwache UDP-Server-Socket auf eingehende Datagramme (Aufgabe 1.1)
    };


    struct connection_state state = {0};
    while (true) {

        // Use poll() to wait for events on the monitored sockets.
        /*Die poll-Funktion wartet hier auf Ereignisse (z. B. eingehende Daten oder Verbindungen) auf den 
        überwachten Sockets (sockets-Array), ohne den Prozess zu blockieren. 
        Sie gibt die Anzahl der bereitstehenden Sockets zurück (ready),
         damit der Server effizient auf TCP-Verbindungen, UDP-Datagramme oder Client-Daten 
         reagieren kann, anstatt ständig zu prüfen. Der Timeout -1 bedeutet unbegrenztes Warten */
        int ready = poll(sockets, sizeof(sockets) / sizeof(sockets[0]), -1);
        if (ready == -1) {
            perror("poll");
            exit(EXIT_FAILURE);
        }

        // Process events on the monitored sockets.
        //  in der for-Schleife wird jedes Socket individuell über sockets[i].revents geprüft, ob ein POLLIN-Ereignis vorliegt. 
        
        for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i += 1) {
            if (sockets[i].revents != POLLIN) {
                // If there are no POLLIN events on the socket, continue to the
                // next iteration.
                continue;
            }
            int s = sockets[i].fd;

            if (s == server_socket) {

                // If the event is on the server_socket, accept a new connection
                // from a client.
                int connection = accept(server_socket, NULL, NULL);
                if (connection == -1 && errno != EAGAIN &&
                    errno != EWOULDBLOCK) {
                    close(server_socket);
                    perror("accept");
                    exit(EXIT_FAILURE);
                } else {
                    connection_setup(&state, connection);

                    // limit to one connection at a time
                    sockets[0].events = 0;
                    sockets[1].fd = connection;
                    sockets[1].events = POLLIN;
                }
            }
            else if (s == server_socket_udp) {  //Aufgabe 1.1
                char RecvBuf[HTTP_MAX_SIZE];
                struct sockaddr_in SenderAddr;
                socklen_t sender_addrlen = sizeof (SenderAddr);

                printf("Receiving datagrams...\n");

                ssize_t dg_connection = recvfrom(server_socket_udp,RecvBuf, HTTP_MAX_SIZE, 0, (struct sockaddr *) & SenderAddr, &sender_addrlen);
                if (dg_connection == -1 && errno != EAGAIN &&
                    errno != EWOULDBLOCK)
                {
                    close(server_socket_udp);
                    perror("accept");
                    exit(EXIT_FAILURE);
                }


            } else {
                assert(s == state.sock);

                // Call the 'handle_connection' function to process the incoming
                // data on the socket.
                bool cont = handle_connection(&state);
                if (!cont) { // get ready for a new connection
                    sockets[0].events = POLLIN;
                    sockets[1].fd = -1;
                    sockets[1].events = 0;
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
