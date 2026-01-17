import socket
import struct
import threading
import time

def handle_client(client_socket, client_address, delay=0):
    print(f"Accepted connection from {client_address}")
    if delay > 0:
        time.sleep(delay)
    
    # Receive handshake
    try:
        data = client_socket.recv(68)
        if len(data) != 68:
            print(f"Invalid handshake length: {len(data)}")
            client_socket.close()
            return
        
        info_hash = data[28:48]
        print(f"Received handshake for info_hash: {info_hash.hex()}")
        
        # Send handshake response (mirror info_hash)
        # Protocol: 19 + "BitTorrent protocol" + 8 reserved + info_hash + peer_id
        response = bytearray()
        response.append(19)
        response.extend(b'BitTorrent protocol')
        response.extend(b'\x00' * 8)
        response.extend(info_hash)
        response.extend(b'-DP0001-123456789012') # Dummy peer ID
        
        client_socket.sendall(response)
        print(f"Sent handshake to {client_address}")
        
    except Exception as e:
        print(f"Error handling client: {e}")
    finally:
        client_socket.close()

def start_server(port, delay=0):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', port))
    server.listen(5)
    print(f"Listening on port {port} (delay={delay}s)")
    
    while True:
        client, addr = server.accept()
        threading.Thread(target=handle_client, args=(client, addr, delay)).start()

if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    delay = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    start_server(port, delay)
