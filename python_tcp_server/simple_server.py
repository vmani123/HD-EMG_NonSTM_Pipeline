import socket
import matplotlib.pyplot as plt
import numpy as np


# Configuration
HOST = '172.20.10.3'  # Listen on localhost
PORT = 3333         # Port to listen on
BUFFER_SIZE = 500  # Size of receive buffer
DATA_FILE = 'received_data.bin'  # Output file
def main():

    plt.ion()
    fig = plt.figure()
    ax = fig.add_subplot(1, 1, 1)
    plt.ylim([0,256])
    plt.xlim([0,500])

    data = np.zeros(50, dtype=np.uint8)

    line, = ax.plot(data)    

    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Data')
    ax.set_title('Realtime Data Plot')
    ax.legend(['Data'],loc='upper right')


    # Create a TCP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    # Bind to address and port
    server_socket.bind((HOST, PORT))
    
    # Listen for incoming connections
    server_socket.listen(1)
    print(f"Server listening on {HOST}:{PORT}")
    
    # Accept a client connection
    client_socket, client_address = server_socket.accept()
    print(f"Client connected from {client_address}")
    
    # Open file in append binary mode
    message_counter = 0
    with open(DATA_FILE, 'ab') as f:
        try:
            while True:
                # Receive data from client
                new_data = client_socket.recv(BUFFER_SIZE)
                new_arr = np.frombuffer(new_data, dtype=np.uint8)
                # new_arr = np.fromstring(new_data, sep='', dtype=np.float32)

                data = np.append(data,new_arr)
                if len(data)>500:
                    data = data[-500:]

                line.set_ydata(data)
                line.set_xdata(np.arange(len(data)))
                plt.draw()
                plt.pause(0.1)                    

                # If no data, client has disconnected
                if not new_data:
                    print("Client disconnected")
                    break
                
                # Increment counter
                message_counter += 1
                
                # Write data to file
                f.write(new_data)
                f.flush()  # Ensure data is written immediately
                print(f"Message #{message_counter}: Received and wrote {len(new_data)} bytes")
                
        except KeyboardInterrupt:
            print("\nServer shutting down...")
        finally:
            client_socket.close()
    
    server_socket.close()
    print("Server closed")

if __name__ == '__main__':
    main()
