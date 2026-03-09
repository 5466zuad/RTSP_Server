import socket

# Send dummy UDP packet to unblock sender if needed
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(b'1', ('127.0.0.1', 9380))
sock.close()
