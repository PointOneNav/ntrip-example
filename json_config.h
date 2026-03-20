/*
 * json_config.h - Simple JSON configuration parser
 * 
 * A minimal JSON parser for configuration files.
 * Handles basic key-value pairs with string and integer values.
 */

#ifndef JSON_CONFIG_H
#define JSON_CONFIG_H

#include <string>
#include <fstream>
#include <iostream>
#include <cctype>

class JsonConfig {
public:
    // NTRIP settings
    std::string ntrip_host;
    int ntrip_port = 2101;
    std::string ntrip_mountpoint;
    std::string ntrip_user;
    std::string ntrip_password;
    
    // Serial port settings
    std::string serial_port;
    int serial_baud = 9600;
    
    // TCP server settings (for forwarding serial data)
    int tcp_port = 0;  // 0 = disabled
    
    // Other settings
    int gga_interval_sec = 10;
    bool log_serial = true;  // Log raw serial data to a timestamped .log file
    
    bool parse(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open config file: " << filename << std::endl;
            return false;
        }
        
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        
        ntrip_host = get_string(content, "ntrip_host");
        ntrip_port = get_int(content, "ntrip_port", 2101);
        ntrip_mountpoint = get_string(content, "ntrip_mountpoint");
        ntrip_user = get_string(content, "ntrip_user");
        ntrip_password = get_string(content, "ntrip_password");
        
        serial_port = get_string(content, "serial_port");
        serial_baud = get_int(content, "serial_baud", 9600);
        
        tcp_port = get_int(content, "tcp_port", 0);
        
        gga_interval_sec = get_int(content, "gga_interval_sec", 10);
        log_serial = get_bool(content, "log_serial", true);

        return validate();
    }
    
    bool validate() const {
        if (ntrip_host.empty()) { 
            std::cerr << "Error: ntrip_host required\n"; 
            return false; 
        }
        if (ntrip_mountpoint.empty()) { 
            std::cerr << "Error: ntrip_mountpoint required\n"; 
            return false; 
        }
        if (serial_port.empty()) { 
            std::cerr << "Error: serial_port required\n"; 
            return false; 
        }
        return true;
    }
    
    void print() const {
        std::cout << "Configuration:\n"
                  << "  NTRIP: " << ntrip_host << ":" << ntrip_port << "/" << ntrip_mountpoint << "\n"
                  << "  Serial: " << serial_port << " @ " << serial_baud << " baud\n";
        if (tcp_port > 0) {
            std::cout << "  TCP Server: port " << tcp_port << "\n";
        }
        std::cout << "  GGA Interval: " << gga_interval_sec << "s\n"
                  << "  Serial Logging: " << (log_serial ? "enabled" : "disabled") << "\n";
    }
    
    // Static helper methods for parsing JSON - can be used externally
    static std::string get_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        
        pos = json.find('"', pos);
        if (pos == std::string::npos) return "";
        
        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        
        return json.substr(pos + 1, end - pos - 1);
    }
    
    static int get_int(const std::string& json, const std::string& key, int def = 0) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return def;
        
        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;
        
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        
        std::string num;
        while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '-')) {
            num += json[pos++];
        }
        
        return num.empty() ? def : std::stoi(num);
    }
    
    static bool get_bool(const std::string& json, const std::string& key, bool def = false) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return def;
        
        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;
        
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        
        if (json.substr(pos, 4) == "true") return true;
        if (json.substr(pos, 5) == "false") return false;
        
        return def;
    }
};

#endif // JSON_CONFIG_H
