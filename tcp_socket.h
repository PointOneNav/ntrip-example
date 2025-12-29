/*
 * tcp_socket.h - Cross-platform TCP socket handling
 * 
 * Supports Linux, macOS, and Windows TCP communication.
 * Includes both client (TcpSocket) and server (TcpServer) classes.
 */

#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#include <string>
#include <iostream>
#include <vector>
#include <cstring>

// Platform detection and includes
#ifdef _WIN32
    #define PLATFORM_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define SOCKET_CLOSE(s) closesocket(s)
    #define SOCKET_ERROR_CODE WSAGetLastError()
#else
    #define PLATFORM_UNIX
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <errno.h>
    
    typedef int socket_t;
    #define INVALID_SOCKET_VALUE (-1)
    #define SOCKET_CLOSE(s) ::close(s)
    #define SOCKET_ERROR_CODE errno
#endif

// Cross-platform sleep helper
inline void socket_sleep_ms(int ms) {
#ifdef PLATFORM_WINDOWS
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

// Set socket to non-blocking mode
inline void set_socket_nonblocking(socket_t sock, bool nonblocking) {
#ifdef PLATFORM_WINDOWS
    u_long mode = nonblocking ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (nonblocking) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    } else {
        fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
}

class TcpSocket {
public:
    socket_t sock = INVALID_SOCKET_VALUE;
    
    TcpSocket() = default;
    
    // Construct from existing socket (used by TcpServer::accept)
    explicit TcpSocket(socket_t s) : sock(s) {}
    
    ~TcpSocket() {
        close();
    }
    
    // Disable copy
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    
    // Allow move
    TcpSocket(TcpSocket&& other) noexcept {
        sock = other.sock;
        other.sock = INVALID_SOCKET_VALUE;
    }
    
    TcpSocket& operator=(TcpSocket&& other) noexcept {
        if (this != &other) {
            close();
            sock = other.sock;
            other.sock = INVALID_SOCKET_VALUE;
        }
        return *this;
    }
    
    // Initialize socket subsystem (required for Windows)
    static bool init() {
#ifdef PLATFORM_WINDOWS
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }
    
    // Cleanup socket subsystem (required for Windows)
    static void cleanup() {
#ifdef PLATFORM_WINDOWS
        WSACleanup();
#endif
    }
    
    bool connect(const std::string& host, int port) {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET_VALUE) {
            std::cerr << "Error creating socket: " << SOCKET_ERROR_CODE << std::endl;
            return false;
        }
        
        struct addrinfo hints, *result = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
            std::cerr << "Error resolving host: " << host << std::endl;
            close();
            return false;
        }
        
        int ret = ::connect(sock, result->ai_addr, (int)result->ai_addrlen);
        freeaddrinfo(result);
        
        if (ret != 0) {
            std::cerr << "Error connecting to " << host << ":" << port 
                      << ": " << SOCKET_ERROR_CODE << std::endl;
            close();
            return false;
        }
        
        // Set non-blocking
        set_nonblocking(true);
        
        return true;
    }
    
    void close() {
        if (sock != INVALID_SOCKET_VALUE) {
            SOCKET_CLOSE(sock);
            sock = INVALID_SOCKET_VALUE;
        }
    }
    
    int read(char* buf, size_t len) {
        return recv(sock, buf, (int)len, 0);
    }
    
    int write(const char* buf, size_t len) {
        return send(sock, buf, (int)len, 0);
    }
    
    // Send all data, retrying on EAGAIN/EWOULDBLOCK
    bool send_all(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = send(sock, data.c_str() + sent, (int)(data.size() - sent), 0);
            if (n < 0) {
#ifdef PLATFORM_WINDOWS
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    socket_sleep_ms(1);
                    continue;
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    socket_sleep_ms(1);
                    continue;
                }
#endif
                return false;
            }
            sent += n;
        }
        return true;
    }
    
    bool is_open() const {
        return sock != INVALID_SOCKET_VALUE;
    }
    
    // Wait for data with timeout
    // Returns: 1=data ready, 0=timeout, -1=error
    int wait_readable(int timeout_sec) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        
        return select((int)sock + 1, &fds, nullptr, nullptr, &tv);
    }
    
    // Wait for socket to be writable with timeout
    // Returns: 1=ready to write, 0=timeout, -1=error
    int wait_writable(int timeout_sec) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        
        return select((int)sock + 1, nullptr, &fds, nullptr, &tv);
    }
    
    void set_nonblocking(bool nonblocking) {
        set_socket_nonblocking(sock, nonblocking);
    }
    
    // Get the last error code
    static int get_error_code() {
        return SOCKET_ERROR_CODE;
    }
    
    // Check if the last error was EAGAIN/EWOULDBLOCK
    static bool would_block() {
#ifdef PLATFORM_WINDOWS
        return WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }
    
    // Check if the last error was EINTR
    static bool was_interrupted() {
#ifdef PLATFORM_WINDOWS
        return WSAGetLastError() == WSAEINTR;
#else
        return errno == EINTR;
#endif
    }
};


