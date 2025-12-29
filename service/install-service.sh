#!/bin/bash
#
# Install NTRIP Client as a systemd service
#
# Usage: sudo ./install-service.sh [config_file]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_SRC="${1:-$SCRIPT_DIR/../config.json}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    log_error "Please run as root (use sudo)"
    exit 1
fi

# Check if binary exists
if [ ! -f "$SCRIPT_DIR/../ntrip_client" ]; then
    log_error "ntrip_client binary not found. Please build it first with 'make'"
    exit 1
fi

# Check if config exists
if [ ! -f "$CONFIG_SRC" ]; then
    log_error "Config file not found: $CONFIG_SRC"
    log_info "Usage: sudo ./install-service.sh [path/to/config.json]"
    exit 1
fi

log_info "Installing NTRIP Client systemd service..."

# Create ntrip user if it doesn't exist
if ! id -u ntrip &>/dev/null; then
    log_info "Creating 'ntrip' user..."
    useradd -r -s /usr/sbin/nologin -G dialout ntrip
else
    log_info "User 'ntrip' already exists"
    # Make sure user is in dialout group
    usermod -a -G dialout ntrip
fi

# Install binary
log_info "Installing binary to /usr/local/bin/..."
cp "$SCRIPT_DIR/../ntrip_client" /usr/local/bin/
chmod 755 /usr/local/bin/ntrip_client

# Install config
log_info "Installing config to /etc/ntrip/..."
mkdir -p /etc/ntrip
cp "$CONFIG_SRC" /etc/ntrip/config.json
chmod 640 /etc/ntrip/config.json
chown root:ntrip /etc/ntrip/config.json

# Install service file
log_info "Installing systemd service..."
cp "$SCRIPT_DIR/ntrip-client.service" /etc/systemd/system/
chmod 644 /etc/systemd/system/ntrip-client.service

# Reload systemd
log_info "Reloading systemd daemon..."
systemctl daemon-reload

log_info "Installation complete!"
echo ""
echo "Next steps:"
echo "  1. Edit config:        sudo nano /etc/ntrip/config.json"
echo "  2. Enable service:     sudo systemctl enable ntrip-client"
echo "  3. Start service:      sudo systemctl start ntrip-client"
echo "  4. Check status:       sudo systemctl status ntrip-client"
echo "  5. View logs:          sudo journalctl -u ntrip-client -f"
echo ""
echo "To uninstall: sudo $SCRIPT_DIR/uninstall-service.sh"
