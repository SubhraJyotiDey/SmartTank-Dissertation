#!/usr/bin/env python3
"""
Robust Over-The-Air (OTA) Flash Tool for ESP32 Smart Tank System
Fixes Windows WinError 10040 buffer truncation issue in legacy espota.py.
"""

import sys
import os
import socket
import hashlib
import time

FLASH_CMD = 0
SPIFFS_CMD = 100
AUTH_CMD  = 200

def flash_esp32_ota(esp_ip, bin_path, password="admin123", host_ip=None, esp_port=3232):
    if not os.path.exists(bin_path):
        print(f"[ERROR] Firmware binary not found: {bin_path}")
        return False

    with open(bin_path, "rb") as f:
        content = f.read()
    content_size = len(content)
    file_md5 = hashlib.md5(content).hexdigest()

    print("=" * 70)
    print("ESP32 OVER-THE-AIR (OTA) WIRELESS FLASHER")
    print("=" * 70)
    print(f"Target ESP32 IP : {esp_ip}:{esp_port}")
    print(f"Firmware Binary : {bin_path}")
    print(f"Binary Size     : {content_size} bytes ({content_size / (1024*1024):.2f} MB)")
    print(f"Firmware MD5    : {file_md5}")

    # Determine local host IP for reverse TCP stream
    if not host_ip:
        # Auto-detect IP route to ESP32
        temp_s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            temp_s.connect((esp_ip, esp_port))
            host_ip = temp_s.getsockname()[0]
        except Exception:
            host_ip = "0.0.0.0"
        finally:
            temp_s.close()

    print(f"Host Interface  : {host_ip}")

    # Create TCP server to receive connection from ESP32
    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind((host_ip, 0)) # bind to random available port
    local_port = tcp_server.getsockname()[1]
    tcp_server.listen(1)
    tcp_server.settimeout(15.0)

    print(f"Reverse TCP Port: {local_port} (Listening...)")
    print(f"Contacting ESP32 on UDP {esp_ip}:{esp_port}...")

    # Step 1: Send invitation via UDP
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.settimeout(3.0)

    # Format: command, localPort, content_size, file_md5
    invite_msg = f"{FLASH_CMD} {local_port} {content_size} {file_md5}\n"

    auth_nonce = None
    for attempt in range(1, 11):
        try:
            udp_sock.sendto(invite_msg.encode(), (esp_ip, esp_port))
            data, addr = udp_sock.recvfrom(2048) # 2048 buffer safely prevents WinError 10040!
            resp = data.decode(errors="ignore").strip()
            print(f"[RECV] Invitation reply from {addr}: {resp}")

            if resp.startswith("AUTH"):
                parts = resp.split()
                if len(parts) >= 2:
                    auth_nonce = parts[1]
                    break
            elif resp == "OK":
                break
        except socket.timeout:
            print(f"  ... waiting for ESP invitation ack (attempt {attempt}/10)")
            time.sleep(0.5)

    if not resp:
        print("[ERROR] No response from ESP32. Is it online and running ArduinoOTA?")
        tcp_server.close()
        udp_sock.close()
        return False

    # Step 2: Handle Authentication if requested
    if auth_nonce:
        print(f"[AUTH] Challenge nonce received: {auth_nonce}")
        # MD5(filename + size + md5 + remoteAddr)
        cnonce_seed = f"{bin_path}{content_size}{file_md5}{esp_ip}"
        cnonce = hashlib.md5(cnonce_seed.encode()).hexdigest()
        pass_md5 = hashlib.md5(password.encode()).hexdigest()
        challenge_token = f"{pass_md5}:{auth_nonce}:{cnonce}"
        auth_response = hashlib.md5(challenge_token.encode()).hexdigest()

        auth_msg = f"{AUTH_CMD} {cnonce} {auth_response}\n"
        print("[AUTH] Transmitting cryptographic authentication response...")
        udp_sock.sendto(auth_msg.encode(), (esp_ip, esp_port))

        try:
            auth_ack_data, _ = udp_sock.recvfrom(2048)
            auth_ack = auth_ack_data.decode(errors="ignore").strip()
            print(f"[AUTH] Result: {auth_ack}")
            if auth_ack != "OK":
                print(f"[ERROR] Authentication rejected by ESP32: {auth_ack}")
                tcp_server.close()
                udp_sock.close()
                return False
        except socket.timeout:
            print("[ERROR] Timeout waiting for authentication acknowledgement.")
            tcp_server.close()
            udp_sock.close()
            return False

    udp_sock.close()

    # Step 3: Accept reverse TCP connection from ESP32
    print("[TCP] Waiting for ESP32 to establish reverse stream connection...")
    try:
        conn, client_addr = tcp_server.accept()
        print(f"[TCP] Connected! ESP32 client stream connected from {client_addr}")
    except socket.timeout:
        print("[ERROR] ESP32 failed to connect back via TCP within 15 seconds.")
        print("Check if Windows Firewall is blocking incoming connections on python.exe.")
        tcp_server.close()
        return False

    # Step 4: Stream binary chunks to ESP32
    print("[STREAM] Beginning firmware stream upload...")
    chunk_size = 1460 # Optimal TCP MSS chunk
    total_sent = 0
    start_time = time.time()
    conn.settimeout(10.0)

    try:
        while total_sent < content_size:
            chunk = content[total_sent : total_sent + chunk_size]
            conn.sendall(chunk)
            total_sent += len(chunk)
            
            # Progress bar
            pct = (total_sent / content_size) * 100.0
            elapsed = time.time() - start_time
            speed_kb = (total_sent / 1024.0) / (elapsed + 0.001)
            bar_len = 35
            filled = int(round(bar_len * (total_sent / content_size)))
            bar = "#" * filled + "-" * (bar_len - filled)
            sys.stdout.write(f"\rProgress: [{bar}] {pct:5.1f}% ({total_sent}/{content_size} bytes, {speed_kb:5.1f} KB/s)")
            sys.stdout.flush()

        print("\n[STREAM] Binary transfer finished! Waiting for ESP32 flash write confirmation...")
        
        # Step 5: Await final OK confirmation
        conn.settimeout(30.0)
        final_resp = conn.recv(1024).decode(errors="ignore").strip()
        print(f"[RESULT] ESP32 Final Status: {final_resp}")

        if "OK" in final_resp or len(final_resp) == 0:
            print("=" * 70)
            print("OTA FLASH SUCCESSFUL! ESP32 is rebooting with updated firmware.")
            print("=" * 70)
            return True
        else:
            print(f"[WARN] Unexpected result from ESP32: {final_resp}")
            return False

    except Exception as e:
        print(f"\n[ERROR] Streaming failed: {e}")
        return False
    finally:
        conn.close()
        tcp_server.close()

if __name__ == "__main__":
    esp_ip = "192.168.29.125"
    bin_path = r"A:\Dissertation\code\firmware\esp32_pressure_tap_node\pressure_node_firmware.bin"
    success = flash_esp32_ota(esp_ip, bin_path, password="admin123", host_ip="192.168.29.114")
    sys.exit(0 if success else 1)
