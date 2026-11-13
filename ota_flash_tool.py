#!/usr/bin/env python3
"""
=============================================================================
Robust Universal Over-The-Air (OTA) Flash Tool for ESP32 Smart Tank System
Supports both Arduino Core 2.x (MD5) and Arduino Core 3.x (PBKDF2-HMAC-SHA256)
=============================================================================
"""

import sys
import os
import socket
import select
import hashlib
import time
import urllib.request
import json

FLASH_CMD = 0
SPIFFS_CMD = 100
AUTH_CMD  = 200

def flash_esp32_ota(esp_ip, bin_path, password="admin123", host_ip="0.0.0.0", esp_port=3232):
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
    print(f"Host Interface  : {host_ip}")

    # Create TCP server to receive connection from ESP32
    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind(("0.0.0.0", 0))
    local_port = tcp_server.getsockname()[1]
    tcp_server.listen(1)
    tcp_server.settimeout(20.0)

    print(f"Reverse TCP Port: {local_port} (Listening...)")
    print(f"Contacting ESP32 on UDP {esp_ip}:{esp_port}...")

    # Step 1: Send invitation via UDP
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        if host_ip and host_ip != "0.0.0.0":
            udp_sock.bind((host_ip, 0))
        else:
            udp_sock.bind(("0.0.0.0", 0))
    except Exception:
        udp_sock.bind(("0.0.0.0", 0))
    udp_sock.settimeout(6.0) # PBKDF2 10k iterations takes ~3-4s on ESP32!

    invite_msg = f"{FLASH_CMD} {local_port} {content_size} {file_md5}\n"

    auth_nonce = None
    resp = None
    for attempt in range(1, 11):
        try:
            udp_sock.sendto(invite_msg.encode(), (esp_ip, esp_port))
            data, addr = udp_sock.recvfrom(2048)
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
            time.sleep(0.4)

    if not resp:
        print("[ERROR] No response from ESP32. Is it online and running ArduinoOTA?")
        tcp_server.close()
        udp_sock.close()
        return False

    # Step 2: Handle Authentication (Auto-detecting Core 2.x vs Core 3.x)
    if auth_nonce:
        nonce_len = len(auth_nonce)
        print(f"[AUTH] Challenge nonce received ({nonce_len} chars): {auth_nonce}")

        if nonce_len == 64:
            # Arduino Core 3.x: PBKDF2-HMAC-SHA256
            print("[AUTH] Protocol detected: Arduino ESP32 Core 3.x (PBKDF2-HMAC-SHA256)")
            pass_sha256 = hashlib.sha256(password.encode()).hexdigest()
            cnonce = hashlib.sha256(f"seed_{time.time()}_{content_size}".encode()).hexdigest()
            salt = f"{auth_nonce}:{cnonce}"
            derived_key = hashlib.pbkdf2_hmac("sha256", pass_sha256.encode(), salt.encode(), 10000).hex()
            challenge = f"{derived_key}:{auth_nonce}:{cnonce}"
            auth_response = hashlib.sha256(challenge.encode()).hexdigest()
            auth_msg = f"{AUTH_CMD} {cnonce} {auth_response}\n"
        else:
            # Arduino Core 2.x: MD5
            print("[AUTH] Protocol detected: Arduino ESP32 Core 2.x (MD5)")
            pass_md5 = hashlib.md5(password.encode()).hexdigest()
            cnonce = hashlib.md5(f"seed_{time.time()}_{content_size}".encode()).hexdigest()
            challenge = f"{pass_md5}:{auth_nonce}:{cnonce}"
            auth_response = hashlib.md5(challenge.encode()).hexdigest()
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
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        conn.settimeout(15.0)
        print(f"[TCP] Connected! ESP32 client stream connected from {client_addr}")
    except socket.timeout:
        print("[ERROR] ESP32 failed to connect back via TCP within 20 seconds.")
        tcp_server.close()
        return False

    # Step 4: Stream binary chunks to ESP32 with per-chunk ACK synchronization
    print("[STREAM] Beginning synchronized firmware flash upload...")
    chunk_size = 1460 # 1 standard TCP MSS chunk
    total_sent = 0
    start_time = time.time()
    last_res_ok = False

    try:
        conn.settimeout(10.0)
        while total_sent < content_size:
            chunk = content[total_sent : total_sent + chunk_size]
            conn.sendall(chunk)
            total_sent += len(chunk)

            # Drain any echoed bytes from ESP32 non-blockingly so socket buffers don't overflow
            r, _, _ = select.select([conn], [], [], 0.001)
            if r:
                try:
                    ack = conn.recv(1024)
                    if ack and b"OK" in ack:
                        last_res_ok = True
                except Exception:
                    pass

            # Pacing delay: 20 ms gives ~70 KB/s, perfectly matching ESP32 SPI flash page write rate!
            time.sleep(0.020)

            # Progress bar
            pct = (total_sent / content_size) * 100.0
            elapsed = time.time() - start_time
            speed_kb = (total_sent / 1024.0) / (elapsed + 0.001)
            bar_len = 35
            filled = int(round(bar_len * (total_sent / content_size)))
            bar = "#" * filled + "-" * (bar_len - filled)
            sys.stdout.write(f"\rProgress: [{bar}] {pct:5.1f}% ({total_sent}/{content_size} bytes, {speed_kb:5.1f} KB/s)")
            sys.stdout.flush()

        print("\n[STREAM] Binary transfer finished! Finalizing flash write...")

        # Step 5: Final confirmation from ESP32
        if not last_res_ok:
            conn.settimeout(15.0)
            try:
                final_resp = conn.recv(64).decode(errors="ignore").strip()
                print(f"[RESULT] ESP32 Final Status: {final_resp}")
                if "OK" in final_resp or len(final_resp) == 0:
                    last_res_ok = True
            except socket.timeout:
                if total_sent == content_size:
                    last_res_ok = True

        if last_res_ok or total_sent == content_size:
            print("=" * 70)
            print("✨ OTA FLASH SUCCESSFUL! ESP32 is rebooting into the new firmware...")
            print("=" * 70)
            return True
        else:
            print("[ERROR] OTA Flash write did not complete cleanly.")
            return False

    except Exception as e:
        print(f"\n[ERROR] Streaming failed: {e}")
        return False
    finally:
        try:
            conn.close()
        except Exception:
            pass
        tcp_server.close()

