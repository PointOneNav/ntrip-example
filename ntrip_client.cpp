/*
 * Simple NTRIP Client for Linux/Mac/Windows
 * 
 * Features:
 * - Connects to NTRIP caster and receives RTCM correction data
 * - Reads GGA messages from GPS receiver on serial port
 * - Runs a TCP server to forward serial data to connected clients
 * - JSON configuration file
 * 
 * Compile:
 *   Linux/Mac: g++ -std=c++17 -o ntrip_client ntrip_client.cpp -pthread
 *   Windows:   cl /EHsc /std:c++17 ntrip_client.cpp ws2_32.lib
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <csignal>

#include "json_config.h"
#include "serial_port.h"
#include "tcp_socket.h"

// Base64 encoding for authentication
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& input) {
    std::string output;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) output.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (output.size() % 4) output.push_back('=');
    return output;
}

// Global state
std::atomic<bool> g_running{true};
std::mutex g_gga_mutex;
std::string g_last_gga;

void signal_handler(int) {
    g_running = false;
}

// Extract GGA sentence from NMEA data
std::string extract_gga(const std::string& nmea_buffer) {
    size_t pos = nmea_buffer.find("$GPGGA");
    if (pos == std::string::npos) {
        pos = nmea_buffer.find("$GNGGA");
    }
    
    if (pos != std::string::npos) {
        size_t end = nmea_buffer.find('\n', pos);
        if (end != std::string::npos) {
            std::string gga = nmea_buffer.substr(pos, end - pos);
            // Ensure it ends with \r\n
            if (!gga.empty() && gga.back() == '\r') {
                gga.pop_back();
            }
            return gga + "\r\n";
        }
    }
    return "";
}

// NTRIP client thread
void ntrip_thread(const JsonConfig& config, SerialPort& serial) {
    while (g_running) {
        TcpSocket ntrip;
        
        std::cout << "Connecting to NTRIP caster " << config.ntrip_host 
                  << ":" << config.ntrip_port << "..." << std::endl;
        
        if (!ntrip.connect(config.ntrip_host, config.ntrip_port)) {
            std::cerr << "Failed to connect to NTRIP caster, retrying in 5s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        
        // Build NTRIP request
        std::string request = "GET /" + config.ntrip_mountpoint + " HTTP/1.0\r\n";
        request += "User-Agent: NTRIP SimpleClient/1.0\r\n";
        request += "Accept: */*\r\n";
        
        if (!config.ntrip_user.empty()) {
            std::string auth = config.ntrip_user + ":" + config.ntrip_password;
            request += "Authorization: Basic " + base64_encode(auth) + "\r\n";
        }
        
        request += "\r\n";
        
        std::cout << "Sending NTRIP request..." << std::endl;
        
        if (!ntrip.send_all(request)) {
            std::cerr << "Failed to send NTRIP request" << std::endl;
            ntrip.close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        
        // Read response header
        char buf[4096];
        std::string response;
        bool header_done = false;
        
        while (g_running && !header_done) {
            int ret = ntrip.wait_readable(5);
            
            if (ret <= 0) {
                std::cerr << "Timeout waiting for NTRIP response" << std::endl;
                break;
            }
            
            int n = ntrip.read(buf, sizeof(buf) - 1);
            if (n <= 0) break;
            
            buf[n] = '\0';
            response += buf;
            
            if (response.find("\r\n\r\n") != std::string::npos) {
                header_done = true;
            }
        }
        
        if (!header_done || response.find("ICY 200 OK") == std::string::npos) {
            std::cerr << "NTRIP connection failed. Response:\n" << response << std::endl;
            ntrip.close();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        
        std::cout << "NTRIP connected successfully!" << std::endl;
        
        auto last_gga_time = std::chrono::steady_clock::now();
        auto last_data_time = std::chrono::steady_clock::now();
        const int receive_timeout_sec = 30;
        
        // Main NTRIP loop
        while (g_running) {
            int ret = ntrip.wait_readable(1);
            
            if (ret < 0) {
                if (TcpSocket::was_interrupted()) continue;
                break;
            }
            
            // Read RTCM data from NTRIP and write to serial
            if (ret > 0) {
                int n = ntrip.read(buf, sizeof(buf));
                if (n <= 0) {
                    std::cerr << "NTRIP connection lost" << std::endl;
                    break;
                }
                
                // Update last data time
                last_data_time = std::chrono::steady_clock::now();
                
                // Write RTCM corrections to GPS receiver
                if (serial.is_open()) {
                    serial.write(buf, n);
                }
            }
            
            auto now = std::chrono::steady_clock::now();
            
            // Check for receive timeout
            auto data_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_data_time).count();
            if (data_elapsed >= receive_timeout_sec) {
                std::cerr << "No data received for " << receive_timeout_sec << "s, reconnecting..." << std::endl;
                break;
            }
            
            // Send GGA periodically
            auto gga_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_gga_time).count();
            
            if (gga_elapsed >= config.gga_interval_sec) {
                std::string gga;
                {
                    std::lock_guard<std::mutex> lock(g_gga_mutex);
                    gga = g_last_gga;
                }
                
                if (!gga.empty()) {
                    std::cout << "Sending GGA: " << gga;
                    if (!ntrip.send_all(gga)) {
                        std::cerr << "Failed to send GGA" << std::endl;
                        break;
                    }
                }
                last_gga_time = now;
            }
        }
        
        ntrip.close();
        
        if (g_running) {
            std::cerr << "Reconnecting in 5s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

// Serial reader thread - also manages TCP server for forwarding
void serial_thread(SerialPort& serial, TcpServer* tcp_server) {
    std::string nmea_buffer;
    char buf[1024];
    
    while (g_running) {
        // Accept any pending TCP client connections
        if (tcp_server && tcp_server->is_listening()) {
            tcp_server->accept_pending();
        }
        
        int n = serial.read(buf, sizeof(buf));
        
        if (n < 0) {
#ifdef PLATFORM_WINDOWS
            socket_sleep_ms(10);
            continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                socket_sleep_ms(10);
                continue;
            }
            std::cerr << "Serial read error: " << SerialPort::get_error_string() << std::endl;
            break;
#endif
        }
        
        if (n == 0) {
            socket_sleep_ms(10);
            continue;
        }
        
        // Broadcast to all connected TCP clients
        if (tcp_server && tcp_server->is_listening()) {
            tcp_server->broadcast(buf, n);
        }
        
        // Parse for GGA
        nmea_buffer.append(buf, n);
        
        // Keep buffer from growing too large
        if (nmea_buffer.size() > 8192) {
            nmea_buffer = nmea_buffer.substr(nmea_buffer.size() - 4096);
        }
        
        // Extract GGA
        std::string gga = extract_gga(nmea_buffer);
        if (!gga.empty()) {
            std::lock_guard<std::mutex> lock(g_gga_mutex);
            g_last_gga = gga;
            
            // Clear processed part of buffer
            size_t pos = nmea_buffer.rfind('\n');
            if (pos != std::string::npos) {
                nmea_buffer = nmea_buffer.substr(pos + 1);
            }
        }
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <config.json>\n\n"
              << "Simple NTRIP Client - receives RTCM corrections and forwards GPS data\n\n"
              << "Create a config.json file with:\n"
              << "{\n"
              << "    \"ntrip_host\": \"polaris.pointonenav.com\",\n"
              << "    \"ntrip_port\": 2101,\n"
              << "    \"ntrip_mountpoint\": \"POLARIS\",\n"
              << "    \"ntrip_user\": \"username\",\n"
              << "    \"ntrip_password\": \"password\",\n"
#ifdef PLATFORM_WINDOWS
              << "    \"serial_port\": \"COM3\",\n"
#else
              << "    \"serial_port\": \"/dev/ttyUSB0\",\n"
#endif
              << "    \"serial_baud\": 115200,\n"
              << "    \"tcp_port\": 5000,\n"
              << "    \"gga_interval_sec\": 10\n"
              << "}\n\n"
              << "TCP Server: If tcp_port > 0, a TCP server is started that forwards\n"
              << "all serial port data to connected clients. Multiple clients supported.\n"
              << "Connect with: nc localhost 5000\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Initialize sockets (required for Windows)
    if (!TcpSocket::init()) {
        std::cerr << "Failed to initialize sockets" << std::endl;
        return 1;
    }
    
    // Parse config
    JsonConfig config;
    if (!config.parse(argv[1])) {
        TcpSocket::cleanup();
        return 1;
    }
    
    config.print();
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE from broken TCP connections
#endif
    
    // Open serial port
    SerialPort serial;
    if (!serial.open(config.serial_port, config.serial_baud)) {
        TcpSocket::cleanup();
        return 1;
    }
    std::cout << "Serial port opened: " << config.serial_port << std::endl;
    
    // Start TCP server if configured
    TcpServer tcp_server;
    TcpServer* tcp_ptr = nullptr;
    
    if (config.tcp_port > 0) {
        if (tcp_server.listen(config.tcp_port)) {
            std::cout << "TCP server listening on port " << config.tcp_port << std::endl;
            tcp_ptr = &tcp_server;
        } else {
            std::cerr << "Warning: TCP server failed to start, continuing without it" << std::endl;
        }
    }
    
    // Start threads
    std::thread ntrip_th(ntrip_thread, std::cref(config), std::ref(serial));
    std::thread serial_th(serial_thread, std::ref(serial), tcp_ptr);
    
    // Wait for threads
    serial_th.join();
    ntrip_th.join();
    
    // Cleanup
    serial.close();
    tcp_server.close();
    TcpSocket::cleanup();
    
    std::cout << "Shutdown complete" << std::endl;
    return 0;
}
