/*
 * serial_port.h - Cross-platform serial port handling
 * 
 * Supports Linux, macOS, and Windows serial communication.
 */

#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <string>
#include <iostream>
#include <cstring>

// Platform detection and includes
#ifdef _WIN32
    #define PLATFORM_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #define PLATFORM_UNIX
    #include <unistd.h>
    #include <fcntl.h>
    #include <termios.h>
    #include <errno.h>
#endif

class SerialPort {
public:
#ifdef PLATFORM_WINDOWS
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    
    SerialPort() = default;
    
    ~SerialPort() {
        close();
    }
    
    // Disable copy
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    
    // Allow move
    SerialPort(SerialPort&& other) noexcept {
#ifdef PLATFORM_WINDOWS
        handle = other.handle;
        other.handle = INVALID_HANDLE_VALUE;
#else
        fd = other.fd;
        other.fd = -1;
#endif
    }
    
    SerialPort& operator=(SerialPort&& other) noexcept {
        if (this != &other) {
            close();
#ifdef PLATFORM_WINDOWS
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
#else
            fd = other.fd;
            other.fd = -1;
#endif
        }
        return *this;
    }
    
    bool open(const std::string& port, int baud) {
#ifdef PLATFORM_WINDOWS
        return open_windows(port, baud);
#else
        return open_unix(port, baud);
#endif
    }
    
    void close() {
#ifdef PLATFORM_WINDOWS
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
    }
    
    int read(char* buf, size_t len) {
#ifdef PLATFORM_WINDOWS
        DWORD bytes_read = 0;
        if (!ReadFile(handle, buf, (DWORD)len, &bytes_read, NULL)) {
            return -1;
        }
        return (int)bytes_read;
#else
        return ::read(fd, buf, len);
#endif
    }
    
    int write(const char* buf, size_t len) {
#ifdef PLATFORM_WINDOWS
        DWORD bytes_written = 0;
        if (!WriteFile(handle, buf, (DWORD)len, &bytes_written, NULL)) {
            return -1;
        }
        return (int)bytes_written;
#else
        return ::write(fd, buf, len);
#endif
    }
    
    bool is_open() const {
#ifdef PLATFORM_WINDOWS
        return handle != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }
    
    // Get the last error message
    static std::string get_error_string() {
#ifdef PLATFORM_WINDOWS
        DWORD err = GetLastError();
        char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buf, sizeof(buf), NULL);
        return std::string(buf);
#else
        return std::string(strerror(errno));
#endif
    }
    
private:
#ifdef PLATFORM_WINDOWS
    bool open_windows(const std::string& port, int baud) {
        std::string port_name = port;
        // Handle COM port numbers >= 10
        if (port_name.find("\\\\.\\") != 0 && port_name.find("COM") == 0) {
            port_name = "\\\\.\\" + port_name;
        }
        
        handle = CreateFileA(port_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                            0, NULL, OPEN_EXISTING, 0, NULL);
        
        if (handle == INVALID_HANDLE_VALUE) {
            std::cerr << "Error opening serial port: " << GetLastError() << std::endl;
            return false;
        }
        
        DCB dcb = {0};
        dcb.DCBlength = sizeof(dcb);
        
        if (!GetCommState(handle, &dcb)) {
            std::cerr << "Error getting serial state: " << GetLastError() << std::endl;
            close();
            return false;
        }
        
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        
        if (!SetCommState(handle, &dcb)) {
            std::cerr << "Error setting serial state: " << GetLastError() << std::endl;
            close();
            return false;
        }
        
        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 10;
        timeouts.ReadTotalTimeoutConstant = 100;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 100;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        
        if (!SetCommTimeouts(handle, &timeouts)) {
            std::cerr << "Error setting timeouts: " << GetLastError() << std::endl;
            close();
            return false;
        }
        
        return true;
    }
#else
    bool open_unix(const std::string& port, int baud) {
        fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "Error opening serial port: " << strerror(errno) << std::endl;
            return false;
        }
        
        struct termios tty;
        memset(&tty, 0, sizeof(tty));
        
        if (tcgetattr(fd, &tty) != 0) {
            std::cerr << "Error getting serial attributes: " << strerror(errno) << std::endl;
            close();
            return false;
        }
        
        speed_t speed = baud_to_speed(baud);
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
        
        // Control flags
        tty.c_cflag |= (CLOCAL | CREAD);  // Enable receiver, ignore modem control lines
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;                // 8 data bits
        tty.c_cflag &= ~PARENB;            // No parity
        tty.c_cflag &= ~CSTOPB;            // 1 stop bit
        tty.c_cflag &= ~CRTSCTS;           // No hardware flow control
        
        // Local flags - raw input
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        
        // Input flags
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // No software flow control
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        
        // Output flags - raw output
        tty.c_oflag &= ~OPOST;
        
        // Read settings
        tty.c_cc[VMIN] = 0;   // Non-blocking read
        tty.c_cc[VTIME] = 1;  // 100ms timeout
        
        if (tcsetattr(fd, TCSANOW, &tty) != 0) {
            std::cerr << "Error setting serial attributes: " << strerror(errno) << std::endl;
            close();
            return false;
        }
        
        return true;
    }
    
    static speed_t baud_to_speed(int baud) {
        switch (baud) {
            case 1200:   return B1200;
            case 2400:   return B2400;
            case 4800:   return B4800;
            case 9600:   return B9600;
            case 19200:  return B19200;
            case 38400:  return B38400;
            case 57600:  return B57600;
            case 115200: return B115200;
            case 230400: return B230400;
#ifdef B460800
            case 460800: return B460800;
#endif
#ifdef B921600
            case 921600: return B921600;
#endif
            default:     return B9600;
        }
    }
#endif
};

#endif // SERIAL_PORT_H
