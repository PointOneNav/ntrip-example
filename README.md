# Simple NTRIP Client

A lightweight, cross-platform NTRIP client for Linux, macOS, and Windows that:

- Connects to an NTRIP caster to receive RTCM correction data
- Reads GGA NMEA messages from a GPS receiver on a serial port
- Sends GGA position to the NTRIP caster for VRS (Virtual Reference Station) support
- Forwards RTCM corrections to the GPS receiver
- Optionally forwards all serial data to a TCP socket

## Building

### Linux / macOS

```bash
# Using make
make

# Or directly with g++
g++ -std=c++17 -O2 -o ntrip_client ntrip_client.cpp -pthread
```

### Windows

**Using Visual Studio Developer Command Prompt:**
```cmd
cl /EHsc /std:c++17 /O2 ntrip_client.cpp ws2_32.lib
```

**Using MinGW-w64:**
```cmd
g++ -std=c++17 -O2 -o ntrip_client.exe ntrip_client.cpp -lws2_32
```

## Configuration

Create a `config.json` file:

```json
{
    "ntrip_host": "ntrip.example.com",
    "ntrip_port": 2101,
    "ntrip_mountpoint": "RTCM3_MOUNT",
    "ntrip_user": "your_username",
    "ntrip_password": "your_password",
    
    "serial_port": "/dev/ttyUSB0",
    "serial_baud": 115200,
    
    "tcp_host": "localhost",
    "tcp_port": 5000,
    
    "gga_interval_sec": 10
}
```

**Note:** On Windows, use `"serial_port": "COM3"` (or appropriate COM port).

### Configuration Options

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `ntrip_host` | Yes | - | NTRIP caster hostname |
| `ntrip_port` | No | 2101 | NTRIP caster port |
| `ntrip_mountpoint` | Yes | - | Mountpoint name |
| `ntrip_user` | No | - | Username for authentication |
| `ntrip_password` | No | - | Password for authentication |
| `serial_port` | Yes | - | Serial port path (e.g., `/dev/ttyUSB0`, `/dev/tty.usbserial`) |
| `serial_baud` | No | 9600 | Baud rate (4800, 9600, 19200, 38400, 57600, 115200, 230400) |
| `tcp_host` | No | - | TCP host for forwarding serial data |
| `tcp_port` | No | 0 | TCP port for forwarding (0 = disabled) |
| `gga_interval_sec` | No | 10 | Interval for sending GGA to caster |

## Usage

```bash
./ntrip_client config.json
```

Press `Ctrl+C` to stop.

## Serial Port Permissions

On Linux, you may need to add your user to the `dialout` group:

```bash
sudo usermod -a -G dialout $USER
# Log out and back in for changes to take effect
```

On macOS, serial ports are typically accessible without additional configuration.

## Common Serial Port Paths

**Linux:**
- USB serial: `/dev/ttyUSB0`, `/dev/ttyUSB1`
- ACM devices: `/dev/ttyACM0`, `/dev/ttyACM1`

**macOS:**
- USB serial: `/dev/tty.usbserial-*`, `/dev/tty.usbmodem*`

**Windows:**
- COM ports: `COM1`, `COM3`, `COM10`, etc.

List available ports:
```bash
# Linux
ls /dev/tty*

# macOS
ls /dev/tty.*

# Windows (PowerShell)
[System.IO.Ports.SerialPort]::GetPortNames()

# Windows (Command Prompt)
mode
```

## How It Works

1. Opens the serial port and connects to the GPS receiver
2. Connects to the NTRIP caster and authenticates
3. Reads NMEA data from the serial port, parsing GGA messages
4. Sends GGA messages to the NTRIP caster periodically (for VRS)
5. Receives RTCM correction data from the caster
6. Writes RTCM data to the serial port (to the GPS receiver)
7. Optionally forwards all serial data to a TCP socket

## Dependencies

**None!** Uses only standard C++17 and platform APIs:
- Linux/macOS: POSIX (termios, sockets)
- Windows: Win32 API (serial), Winsock2 (sockets)

## Testing

The project includes a comprehensive test suite with unit tests and integration tests.

### Running Tests

```bash
# Run all tests
make test

# Run only unit tests
make test-unit

# Run only integration tests
make test-integration

# Run integration test with custom duration (default 15 seconds)
cd test && ./run_tests.sh integration 30
```

