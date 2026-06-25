#!/usr/bin/env python3
"""
Extract the SSL certificate from the OTA server and save as cert.pem
"""

import ssl
import socket
import sys

HOST = "192.168.31.237"
PORT = 8000

print(f"Connecting to {HOST}:{PORT}...")

try:
    # Create SSL context that doesn't verify the cert
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    
    # Connect and get the certificate
    with socket.create_connection((HOST, PORT), timeout=5) as sock:
        with context.wrap_socket(sock, server_hostname=HOST) as ssock:
            cert_binary = ssock.getpeercert(binary_form=True)
    
    if cert_binary:
        # Convert binary cert to PEM format
        import base64
        
        cert_b64 = base64.b64encode(cert_binary).decode('utf-8')
        
        # Split into 64-char lines as per PEM standard
        cert_lines = [cert_b64[i:i+64] for i in range(0, len(cert_b64), 64)]
        cert_pem = "-----BEGIN CERTIFICATE-----\n"
        cert_pem += "\n".join(cert_lines)
        cert_pem += "\n-----END CERTIFICATE-----\n"
        
        # Save to file
        with open("cert.pem", "w") as f:
            f.write(cert_pem)
        
        print("✓ Certificate saved to cert.pem")
        print("\nCertificate Details:")
        print(f"  Subject: {ssock.getpeercert()['subject']}")
        print(f"  Issuer: {ssock.getpeercert()['issuer']}")
        
    else:
        print("✗ Failed to retrieve certificate")
        sys.exit(1)

except Exception as e:
    print(f"✗ Error: {e}")
    sys.exit(1)
