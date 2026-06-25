import os
import re
import ssl
import json
import socket
from functools import partial
from http.server import HTTPServer, SimpleHTTPRequestHandler

PORT = 8000
BUILD_DIR = ".pio/build"

# --------------------------------------------------
# Find firmware.bin
# --------------------------------------------------

firmware = None

for root, dirs, files in os.walk(BUILD_DIR):
    if "firmware.bin" in files:
        firmware = os.path.join(root, "firmware.bin")
        break

if firmware is None:
    print(" firmware.bin not found!")
    exit()

folder = os.path.dirname(firmware)
filename = os.path.basename(firmware)

# --------------------------------------------------
# Project Root
# --------------------------------------------------

project_root = os.path.dirname(os.path.abspath(__file__))

cert = os.path.join(project_root, "cert.pem")
key = os.path.join(project_root, "key.pem")

# --------------------------------------------------
# Read PROJECT_VER from CMakeLists.txt
# --------------------------------------------------

cmake_file = os.path.join(project_root, "CMakeLists.txt")

version = "0.0.0"

with open(cmake_file, "r") as f:

    text = f.read()

match = re.search(r'set\s*\(\s*PROJECT_VER\s*"([^"]+)"\s*\)', text)

if match:
    version = match.group(1)

# --------------------------------------------------
# Detect PC IP
# --------------------------------------------------

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

try:
    sock.connect(("8.8.8.8", 80))
    ip = sock.getsockname()[0]
finally:
    sock.close()

# --------------------------------------------------
# Generate version.json automatically
# --------------------------------------------------

version_info = {
    "version": version,
    "url": f"https://{ip}:{PORT}/{filename}"
}

with open(os.path.join(folder, "version.json"), "w") as f:
    json.dump(version_info, f, indent=4)

# --------------------------------------------------
# HTTPS Server
# --------------------------------------------------

Handler = partial(SimpleHTTPRequestHandler, directory=folder)

server = HTTPServer(("0.0.0.0", PORT), Handler)

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile=cert, keyfile=key)

server.socket = context.wrap_socket(
    server.socket,
    server_side=True
)

# --------------------------------------------------
# Information
# --------------------------------------------------

print("=" * 60)
print("ESP32 HTTPS OTA SERVER")
print("=" * 60)

print(f"Firmware : {filename}")
print(f"Version  : {version}")

print()

print("Firmware URL")
print(f"https://{ip}:{PORT}/{filename}")

print()

print("Version URL")
print(f"https://{ip}:{PORT}/version.json")

print("=" * 60)

print("\nWaiting for ESP32...\n")

try:
    server.serve_forever()

except KeyboardInterrupt:

    print("\nStopping server...")

    server.server_close()
# import os
# import socket
# import ssl
# from functools import partial
# from http.server import HTTPServer, SimpleHTTPRequestHandler

# PORT = 8000
# BUILD_DIR = ".pio/build"

# # --------------------------------------------------
# # Find firmware.bin automatically
# # --------------------------------------------------

# firmware = None

# for root, dirs, files in os.walk(BUILD_DIR):
#     if "firmware.bin" in files:
#         firmware = os.path.join(root, "firmware.bin")
#         break

# if firmware is None:
#     print(" firmware.bin not found!")
#     print("Please build the project first.")
#     exit()

# folder = os.path.dirname(firmware)
# filename = os.path.basename(firmware)

# # --------------------------------------------------
# # Project root (where ota_server.py is located)
# # --------------------------------------------------

# project_root = os.path.dirname(os.path.abspath(__file__))

# cert = os.path.join(project_root, "cert.pem")
# key = os.path.join(project_root, "key.pem")

# if not os.path.exists(cert):
#     print(" cert.pem not found")
#     print(cert)
#     exit()

# if not os.path.exists(key):
#     print(" key.pem not found")
#     print(key)
#     exit()

# # --------------------------------------------------
# # Detect PC IP
# # --------------------------------------------------

# sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# try:
#     sock.connect(("8.8.8.8", 80))
#     ip = sock.getsockname()[0]
# finally:
#     sock.close()

# # --------------------------------------------------
# # HTTPS Server
# # --------------------------------------------------

# Handler = partial(SimpleHTTPRequestHandler, directory=folder)

# server = HTTPServer(("0.0.0.0", PORT), Handler)

# context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
# context.load_cert_chain(certfile=cert, keyfile=key)

# server.socket = context.wrap_socket(
#     server.socket,
#     server_side=True
# )

# # --------------------------------------------------
# # Information
# # --------------------------------------------------

# print("=" * 60)
# print("ESP32 HTTPS OTA SERVER")
# print("=" * 60)

# print("Firmware :", filename)
# print("Folder   :", folder)
# print()

# print("Certificate :", cert)
# print("Key         :", key)
# print()

# print("OTA URL:")
# print(f"https://{ip}:{PORT}/{filename}")

# print("=" * 60)

# print("\nWaiting for ESP32...\n")

# try:
#     server.serve_forever()

# except KeyboardInterrupt:

#     print("\nStopping server...")

#     server.server_close()