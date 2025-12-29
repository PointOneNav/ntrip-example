#!/usr/bin/env python3
"""
GPS Position Plotter

Connects to the NTRIP client's TCP server, parses NMEA sentences,
and displays the position on an interactive map.

Requirements:
    pip install plotly

Usage:
    python gps_plotter.py [gps_host] [gps_port] [http_port]
    python gps_plotter.py localhost 5000 8080

Environment Variables:
    MAPBOX_TOKEN - Optional Mapbox access token for satellite imagery
                   Get a free token at https://mapbox.com
                   If not set, uses ESRI satellite imagery (no token needed)
"""

import socket
import sys
import threading
import time
import json
import os
from datetime import datetime
from collections import deque
from dataclasses import dataclass
from typing import Optional
import webbrowser
import http.server
import socketserver

# Get Mapbox token from environment (optional)
# Supports both MAPBOX_TOKEN and MapboxAccessToken
MAPBOX_TOKEN = os.environ.get('MAPBOX_TOKEN') or os.environ.get('MapboxAccessToken', '')


@dataclass
class GpsPosition:
    """GPS position data from GGA sentence"""
    timestamp: str
    latitude: float
    longitude: float
    altitude: float
    fix_quality: int
    num_satellites: int
    hdop: float
    received_at: datetime


@dataclass
class SatelliteInfo:
    """Satellite info from GSV sentence"""
    prn: int          # Satellite PRN number
    elevation: int    # Elevation in degrees
    azimuth: int      # Azimuth in degrees
    snr: int          # Signal-to-noise ratio (C/N0) in dB-Hz
    constellation: str  # GPS, GLONASS, Galileo, etc.


def parse_gsv(sentence: str) -> list[SatelliteInfo]:
    """Parse GSV NMEA sentence for satellite CNO data
    
    GSV format: $GPGSV,numMsg,msgNum,numSV,prn1,elev1,az1,snr1,prn2,elev2,az2,snr2,...*cs
    Each message can contain up to 4 satellites
    """
    satellites = []
    try:
        if '*' in sentence:
            sentence = sentence.split('*')[0]
        
        parts = sentence.split(',')
        
        if len(parts) < 4:
            return satellites
        
        # Determine constellation from sentence type
        msg_type = parts[0]
        if msg_type in ('$GPGSV',):
            constellation = 'GPS'
        elif msg_type in ('$GLGSV',):
            constellation = 'GLO'
        elif msg_type in ('$GAGSV',):
            constellation = 'GAL'
        elif msg_type in ('$GBGSV', '$BDGSV'):
            constellation = 'BDS'
        elif msg_type in ('$GQGSV',):
            constellation = 'QZSS'
        elif msg_type in ('$GNGSV',):
            constellation = 'GNSS'
        else:
            return satellites
        
        # Parse satellite data (4 satellites per message, 4 fields each)
        # Fields start at index 4: prn, elevation, azimuth, snr
        i = 4
        while i + 3 < len(parts):
            try:
                prn = int(parts[i]) if parts[i] else 0
                elev = int(parts[i + 1]) if parts[i + 1] else 0
                az = int(parts[i + 2]) if parts[i + 2] else 0
                snr = int(parts[i + 3]) if parts[i + 3] else 0
                
                if prn > 0:
                    satellites.append(SatelliteInfo(
                        prn=prn,
                        elevation=elev,
                        azimuth=az,
                        snr=snr,
                        constellation=constellation
                    ))
            except (ValueError, IndexError):
                pass
            i += 4
        
    except (ValueError, IndexError):
        pass
    
    return satellites


def parse_nmea_coordinate(coord: str, direction: str) -> Optional[float]:
    """Parse NMEA coordinate (DDMM.MMMM or DDDMM.MMMM) to decimal degrees"""
    if not coord or not direction:
        return None
    
    try:
        dot_pos = coord.index('.')
        if dot_pos <= 2:
            return None
        
        degrees = int(coord[:dot_pos - 2])
        minutes = float(coord[dot_pos - 2:])
        decimal = degrees + minutes / 60.0
        
        if direction in ('S', 'W'):
            decimal = -decimal
        
        return decimal
    except (ValueError, IndexError):
        return None


