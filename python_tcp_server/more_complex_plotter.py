import socket
import matplotlib.pyplot as plt
import numpy as np
from collections import deque

# ─── Configuration ────────────────────────────────────────────────────────────
HOST        = '172.20.10.3'
PORT        = 3333
BUFFER_SIZE = 65536
DATA_FILE   = "received_data.bin"

# SPI frame layout
MAGIC_BYTE_0     = 0x0F
MAGIC_BYTE_1     = 0x0F
NUM_CHANNELS     = 64
BYTES_PER_SAMPLE = 2
MAGIC_LEN        = 2
SPI_FRAME_SIZE   = MAGIC_LEN + NUM_CHANNELS * BYTES_PER_SAMPLE  # 130 bytes

# Timing
SAMPLE_PERIOD_MS = 50.0

# Plot config
HISTORY_SAMPLES  = 200
PLOT_COLS        = 8
PLOT_ROWS        = NUM_CHANNELS // PLOT_COLS
REDRAW_EVERY     = 10
# ──────────────────────────────────────────────────────────────────────────────


class FrameParser:
    """
    Accumulates raw TCP bytes and yields complete SPI frames.
    Handles TCP fragmentation.
    """
    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf.extend(data)

    def frames(self):
        buf = self._buf
        i = 0
        while i + SPI_FRAME_SIZE <= len(buf):
            frame = buf[i:i + SPI_FRAME_SIZE]
            magic_ok = (frame[0] == MAGIC_BYTE_0 and frame[1] == MAGIC_BYTE_1)
            channels = np.frombuffer(frame, dtype='<u2', offset=MAGIC_LEN)
            yield magic_ok, channels
            i += SPI_FRAME_SIZE
        self._buf = buf[i:]


def build_figure():
    """
    Create an 8x8 grid of subplots, one per channel.
    Made taller so all rows remain visible.
    """
    plt.style.use("dark_background")

    # Taller figure fixes bottom rows being cut off
    fig, axs = plt.subplots(
        PLOT_ROWS,
        PLOT_COLS,
        figsize=(20, 8),
        constrained_layout=True
    )

    fig.patch.set_facecolor('#0d0d0d')
    fig.suptitle('64-Channel RMS — Realtime', fontsize=16, fontweight='bold')

    # Try to maximize the window if the backend supports it
    try:
        manager = plt.get_current_fig_manager()
        if hasattr(manager, "window"):
            try:
                manager.window.showMaximized()
            except Exception:
                pass
    except Exception:
        pass

    axes = axs.flatten()
    lines = []

    x_axis = np.arange(HISTORY_SAMPLES) * SAMPLE_PERIOD_MS
    empty_y = np.zeros(HISTORY_SAMPLES, dtype=np.float32)

    for ch, ax in enumerate(axes):
        ax.set_facecolor('#1a1a1a')
        ax.set_title(f'ch{ch}', color='#bbbbbb', fontsize=7, pad=2)

        for spine in ax.spines.values():
            spine.set_edgecolor('#333333')

        ax.tick_params(colors='#888888', labelsize=5, length=2)
        ax.set_xlim(0, HISTORY_SAMPLES * SAMPLE_PERIOD_MS)
        ax.set_ylim(0, 65535)

        # Keep ticks minimal so the layout stays clean
        ax.set_xticks([])
        ax.set_yticks([0, 32767, 65535])
        ax.set_yticklabels(['0', '32k', '65k'], fontsize=4)

        color = plt.cm.plasma(ch / NUM_CHANNELS)
        line, = ax.plot(x_axis, empty_y, color=color, linewidth=0.7, antialiased=True)
        lines.append(line)

    return fig, axes, lines, x_axis


def main():
    history = [deque([0] * HISTORY_SAMPLES, maxlen=HISTORY_SAMPLES)
               for _ in range(NUM_CHANNELS)]

    total_frames = 0
    correct_frames = 0

    plt.ion()
    fig, axes, lines, x_axis = build_figure()
    plt.show(block=False)
    fig.canvas.draw()
    fig.canvas.flush_events()

    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((HOST, PORT))
    server_socket.listen(1)
    print(f"Listening on {HOST}:{PORT}  |  frame={SPI_FRAME_SIZE}B  channels={NUM_CHANNELS}")

    client_socket, client_address = server_socket.accept()
    print(f"Connected: {client_address}")

    parser = FrameParser()
    frames_since_redraw = 0

    with open(DATA_FILE, 'wb') as f:
        try:
            while True:
                raw = client_socket.recv(BUFFER_SIZE)
                if not raw:
                    print("Client disconnected.")
                    break

                f.write(raw)
                f.flush()
                parser.feed(raw)

                for magic_ok, channels in parser.frames():
                    total_frames += 1
                    if magic_ok:
                        correct_frames += 1

                    for ch in range(NUM_CHANNELS):
                        history[ch].append(float(channels[ch]))

                    frames_since_redraw += 1

                if total_frames > 0 and frames_since_redraw >= REDRAW_EVERY:
                    accuracy = 100.0 * correct_frames / total_frames
                    print(
                        f"Frames: {total_frames:6d}  |  "
                        f"Good: {correct_frames:6d}  |  "
                        f"Bad: {total_frames - correct_frames:4d}  |  "
                        f"Accuracy: {accuracy:.2f}%"
                    )

                    for ch in range(NUM_CHANNELS):
                        lines[ch].set_ydata(history[ch])

                    fig.canvas.draw_idle()
                    fig.canvas.flush_events()
                    plt.pause(0.001)
                    frames_since_redraw = 0

        except KeyboardInterrupt:
            print("\nShutting down.")
        finally:
            client_socket.close()
            server_socket.close()
            print(
                f"\nFinal accuracy: {correct_frames}/{total_frames} "
                f"({100.0 * correct_frames / max(1, total_frames):.2f}%)"
            )


if __name__ == '__main__':
    main()