### Test Components

The test suite includes:

1. **Unit Tests**
   - GGA message extraction
   - Base64 encoding for authentication
   - JSON configuration parsing

2. **Integration Test**
   - Mock NTRIP server that validates connections and receives GGA
   - GPS simulator that generates NMEA sentences on a PTY
   - Full end-to-end data flow verification

### Manual Testing

You can also run the test components manually:

```bash
# Build test binaries
make test-binaries

# Terminal 1: Start mock NTRIP server
./build/mock_ntrip_server -p 2101 -m TEST -u user -P pass

# Terminal 2: Start GPS simulator (note the PTY path it prints)
./build/gps_simulator

# Terminal 3: Create test config and run client
# (use PTY path from GPS simulator output as serial_port)
./ntrip_client test_config.json
```

## Running as a Linux systemd Service

The `systemd/` directory contains everything needed to run the NTRIP client as a system service on Linux.

### Quick Install

```bash
# Build the client
make

# Install the service (uses config.json from current directory)
sudo ./systemd/install-service.sh

# Or specify a custom config file
sudo ./systemd/install-service.sh /path/to/my-config.json
```

### Manual Installation

1. **Create a dedicated user:**
   ```bash
   sudo useradd -r -s /usr/sbin/nologin -G dialout ntrip
   ```

2. **Install the binary:**
   ```bash
   sudo cp ntrip_client /usr/local/bin/
   sudo chmod 755 /usr/local/bin/ntrip_client
   ```

3. **Install the configuration:**
   ```bash
   sudo mkdir -p /etc/ntrip
   sudo cp config.json /etc/ntrip/
   sudo chmod 640 /etc/ntrip/config.json
   sudo chown root:ntrip /etc/ntrip/config.json
   ```

4. **Install the service file:**
   ```bash
   sudo cp systemd/ntrip-client.service /etc/systemd/system/
   sudo systemctl daemon-reload
   ```

5. **Enable and start the service:**
   ```bash
   sudo systemctl enable ntrip-client
   sudo systemctl start ntrip-client
   ```

### Service Management

```bash
# Check service status
sudo systemctl status ntrip-client

# View logs (live)
sudo journalctl -u ntrip-client -f

# View recent logs
sudo journalctl -u ntrip-client --since "1 hour ago"

# Restart after config change
sudo systemctl restart ntrip-client

# Stop the service
sudo systemctl stop ntrip-client

# Disable auto-start
sudo systemctl disable ntrip-client
```

### Configuration Location

When running as a service, the configuration file is located at:
```
/etc/ntrip/config.json
```

Edit it with:
```bash
sudo nano /etc/ntrip/config.json
sudo systemctl restart ntrip-client
```

### Serial Port Permissions

The service runs as the `ntrip` user which is added to the `dialout` group. This provides access to serial ports. If you're using a USB-serial adapter, make sure it appears as `/dev/ttyUSB*` or `/dev/ttyACM*`.

If your device uses a different path, edit the service file:
```bash
sudo systemctl edit ntrip-client
```

Add:
```ini
[Service]
DeviceAllow=/dev/your-device rw
```

### Troubleshooting

**Service won't start:**
```bash
# Check for errors
sudo journalctl -u ntrip-client -e

# Test config manually
sudo -u ntrip /usr/local/bin/ntrip_client /etc/ntrip/config.json
```

**Permission denied on serial port:**
```bash
# Check device permissions
ls -la /dev/ttyUSB0

# Verify ntrip user is in dialout group
groups ntrip

# Add to dialout group if needed
sudo usermod -a -G dialout ntrip
sudo systemctl restart ntrip-client
```

**Service keeps restarting:**
- Check logs for connection errors
- Verify NTRIP server credentials
- Check network connectivity

### Uninstall

```bash
sudo ./systemd/uninstall-service.sh
```

Or manually:
```bash
sudo systemctl stop ntrip-client
sudo systemctl disable ntrip-client
sudo rm /etc/systemd/system/ntrip-client.service
sudo systemctl daemon-reload
sudo rm /usr/local/bin/ntrip_client
sudo rm -rf /etc/ntrip
sudo userdel ntrip
```

## License

MIT License - feel free to use and modify.