def parse_gga(sentence: str) -> Optional[GpsPosition]:
    """Parse GGA NMEA sentence"""
    try:
        if '*' in sentence:
            sentence = sentence.split('*')[0]
        
        parts = sentence.split(',')
        
        if len(parts) < 15:
            return None
        
        if parts[0] not in ('$GPGGA', '$GNGGA'):
            return None
        
        timestamp = parts[1]
        lat = parse_nmea_coordinate(parts[2], parts[3])
        lon = parse_nmea_coordinate(parts[4], parts[5])
        
        if lat is None or lon is None:
            return None
        
        fix_quality = int(parts[6]) if parts[6] else 0
        num_sats = int(parts[7]) if parts[7] else 0
        hdop = float(parts[8]) if parts[8] else 99.9
        altitude = float(parts[9]) if parts[9] else 0.0
        
        return GpsPosition(
            timestamp=timestamp,
            latitude=lat,
            longitude=lon,
            altitude=altitude,
            fix_quality=fix_quality,
            num_satellites=num_sats,
            hdop=hdop,
            received_at=datetime.now()
        )
    except (ValueError, IndexError):
        return None


class GpsReceiver:
    """Receives GPS data from TCP server"""
    
    def __init__(self, host: str, port: int, max_positions: int = 1000):
        self.host = host
        self.port = port
        self.max_positions = max_positions
        self.positions: deque[GpsPosition] = deque(maxlen=max_positions)
        self.current_position: Optional[GpsPosition] = None
        self.satellites: dict[str, tuple[SatelliteInfo, float]] = {}  # keyed by "constellation-prn", value is (info, timestamp)
        self.running = False
        self.connected = False
        self.socket: Optional[socket.socket] = None
        self.thread: Optional[threading.Thread] = None
        self.lock = threading.Lock()
        self.error_message: Optional[str] = None
        self.satellite_timeout = 3.0  # seconds before evicting stale satellites
    
    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.thread.start()
    
    def stop(self):
        self.running = False
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
    
    def _receive_loop(self):
        buffer = ""
        
        while self.running:
            try:
                self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.socket.settimeout(5.0)
                self.socket.connect((self.host, self.port))
                self.connected = True
                self.error_message = None
                print(f"Connected to GPS server {self.host}:{self.port}")
                
                self.socket.settimeout(1.0)
                
                while self.running:
                    try:
                        data = self.socket.recv(1024)
                        if not data:
                            break
                        
                        buffer += data.decode('ascii', errors='ignore')
                        
                        while '\n' in buffer:
                            line, buffer = buffer.split('\n', 1)
                            line = line.strip()
                            
                            if line.startswith('$'):
                                # Parse GGA for position
                                pos = parse_gga(line)
                                if pos:
                                    with self.lock:
                                        self.positions.append(pos)
                                        self.current_position = pos
                                
                                # Parse GSV for satellite CNO
                                if 'GSV' in line:
                                    sats = parse_gsv(line)
                                    if sats:
                                        now = time.time()
                                        with self.lock:
                                            for sat in sats:
                                                key = f"{sat.constellation}-{sat.prn}"
                                                self.satellites[key] = (sat, now)
                    
                    except socket.timeout:
                        continue
                
            except socket.error as e:
                self.error_message = str(e)
                self.connected = False
                print(f"Connection error: {e}")
            
            finally:
                self.connected = False
                if self.socket:
                    try:
                        self.socket.close()
                    except:
                        pass
            
            if self.running:
                print("Reconnecting in 3 seconds...")
                time.sleep(3)
    
    def get_positions(self) -> list[GpsPosition]:
        with self.lock:
            return list(self.positions)
    
    def get_current(self) -> Optional[GpsPosition]:
        with self.lock:
            return self.current_position
    
    def get_satellites(self) -> list[SatelliteInfo]:
        with self.lock:
            now = time.time()
            # Evict stale satellites
            stale_keys = [k for k, (sat, ts) in self.satellites.items() 
                          if now - ts > self.satellite_timeout]
            for k in stale_keys:
                del self.satellites[k]
            # Return current satellites
            return [sat for sat, ts in self.satellites.values()]
    
    def get_data_json(self) -> dict:
        """Get current data as JSON-serializable dict"""
        positions = self.get_positions()
        satellites = self.get_satellites()
        
        # Sort satellites by constellation order (GPS, GLO, GAL, BDS) then PRN
        constellation_order = {'GPS': 0, 'GLO': 1, 'GAL': 2, 'BDS': 3, 'QZSS': 4, 'GNSS': 5}
        satellites.sort(key=lambda s: (constellation_order.get(s.constellation, 99), s.prn))
        
        return {
            "connected": self.connected,
            "error": self.error_message,
            "positions": [
                {
                    "lat": p.latitude,
                    "lon": p.longitude,
                    "alt": p.altitude,
                    "sats": p.num_satellites,
                    "hdop": p.hdop,
                    "fix": p.fix_quality
                }
                for p in positions[-500:]
            ],
            "satellites": [
                {
                    "id": f"{s.constellation}{s.prn}",
                    "prn": s.prn,
                    "constellation": s.constellation,
                    "snr": s.snr,
                    "elevation": s.elevation,
                    "azimuth": s.azimuth
                }
                for s in satellites if s.snr > 0  # Only include satellites with signal
            ]
        }