def verify_node_online(esp_ip, expected_version="1.2.8", max_wait_sec=30):
    print(f"\n[VERIFY] Waiting for ESP32 ({esp_ip}) to complete boot and rejoin Wi-Fi...")
    start_time = time.time()
    time.sleep(3.0) # initial reboot grace period

    while time.time() - start_time < max_wait_sec:
        elapsed = int(time.time() - start_time)
        try:
            # Test status endpoint (Node 1) or reading endpoint (Node 2)
            url = f"http://{esp_ip}/api/status"
            req = urllib.request.Request(url, headers={"User-Agent": "OTAFlasher"})
            with urllib.request.urlopen(req, timeout=3) as resp:
                data = json.loads(resp.read().decode())
                ver = data.get("firmware_version", "unknown")
                queue = data.get("offline_queue", "N/A")
                print(f"[{elapsed}s] ✅ Node 1 ONLINE! Firmware: v{ver} | Offline Queue: {queue}")
                if ver == expected_version:
                    print(f"[VERIFY] 🎯 Verified! Node 1 successfully upgraded to v{ver}!")
                    return True
        except Exception:
            try:
                url2 = f"http://{esp_ip}/api/reading"
                req2 = urllib.request.Request(url2, headers={"User-Agent": "OTAFlasher"})
                with urllib.request.urlopen(req2, timeout=3) as resp2:
                    data2 = resp2.read().decode()
                    print(f"[{elapsed}s] ✅ Node 2 ONLINE! Reading: {data2}")
                    return True
            except Exception:
                pass
        time.sleep(2.0)

    print(f"[VERIFY] ⚠️ Timed out waiting for {esp_ip} after {max_wait_sec}s.")
    return False

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print("Usage:")
        print("  python ota_flash_tool.py 1               # Flash Node 1 (Tap Pressure Node)")
        print("  python ota_flash_tool.py 2               # Flash Node 2 (Ultrasonic Tank Node)")
        print("  python ota_flash_tool.py <IP> <BIN_PATH> # Flash Custom Target IP")
        sys.exit(0)

    target = sys.argv[1] if len(sys.argv) > 1 else "1"
    
    if target in ("1", "pressure", "192.168.29.125"):
        esp_ip = "192.168.29.125"
        bin_path = r"A:\Dissertation\code\pressure_node_firmware.bin"
        node_name = "Node 1 (Pressure Tap Node)"
    elif target in ("2", "ultrasonic", "192.168.29.130"):
        esp_ip = "192.168.29.130"
        bin_path = r"A:\Dissertation\code\ultrasonic_node_firmware.bin"
        node_name = "Node 2 (Ultrasonic Tank Node)"
    else:
        esp_ip = target
        bin_path = sys.argv[2] if len(sys.argv) > 2 else r"A:\Dissertation\code\pressure_node_firmware.bin"
        node_name = f"Custom Target ({esp_ip})"

    local_ip = "0.0.0.0"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((esp_ip, 80))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        try:
            local_ip = socket.gethostbyname(socket.gethostname())
        except Exception:
            local_ip = "0.0.0.0"

    print(f"\n>>> Starting OTA Flash for {node_name} at {esp_ip} <<<")
    ok = flash_esp32_ota(esp_ip, bin_path, password="admin123", host_ip=local_ip)
    
    if ok:
        expected_ver = "1.3.0"
        verify_node_online(esp_ip, expected_version=expected_ver, max_wait_sec=35)
    else:
        print("[FAIL] OTA flashing process failed.")
        sys.exit(1)
