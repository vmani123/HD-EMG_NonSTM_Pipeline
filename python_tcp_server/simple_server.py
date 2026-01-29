import socket
import matplotlib.pyplot as plt
import numpy as np
import time

# Configuration
HOST = '172.20.10.3'
PORT = 3333
BUFFER_SIZE = 512
DATA_FILE = 'received_data.bin'

WINDOW_MS = 5000      # show last 5 seconds on the plot
Y_MIN, Y_MAX = 0, 256

def main():
    plt.ion()
    fig, ax = plt.subplots()

    # Rolling buffers
    xs = np.array([], dtype=np.float64)   # timestamps in ms
    ys = np.array([], dtype=np.uint8)     # received bytes

    line, = ax.plot(xs, ys, linestyle='-', marker='')  # no markers for speed
    ax.set_ylim([Y_MIN, Y_MAX])
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Data (byte)')
    ax.set_title('Realtime Data Plot')

    # Create a TCP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((HOST, PORT))
    server_socket.listen(1)
    print(f"Server listening on {HOST}:{PORT}")

    client_socket, client_address = server_socket.accept()
    print(f"Client connected from {client_address}")

    start = time.perf_counter()
    message_counter = 0

    with open(DATA_FILE, 'ab') as f:
        try:
            while True:
                new_data = client_socket.recv(BUFFER_SIZE)

                if not new_data:
                    print("Client disconnected")
                    break

                now_ms = (time.perf_counter() - start) * 1000.0

                new_arr = np.frombuffer(new_data, dtype=np.uint8)

                # Timestamp all samples in this chunk with the receive time.
                # (If you know sample rate, you can spread them out—see note below.)
                new_x = np.full(new_arr.shape, now_ms, dtype=np.float64)

                xs = np.concatenate([xs, new_x])
                ys = np.concatenate([ys, new_arr])

                # Keep only last WINDOW_MS
                cutoff = now_ms - WINDOW_MS
                keep = xs >= cutoff
                xs = xs[keep]
                ys = ys[keep]

                # Update plot limits + data
                ax.set_xlim([max(0.0, now_ms - WINDOW_MS), max(WINDOW_MS, now_ms)])
                line.set_data(xs, ys)

                fig.canvas.draw_idle()
                fig.canvas.flush_events()

                message_counter += 1
                f.write(new_data)
                f.flush()
                print(f"Message #{message_counter}: Received and wrote {len(new_data)} bytes @ {now_ms:.1f} ms")

        except KeyboardInterrupt:
            print("\nServer shutting down...")
        finally:
            client_socket.close()
            server_socket.close()
            print("Server closed")

if __name__ == '__main__':
    main()
