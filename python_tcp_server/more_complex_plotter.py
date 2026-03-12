import socket
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import numpy as np
import time
from collections import deque

# ─── Configuration ────────────────────────────────────────────────────────────
HOST        = '172.20.10.3'
PORT        = 3333
BUFFER_SIZE = 65536          # large recv buffer to absorb full TCP batches

# SPI frame layout
MAGIC_BYTE_0     = 0x0F
MAGIC_BYTE_1     = 0x0F
NUM_CHANNELS     = 64
BYTES_PER_SAMPLE = 2
MAGIC_LEN        = 2
SPI_FRAME_SIZE   = MAGIC_LEN + NUM_CHANNELS * BYTES_PER_SAMPLE  # 130 bytes

# Timing
SAMPLE_PERIOD_MS = 50.0      # one SPI frame every 50 ms

# Plot config
HISTORY_SAMPLES  = 200       # how many samples (frames) to keep per channel
PLOT_COLS        = 8         # subplot grid columns
PLOT_ROWS        = NUM_CHANNELS // PLOT_COLS   # 8 rows
REDRAW_EVERY     = 10        # redraw after this many new frames (throttle)
# ──────────────────────────────────────────────────────────────────────────────


class FrameParser:
    """
    Accumulates raw TCP bytes and yields complete 130-byte SPI frames.
    Handles fragmentation (TCP may deliver partial batches).
    """
    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf.extend(data)

    def frames(self):
        """Yield (magic_ok: bool, channels: np.ndarray[64] uint16) for every complete frame."""
        buf = self._buf
        i = 0
        while i + SPI_FRAME_SIZE <= len(buf):
            frame = buf[i : i + SPI_FRAME_SIZE]
            magic_ok = (frame[0] == MAGIC_BYTE_0 and frame[1] == MAGIC_BYTE_1)
            # Parse 64 big-endian uint16 channel values
            channels = np.frombuffer(frame, dtype='<u2', offset=MAGIC_LEN)  # shape (64,)
            yield magic_ok, channels
            i += SPI_FRAME_SIZE
        # Keep any incomplete trailing bytes
        self._buf = buf[i:]


def build_figure():
    """Create an 8×8 grid of subplots, one per channel."""
    fig = plt.figure(figsize=(20, 12), facecolor='#0d0d0d')
    fig.suptitle('64-Channel RMS — Realtime', color='#e0e0e0',
                 fontsize=13, fontweight='bold', y=0.995)

    gs = gridspec.GridSpec(PLOT_ROWS, PLOT_COLS, figure=fig,
                           hspace=0.55, wspace=0.35,
                           left=0.04, right=0.98, top=0.97, bottom=0.04)

    axes  = []
    lines = []
    empty_x = np.arange(HISTORY_SAMPLES) * SAMPLE_PERIOD_MS
    empty_y = np.zeros(HISTORY_SAMPLES, dtype=np.float32)

    for ch in range(NUM_CHANNELS):
        row, col = divmod(ch, PLOT_COLS)
        ax = fig.add_subplot(gs[row, col])
        ax.set_facecolor('#1a1a1a')
        ax.tick_params(colors='#666', labelsize=5, length=2)
        for spine in ax.spines.values():
            spine.set_edgecolor('#333')
        ax.set_ylim(0, 65535)
        ax.set_xlim(0, HISTORY_SAMPLES * SAMPLE_PERIOD_MS)
        ax.set_title(f'ch{ch}', color='#999', fontsize=5.5, pad=2)
        ax.set_xticks([])
        ax.set_yticks([0, 32767, 65535])
        ax.set_yticklabels(['0', '32k', '65k'], fontsize=4)

        color = plt.cm.plasma(ch / NUM_CHANNELS)
        (line,) = ax.plot(empty_x, empty_y, color=color, linewidth=0.7, antialiased=True)
        axes.append(ax)
        lines.append(line)

    return fig, axes, lines


def main():
    # ── Rolling buffers: one deque per channel ──────────────────────────────
    history = [deque(maxlen=HISTORY_SAMPLES) for _ in range(NUM_CHANNELS)]
    for ch in range(NUM_CHANNELS):
        history[ch].extend([0] * HISTORY_SAMPLES)   # pre-fill with zeros

    x_axis = np.arange(HISTORY_SAMPLES) * SAMPLE_PERIOD_MS   # fixed x (relative sample index)

    # ── Accuracy counters ────────────────────────────────────────────────────
    total_frames   = 0
    correct_frames = 0

    # ── Matplotlib setup ─────────────────────────────────────────────────────
    plt.ion()
    fig, axes, lines = build_figure()
    fig.canvas.draw()
    fig.canvas.flush_events()

    # ── TCP server ───────────────────────────────────────────────────────────
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((HOST, PORT))
    server_socket.listen(1)
    print(f"Listening on {HOST}:{PORT}  |  frame={SPI_FRAME_SIZE}B  channels={NUM_CHANNELS}")

    client_socket, client_address = server_socket.accept()
    print(f"Connected: {client_address}")

    parser       = FrameParser()
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
                    else:
                        # Still plot the data even if magic bytes are wrong;
                        # accuracy tracking will flag it.
                        pass

                    # Push each channel's new sample into its rolling buffer
                    for ch in range(NUM_CHANNELS):
                        history[ch].append(float(channels[ch]))

                    frames_since_redraw += 1

                # ── Accuracy report (every frame parsed above) ───────────────
                if total_frames > 0 and frames_since_redraw >= REDRAW_EVERY:
                    accuracy = 100.0 * correct_frames / total_frames
                    print(f"Frames: {total_frames:6d}  |  "
                        f"Good: {correct_frames:6d}  |  "
                        f"Bad: {total_frames - correct_frames:4d}  |  "
                        f"Accuracy: {accuracy:.2f}%")

                    # ── Redraw all 64 subplots ────────────────────────────────
                    for ch in range(NUM_CHANNELS):
                        lines[ch].set_ydata(list(history[ch]))
                    fig.canvas.draw_idle()
                    fig.canvas.flush_events()
                    frames_since_redraw = 0

        except KeyboardInterrupt:
            print("\nShutting down.")
        finally:
            client_socket.close()
            server_socket.close()
            print(f"\nFinal accuracy: {correct_frames}/{total_frames} "
                f"({100.0 * correct_frames / max(1, total_frames):.2f}%)")


if __name__ == '__main__':
    main()
