import socket
import json
import time

# --- Protocol constants (must match STM32 firmware) ---
DISCOVERY_PORT = 5005
BROADCAST_IP = "255.255.255.255"
DISCOVERY_MESSAGE = b"DISCOVER_NAMI_DEVICES"
DISCOVERY_TIMEOUT = 3.0

DEFAULT_TCP_PORT = 6000
TCP_TIMEOUT = 5.0
TARGET_TYPE = "test"       # matches g_device_info.type = "test" on STM32


def discover_first_test_device():
    """
    Sends a UDP broadcast to discover NaMi devices and returns
    the first device with type == 'test' (or None if nothing found).
    """
    print(">>> Searching for NaMi test devices on the network...")
    devices = []

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.settimeout(DISCOVERY_TIMEOUT)

            print(f"    Sending discovery message '{DISCOVERY_MESSAGE.decode()}' on UDP port {DISCOVERY_PORT}...")
            s.sendto(DISCOVERY_MESSAGE, (BROADCAST_IP, DISCOVERY_PORT))

            while True:
                try:
                    data, addr = s.recvfrom(1024)
                    # STM32 responds with JSON
                    info = json.loads(data.decode("utf-8"))

                    dev_type = info.get("type")
                    dev_id = info.get("device_id", "N/A")

                    print(f"    Received response from {addr[0]}: device_id='{dev_id}', type='{dev_type}'")

                    if dev_type == TARGET_TYPE:
                        info["ip_address"] = addr[0]
                        devices.append(info)
                except socket.timeout:
                    break
                except (json.JSONDecodeError, UnicodeDecodeError):
                    # Ignore garbage / non-JSON packets
                    continue

    except Exception as e:
        print(f"[ERROR] Discovery failed: {e}")
        return None

    if not devices:
        print(">>> No matching devices found.")
        return None

    # Take the first matching device
    dev = devices[0]
    print(f">>> Found test device: {dev.get('device_id', 'N/A')} at {dev['ip_address']}")
    return dev


def connect_and_init(ip, tcp_port=DEFAULT_TCP_PORT):
    """
    Opens a TCP connection to the device and performs the INIT handshake.
    Returns the session_id (or None on failure).
    """
    print(f"\n>>> Connecting to {ip}:{tcp_port}...")
    session_id = None

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        try:
            sock.settimeout(TCP_TIMEOUT)
            sock.connect((ip, tcp_port))
            print(">>> TCP connection established.")

            # Build and send INIT command (must match STM32 expectation)
            init_cmd = {
                "cmd": "INIT",
                "client": "Python demo",
                "purpose": "test_connection"
            }
            init_json = json.dumps(init_cmd)
            print(f">>> Sending INIT command: {init_json}")
            sock.sendall(init_json.encode("utf-8"))

            # Wait for response
            print(">>> Waiting for INIT response...")
            resp_bytes = sock.recv(1024)
            if not resp_bytes:
                print("[ERROR] No response received. Connection may have been closed.")
                return None

            resp = json.loads(resp_bytes.decode("utf-8"))
            print(f">>> Received response: {resp}")

            if resp.get("status") == "OK":
                session_id = resp.get("session_id")
                print(f">>> Session initialized successfully. Session ID: {session_id}")
            else:
                print(f"[ERROR] INIT failed. Device responded with: {resp}")

        except socket.timeout:
            print("[ERROR] Timeout while connecting or waiting for response.")
        except ConnectionRefusedError:
            print("[ERROR] Connection refused. Is the STM32 TCP server running?")
        except Exception as e:
            print(f"[ERROR] Unexpected error: {e}")

    print(">>> TCP connection closed.")
    return session_id


def main():
    # Step 1: discover STM32 device
    device = discover_first_test_device()
    if not device:
        return

    ip = device["ip_address"]
    tcp_port = device.get("tcp_port", DEFAULT_TCP_PORT)

    # Small delay just to make logs more readable
    time.sleep(0.5)

    # Step 2: connect and perform INIT
    session_id = connect_and_init(ip, tcp_port)

    # Step 3: simulate "ready for further commands"
    if session_id:
        print("\n>>> Device is ready for further commands (not implemented in this minimal demo).")
    else:
        print("\n>>> Session was not established. Nothing more to do.")


if __name__ == "__main__":
    main()