# Global receiver reference for HTTP handler
gps_receiver: Optional[GpsReceiver] = None


class GpsHttpHandler(http.server.BaseHTTPRequestHandler):
    """HTTP handler that serves GPS data"""
    
    def log_message(self, format, *args):
        pass  # Suppress HTTP logging
    
    def do_GET(self):
        if self.path.startswith('/gps_data.json'):
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            
            if gps_receiver:
                data = gps_receiver.get_data_json()
            else:
                data = {"connected": False, "error": "No receiver", "positions": []}
            
            self.wfile.write(json.dumps(data).encode())
        
        elif self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(get_html_content().encode())
        
        else:
            self.send_error(404)


def get_html_content() -> str:
    """Return the HTML page content"""
    
    # Choose map style based on whether we have a Mapbox token
    if MAPBOX_TOKEN:
        map_config_js = f'''{{
                    style: 'mapbox://styles/mapbox/satellite-streets-v12',
                    accesstoken: '{MAPBOX_TOKEN}',
                    center: {{ lat: 37.7749, lon: -122.4194 }},
                    zoom: 17
                }}'''
    else:
        # Use ESRI satellite tiles with proper mapbox-gl style spec
        map_config_js = '''{
                    style: {
                        version: 8,
                        sources: {
                            'esri-satellite': {
                                type: 'raster',
                                tiles: [
                                    'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}'
                                ],
                                tileSize: 256
                            }
                        },
                        layers: [
                            {
                                id: 'esri-satellite-layer',
                                type: 'raster',
                                source: 'esri-satellite'
                            }
                        ]
                    },
                    center: { lat: 37.7749, lon: -122.4194 },
                    zoom: 17
                }'''
    
    return '''<!DOCTYPE html>
<html>
<head>
    <title>GPS Position Tracker</title>
    <script src="https://cdn.plot.ly/plotly-3.3.1.min.js" charset="utf-8"></script>
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <style>
        * {
            box-sizing: border-box;
        }
        html, body {
            margin: 0;
            padding: 0;
            height: 100%;
            overflow: hidden;
        }
        body {
            font-family: Arial, sans-serif;
            padding: 8px;
            background: #1a1a1a;
            color: #fff;
            display: flex;
            flex-direction: column;
        }
        #header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
            flex-shrink: 0;
        }
        #logo {
            height: 50px;
        }
        #status {
            padding: 8px 12px;
            border-radius: 5px;
            font-weight: bold;
            font-size: 14px;
        }
        .connected { background: #2d5a2d; }
        .disconnected { background: #5a2d2d; }
        #info {
            display: flex;
            gap: 8px;
            margin-bottom: 8px;
            flex-wrap: wrap;
            flex-shrink: 0;
        }
        .info-box {
            background: #2d2d2d;
            padding: 6px 12px;
            border-radius: 5px;
            min-width: 70px;
        }
        .info-box .label {
            color: #888;
            font-size: 10px;
        }
        .info-box .value {
            font-size: 14px;
            font-weight: bold;
        }
        .fix-0 { color: #f44; }
        .fix-1 { color: #fa4; }
        .fix-2 { color: #4af; }
        .fix-4 { color: #4f4; }
        .fix-5 { color: #ff4; }
        #map-container {
            flex: 1;
            min-height: 0;
            display: flex;
            flex-direction: column;
        }
        #map {
            width: 100%;
            flex: 1;
            min-height: 100px;
        }
        #snr-chart {
            width: 100%;
            height: 120px;
            flex-shrink: 0;
            margin-top: 8px;
        }
        
        /* Landscape mobile */
        @media (max-height: 500px) {
            #info {
                gap: 4px;
                margin-bottom: 4px;
            }
            .info-box {
                padding: 4px 8px;
                min-width: 60px;
            }
            .info-box .label {
                font-size: 8px;
            }
            .info-box .value {
                font-size: 12px;
            }
            #logo {
                height: 30px;
            }
            #status {
                padding: 6px 10px;
                font-size: 12px;
            }
            #snr-chart {
                height: 80px;
                margin-top: 4px;
            }
            body {
                padding: 4px;
            }
            #header {
                margin-bottom: 4px;
            }
        }
        
        /* Portrait mobile */
        @media (max-width: 600px) {
            .info-box {
                padding: 4px 8px;
                min-width: 55px;
            }
            .info-box .label {
                font-size: 9px;
            }
            .info-box .value {
                font-size: 13px;
            }
        }
    </style>
</head>
<body>
    <div id="header">
        <div id="status" class="disconnected">Connecting...</div>
        <a href="https://pointonenav.com" target="_blank">
            <img id="logo" src="https://pointonenav.com/wp-content/uploads/2024/06/logo-12.png" alt="Point One Navigation">
        </a>
    </div>
    <div id="info">
        <div class="info-box">
            <div class="label">Latitude</div>
            <div class="value" id="lat">--</div>
        </div>
        <div class="info-box">
            <div class="label">Longitude</div>
            <div class="value" id="lon">--</div>
        </div>
        <div class="info-box">
            <div class="label">Altitude</div>
            <div class="value" id="alt">--</div>
        </div>
        <div class="info-box">
            <div class="label">Satellites</div>
            <div class="value" id="sats">--</div>
        </div>
        <div class="info-box">
            <div class="label">Fix Quality</div>
            <div class="value" id="fix">--</div>
        </div>
        <div class="info-box">
            <div class="label">HDOP</div>
            <div class="value" id="hdop">--</div>
        </div>
        <div class="info-box">
            <div class="label">Points</div>
            <div class="value" id="points">0</div>
        </div>
    </div>
    <div id="map-container">
        <div id="map"></div>
        <div id="snr-chart"></div>
    </div>
    
    <script>
        const FIX_NAMES = {
            0: 'No Fix',
            1: 'GPS',
            2: 'DGPS',
            4: 'RTK Fixed',
            5: 'RTK Float'
        };
        
        const FIX_COLORS = {
            0: 'red',
            1: 'orange',
            2: 'blue',
            4: 'green',
            5: 'yellow'
        };
        
        const CONSTELLATION_COLORS = {
            'GPS': '#4CAF50',
            'GLO': '#F44336',
            'GAL': '#2196F3',
            'BDS': '#FF9800',
            'QZSS': '#9C27B0',
            'GNSS': '#607D8B'
        };
        
        let positions = [];
        let lastCenter = null;
        
        function initMap() {
            const data = [
                {
                    type: 'scattermapbox',
                    lat: [],
                    lon: [],
                    mode: 'lines',
                    line: { width: 2, color: 'cyan' },
                    name: 'Track'
                },
                {
                    type: 'scattermapbox',
                    lat: [],
                    lon: [],
                    mode: 'markers',
                    marker: { size: 8, color: [] },
                    name: 'Positions'
                },
                {
                    type: 'scattermapbox',
                    lat: [],
                    lon: [],
                    mode: 'markers',
                    marker: { size: 15, color: 'red' },
                    name: 'Current'
                }
            ];
            
            const layout = {
                mapbox: ''' + map_config_js + ''',
                showlegend: false,
                margin: { l: 0, r: 0, t: 0, b: 0 },
                uirevision: 'constant'
            };
            
            const config = {
                scrollZoom: true,
                doubleClick: false
            };
            
            Plotly.newPlot('map', data, layout, config);
        }
        
        function initSnrChart() {
            const data = [{
                type: 'bar',
                x: [],
                y: [],
                marker: { color: [] },
                text: [],
                textposition: 'outside',
                hovertemplate: '%{x}<br>SNR: %{y} dB-Hz<br>El: %{customdata[0]}°  Az: %{customdata[1]}°<extra></extra>',
                customdata: []
            }];
            
            const layout = {
                paper_bgcolor: '#1a1a1a',
                plot_bgcolor: '#1a1a1a',
                font: { color: '#fff', size: 10 },
                margin: { l: 40, r: 10, t: 5, b: 30 },
                xaxis: {
                    tickangle: -45,
                    tickfont: { size: 9 }
                },
                yaxis: {
                    title: 'C/N0 (dB-Hz)',
                    titlefont: { size: 10 },
                    range: [0, 55],
                    gridcolor: '#333'
                },
                bargap: 0.3
            };
            
            Plotly.newPlot('snr-chart', data, layout, { displayModeBar: false });
        }
        
        function updateSnrChart(satellites) {
            if (!satellites || satellites.length === 0) return;
            
            const ids = satellites.map(s => s.id);
            const snrs = satellites.map(s => s.snr);
            const colors = satellites.map(s => CONSTELLATION_COLORS[s.constellation] || '#888');
            const customdata = satellites.map(s => [s.elevation, s.azimuth]);
            
            Plotly.update('snr-chart', {
                x: [ids],
                y: [snrs],
                'marker.color': [colors],
                customdata: [customdata]
            });
        }
        
        function updateMap() {
            if (positions.length === 0) return;
            
            const lats = positions.map(p => p.lat);
            const lons = positions.map(p => p.lon);
            const colors = positions.map(p => FIX_COLORS[p.fix] || 'gray');
            
            const current = positions[positions.length - 1];
            
            Plotly.update('map', {
                lat: [lats, lats, [current.lat]],
                lon: [lons, lons, [current.lon]],
                'marker.color': [null, colors, null]
            });
            
            // Recenter on first point or occasionally
            if (!lastCenter || positions.length % 20 === 1) {
                Plotly.relayout('map', {
                    'mapbox.center': { lat: current.lat, lon: current.lon }
                });
                lastCenter = { lat: current.lat, lon: current.lon };
            }
        }
        
        function updateInfo(pos) {
            document.getElementById('lat').textContent = pos.lat.toFixed(6);
            document.getElementById('lon').textContent = pos.lon.toFixed(6);
            document.getElementById('alt').textContent = pos.alt.toFixed(1) + ' m';
            document.getElementById('sats').textContent = pos.sats;
            document.getElementById('hdop').textContent = pos.hdop.toFixed(1);
            document.getElementById('points').textContent = positions.length;
            
            const fixEl = document.getElementById('fix');
            fixEl.textContent = FIX_NAMES[pos.fix] || 'Unknown';
            fixEl.className = 'value fix-' + pos.fix;
        }
        
        async function fetchData() {
            try {
                const response = await fetch('/gps_data.json?' + Date.now());
                const data = await response.json();
                
                const statusEl = document.getElementById('status');
                statusEl.className = data.connected ? 'connected' : 'disconnected';
                statusEl.textContent = data.connected ? 'Connected' : 'Disconnected: ' + (data.error || 'Waiting...');
                
                if (data.positions && data.positions.length > 0) {
                    positions = data.positions;
                    updateMap();
                    updateInfo(positions[positions.length - 1]);
                }
                
                if (data.satellites) {
                    updateSnrChart(data.satellites);
                }
            } catch (e) {
                console.error('Fetch error:', e);
                document.getElementById('status').className = 'disconnected';
                document.getElementById('status').textContent = 'Error: ' + e.message;
            }
        }
        
        initMap();
        initSnrChart();
        fetchData();
        setInterval(fetchData, 1000);
        
        // Handle window resize
        let resizeTimeout;
        window.addEventListener('resize', function() {
            clearTimeout(resizeTimeout);
            resizeTimeout = setTimeout(function() {
                Plotly.Plots.resize('map');
                Plotly.Plots.resize('snr-chart');
            }, 100);
        });
    </script>
</body>
</html>
'''


