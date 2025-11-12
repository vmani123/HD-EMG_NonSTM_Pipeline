import struct
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque
from datetime import datetime, timedelta

# Configuration
DATA_FILE = 'received_data.bin'
WINDOW_SECONDS = 10  # Show last 10 seconds of data
REFRESH_RATE = 1000  # Refresh every 1000ms (1 second)
NUMBER_FORMAT = 'f'  # 'f' for float, 'd' for double, 'i' for int, etc.

class RealtimePlotter:
    def __init__(self, window_seconds, refresh_rate):
        self.window_seconds = window_seconds
        self.refresh_rate = refresh_rate
        self.data_points = deque()  # Stores (timestamp, value) tuples
        self.last_position = 0
        
        # Set up the plot
        self.fig, self.ax = plt.subplots(figsize=(10, 6))
        self.line, = self.ax.plot([], [], 'b-', linewidth=2)
        self.ax.set_xlabel('Time (seconds ago)')
        self.ax.set_ylabel('Value')
        self.ax.set_title('Real-time Data Plot')
        self.ax.grid(True, alpha=0.3)
        
    def read_new_data(self):
        """Read new data from file since last read"""
        try:
            with open(DATA_FILE, 'rb') as f:
                f.seek(self.last_position)
                new_data = f.read()
                self.last_position = f.tell()
                
            # Parse binary data as numbers
            bytes_per_number = struct.calcsize(NUMBER_FORMAT)
            current_time = datetime.now()
            
            for i in range(0, len(new_data), bytes_per_number):
                if i + bytes_per_number <= len(new_data):
                    value = struct.unpack(NUMBER_FORMAT, new_data[i:i+bytes_per_number])[0]
                    self.data_points.append((current_time, value))
                    
        except FileNotFoundError:
            pass
        except Exception as e:
            print(f"Error reading data: {e}")
    
    def update_plot(self, frame):
        """Update plot with new data"""
        # Read any new data
        self.read_new_data()
        
        # Remove data older than window_seconds
        cutoff_time = datetime.now() - timedelta(seconds=self.window_seconds)
        while self.data_points and self.data_points[0][0] < cutoff_time:
            self.data_points.popleft()
        
        # Prepare data for plotting
        if self.data_points:
            current_time = datetime.now()
            times = [-(current_time - ts).total_seconds() for ts, _ in self.data_points]
            values = [val for _, val in self.data_points]
            
            # Update line data
            self.line.set_data(times, values)
            
            # Auto-scale axes
            self.ax.set_xlim(-self.window_seconds, 0)
            if values:
                y_min, y_max = min(values), max(values)
                margin = (y_max - y_min) * 0.1 if y_max != y_min else 1
                self.ax.set_ylim(y_min - margin, y_max + margin)
        
        return self.line,
    
    def start(self):
        """Start the animation"""
        ani = animation.FuncAnimation(
            self.fig, 
            self.update_plot, 
            interval=self.refresh_rate,
            blit=True,
            cache_frame_data=False
        )
        plt.show()

if __name__ == '__main__':
    plotter = RealtimePlotter(WINDOW_SECONDS, REFRESH_RATE)
    print(f"Starting real-time plotter...")
    print(f"Window: {WINDOW_SECONDS} seconds")
    print(f"Refresh rate: {REFRESH_RATE}ms")
    print(f"Number format: {NUMBER_FORMAT}")
    plotter.start()
