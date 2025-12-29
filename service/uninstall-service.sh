#!/bin/bash
#
# Uninstall NTRIP Client systemd service
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}[ERROR]${NC} Please run as root (use sudo)"
    exit 1
fi

log_info "Uninstalling NTRIP Client systemd service..."

# Stop and disable service
if systemctl is-active --quiet ntrip-client; then
    log_info "Stopping service..."
    systemctl stop ntrip-client
fi

if systemctl is-enabled --quiet ntrip-client 2>/dev/null; then
    log_info "Disabling service..."
    systemctl disable ntrip-client
fi

# Remove service file
if [ -f /etc/systemd/system/ntrip-client.service ]; then
    log_info "Removing service file..."
    rm /etc/systemd/system/ntrip-client.service
fi

# Reload systemd
systemctl daemon-reload

# Ask about config removal
echo ""
read -p "Remove config directory /etc/ntrip? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    log_info "Removing config directory..."
    rm -rf /etc/ntrip
fi

# Ask about binary removal
read -p "Remove binary /usr/local/bin/ntrip_client? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    log_info "Removing binary..."
    rm -f /usr/local/bin/ntrip_client
fi

# Ask about user removal
if id -u ntrip &>/dev/null; then
    read -p "Remove 'ntrip' user? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        log_info "Removing user..."
        userdel ntrip
    fi
fi

log_info "Uninstall complete!"