def run_http_server(port: int):
    """Run HTTP server in background thread"""
    with socketserver.TCPServer(("", port), GpsHttpHandler) as httpd:
        httpd.serve_forever()


def main():
    global gps_receiver
    
    gps_host = "localhost"
    gps_port = 5000
    http_port = 8080
    
    if len(sys.argv) >= 2:
        gps_host = sys.argv[1]
    if len(sys.argv) >= 3:
        gps_port = int(sys.argv[2])
    if len(sys.argv) >= 4:
        http_port = int(sys.argv[3])
    
    print("GPS Position Plotter")
    print("====================")
    print(f"GPS server: {gps_host}:{gps_port}")
    print(f"Web server: http://localhost:{http_port}")
    if MAPBOX_TOKEN:
        print("Map tiles:  Mapbox Satellite")
    else:
        print("Map tiles:  ESRI Satellite (set MAPBOX_TOKEN env var for Mapbox)")
    print()
    
    # Start GPS receiver
    gps_receiver = GpsReceiver(gps_host, gps_port)
    gps_receiver.start()
    
    # Start HTTP server
    http_thread = threading.Thread(
        target=run_http_server,
        args=(http_port,),
        daemon=True
    )
    http_thread.start()
    print(f"HTTP server started on port {http_port}")
    
    # Open browser
    time.sleep(0.5)
    url = f"http://localhost:{http_port}"
    print(f"Opening browser: {url}")
    webbrowser.open(url)
    
    print()
    print("Press Ctrl+C to stop")
    print()
    
    try:
        while True:
            current = gps_receiver.get_current()
            if current:
                fix_names = {0: 'No Fix', 1: 'GPS', 2: 'DGPS', 4: 'RTK Fixed', 5: 'RTK Float'}
                print(f"\rLat: {current.latitude:11.6f}  "
                      f"Lon: {current.longitude:12.6f}  "
                      f"Alt: {current.altitude:7.1f}m  "
                      f"Sats: {current.num_satellites:2d}  "
                      f"Fix: {fix_names.get(current.fix_quality, '?'):10s}  "
                      f"Pts: {len(gps_receiver.positions)}", end='', flush=True)
            time.sleep(0.5)
    
    except KeyboardInterrupt:
        print("\n\nStopping...")
    
    finally:
        gps_receiver.stop()
    
    print("Done")


if __name__ == "__main__":
    main()
