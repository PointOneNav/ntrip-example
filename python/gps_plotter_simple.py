#!/usr/bin/env python3
"""
GPS Position Plotter (Simple Version)

Connects to the NTRIP client's TCP server, parses NMEA sentences,
and displays the position on an interactive map.

This version creates a single HTML file that auto-updates.

Requirements:
    pip install plotly

Usage:
    python gps_plotter_simple.py [host] [port]
    python gps_plotter_simple.py localhost 5000
"""

import socket
import sys
import time
from datetime import datetime
from dataclasses import dataclass
from typing import Optional
import webbrowser
import tempfile
import os

try:
    import plotly.graph_objects as go
except ImportError:
    print("Error: plotly not installed.")
    print("Install with: pip install plotly")
    sys.exit(1)


@dataclass 
class GpsPosition:
    latitude: float
    longitude: float
    altitude: float
    fix_quality: int
    num_satellites: int
    hdop: float


def parse_nmea_coord(coord: str, direction: str) -> Optional[float]:
    """Parse NMEA coordinate to decimal degrees"""
    if not coord or not direction:
        return None
    try:
        dot = coord.index('.')
        deg = int(coord[:dot-2])
        mins = float(coord[dot-2:])
        val = deg + mins / 60.0
        return -val if direction in ('S', 'W') else val
    except:
        return None


def parse_gga(line: str) -> Optional[GpsPosition]:
    """Parse GGA sentence"""
    try:
        if '*' in line:
            line = line.split('*')[0]
        p = line.split(',')
        if len(p) < 15 or p[0] not in ('$GPGGA', '$GNGGA'):
            return None
        
        lat = parse_nmea_coord(p[2], p[3])
        lon = parse_nmea_coord(p[4], p[5])
        if lat is None or lon is None:
            return None
        
        return GpsPosition(
            latitude=lat,
            longitude=lon,
            altitude=float(p[9]) if p[9] else 0,
            fix_quality=int(p[6]) if p[6] else 0,
            num_satellites=int(p[7]) if p[7] else 0,
            hdop=float(p[8]) if p[8] else 99
        )
    except:
        return None


def get_fix_color(fix: int) -> str:
    """Get color for fix quality"""
    return {4: 'green', 5: 'yellow', 2: 'blue', 1: 'orange'}.get(fix, 'red')


def get_fix_name(fix: int) -> str:
    """Get name for fix quality"""
    return {0: 'No Fix', 1: 'GPS', 2: 'DGPS', 4: 'RTK Fixed', 5: 'RTK Float'}.get(fix, 'Unknown')


def create_map(positions: list[GpsPosition]) -> go.Figure:
    """Create map figure"""
    fig = go.Figure()
    
    if not positions:
        # Empty map centered on SF
        fig.update_layout(
            mapbox=dict(style="open-street-map", center=dict(lat=37.77, lon=-122.42), zoom=12),
            height=800, margin=dict(l=0,r=0,t=50,b=0),
            title="GPS Tracker - Waiting for data..."
        )
        return fig
    
    lats = [p.latitude for p in positions]
    lons = [p.longitude for p in positions]
    colors = [get_fix_color(p.fix_quality) for p in positions]
    
    # Track line
    fig.add_trace(go.Scattermapbox(
        lat=lats, lon=lons, mode='lines',
        line=dict(width=2, color='royalblue'),
        name='Track', hoverinfo='skip'
    ))
    
    # Points
    hover = [f"Lat: {p.latitude:.6f}<br>Lon: {p.longitude:.6f}<br>"
             f"Alt: {p.altitude:.1f}m<br>Sats: {p.num_satellites}<br>"
             f"Fix: {get_fix_name(p.fix_quality)}" for p in positions]
    
    fig.add_trace(go.Scattermapbox(
        lat=lats, lon=lons, mode='markers',
        marker=dict(size=8, color=colors),
        text=hover, hoverinfo='text', name='Positions'
    ))
    
    # Current position
    cur = positions[-1]
    fig.add_trace(go.Scattermapbox(
        lat=[cur.latitude], lon=[cur.longitude], mode='markers',
        marker=dict(size=15, color='red'),
        name='Current', hoverinfo='skip'
    ))
    
    # Layout
    center_lat = sum(lats) / len(lats)
    center_lon = sum(lons) / len(lons)
    
    # Auto zoom
    lat_range = max(lats) - min(lats)
    lon_range = max(lons) - min(lons)
    span = max(lat_range, lon_range)
    zoom = 18 if span < 0.0005 else 16 if span < 0.002 else 14 if span < 0.01 else 12 if span < 0.05 else 10
    
    fig.update_layout(
        mapbox=dict(
            style="open-street-map",
            center=dict(lat=center_lat, lon=center_lon),
            zoom=zoom
        ),
        height=800,
        margin=dict(l=0, r=0, t=50, b=0),
        title=f"GPS Tracker | {len(positions)} pts | "
              f"Lat: {cur.latitude:.6f} Lon: {cur.longitude:.6f} | "
              f"Alt: {cur.altitude:.1f}m | Sats: {cur.num_satellites} | "
              f"Fix: {get_fix_name(cur.fix_quality)}",
        showlegend=False
    )
    
    return fig


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    
    print(f"GPS Plotter - Connecting to {host}:{port}")
    print("Press Ctrl+C to stop\n")
    
    positions: list[GpsPosition] = []
    html_file = os.path.join(tempfile.gettempdir(), "gps_map.html")
    browser_opened = False
    
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((host, port))
            print(f"Connected to {host}:{port}")
            sock.settimeout(1)
            
            buffer = ""
            last_update = 0
            
            while True:
                try:
                    data = sock.recv(1024)
                    if not data:
                        break
                    
                    buffer += data.decode('ascii', errors='ignore')
                    
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        pos = parse_gga(line.strip())
                        if pos:
                            positions.append(pos)
                            if len(positions) > 2000:
                                positions = positions[-1000:]
                            
                            # Update display
                            print(f"\rLat: {pos.latitude:11.6f}  Lon: {pos.longitude:12.6f}  "
                                  f"Alt: {pos.altitude:6.1f}m  Sats: {pos.num_satellites:2d}  "
                                  f"Fix: {get_fix_name(pos.fix_quality):10s}  "
                                  f"Pts: {len(positions)}", end='', flush=True)
                    
                    # Update map every 2 seconds
                    now = time.time()
                    if now - last_update >= 2 and positions:
                        fig = create_map(positions)
                        fig.write_html(html_file, auto_open=False)
                        last_update = now
                        
                        if not browser_opened:
                            webbrowser.open(f"file://{html_file}")
                            browser_opened = True
                            print(f"\nMap opened in browser: {html_file}")
                            print("Refresh browser to see updates\n")
                
                except socket.timeout:
                    continue
                except KeyboardInterrupt:
                    raise
        
        except socket.error as e:
            print(f"\nConnection error: {e}")
        except KeyboardInterrupt:
            print("\n\nStopping...")
            break
        
        print("Reconnecting in 3s...")
        time.sleep(3)
    
    # Final map
    if positions:
        fig = create_map(positions)
        fig.write_html(html_file)
        print(f"Final map saved: {html_file}")


if __name__ == "__main__":
    main()