// TCP Server class - listens for connections and manages multiple clients
class TcpServer {
public:
    socket_t listen_sock = INVALID_SOCKET_VALUE;
    std::vector<socket_t> clients;
    int port = 0;
    
    TcpServer() = default;
    
    ~TcpServer() {
        close();
    }
    
    // Disable copy
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    
    // Start listening on a port
    bool listen(int listen_port, int backlog = 5) {
        port = listen_port;
        
        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == INVALID_SOCKET_VALUE) {
            std::cerr << "Error creating server socket: " << SOCKET_ERROR_CODE << std::endl;
            return false;
        }
        
        // Allow address reuse
        int opt = 1;
#ifdef PLATFORM_WINDOWS
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#else
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(listen_port);
        
        if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Error binding to port " << listen_port << ": " << SOCKET_ERROR_CODE << std::endl;
            close();
            return false;
        }
        
        if (::listen(listen_sock, backlog) < 0) {
            std::cerr << "Error listening: " << SOCKET_ERROR_CODE << std::endl;
            close();
            return false;
        }
        
        // Set non-blocking
        set_socket_nonblocking(listen_sock, true);
        
        return true;
    }
    
    // Accept new connections (non-blocking)
    // Returns number of new connections accepted
    int accept_pending() {
        int count = 0;
        
        while (true) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            socket_t client = ::accept(listen_sock, (struct sockaddr*)&client_addr, &client_len);
            
            if (client == INVALID_SOCKET_VALUE) {
                break;  // No more pending connections
            }
            
            // Set client to non-blocking
            set_socket_nonblocking(client, true);
            
            clients.push_back(client);
            count++;
            
            // Get client address for logging
            char addr_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
            std::cout << "TCP client connected: " << addr_str << ":" << ntohs(client_addr.sin_port) 
                      << " (total: " << clients.size() << ")" << std::endl;
        }
        
        return count;
    }
    
    // Broadcast data to all connected clients
    // Removes disconnected clients
    void broadcast(const char* data, size_t len) {
        if (clients.empty()) return;
        
        std::vector<socket_t> still_connected;
        
        for (socket_t client : clients) {
#ifdef PLATFORM_WINDOWS
            int sent = send(client, data, (int)len, 0);
#else
            // MSG_NOSIGNAL prevents SIGPIPE if client disconnected
            int sent = send(client, data, (int)len, MSG_NOSIGNAL);
#endif
            
            if (sent < 0) {
#ifdef PLATFORM_WINDOWS
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    // Would block, keep client
                    still_connected.push_back(client);
                    continue;
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Would block, keep client
                    still_connected.push_back(client);
                    continue;
                }
#endif
                // Error, client disconnected
                std::cout << "TCP client disconnected (total: " << still_connected.size() << ")" << std::endl;
                SOCKET_CLOSE(client);
            } else {
                still_connected.push_back(client);
            }
        }
        
        clients = std::move(still_connected);
    }
    
    // Close all connections
    void close() {
        for (socket_t client : clients) {
            SOCKET_CLOSE(client);
        }
        clients.clear();
        
        if (listen_sock != INVALID_SOCKET_VALUE) {
            SOCKET_CLOSE(listen_sock);
            listen_sock = INVALID_SOCKET_VALUE;
        }
    }
    
    bool is_listening() const {
        return listen_sock != INVALID_SOCKET_VALUE;
    }
    
    size_t client_count() const {
        return clients.size();
    }
};

#endif // TCP_SOCKET_H
