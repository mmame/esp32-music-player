import math
import struct
import wave


def format_num(val):
    # Returns an integer string if whole, otherwise replaces decimals with 'p' (e.g., 440.5 -> 440p5)
    if val == int(val):
        return str(int(val))
    return str(val).replace(".", "p")


# File generation parameters
sample_rate = 44100  # 44.1 kHz
duration = 60.0  # Duration in seconds

# Prompt the user for the frequency for Channel 1
while True:
    try:
        user_input_1 = input(
            "Enter the frequency for Channel 1 in Hz (e.g., 1000 or 440): "
        )
        freq1 = float(user_input_1)
        if freq1 <= 0:
            print("Frequency must be greater than 0 Hz.")
            continue
        if freq1 > sample_rate / 2:
            print(
                f"Warning: Frequency exceeds the Nyquist limit ({sample_rate / 2} Hz) for this sample rate."
            )
            continue
        break
    except ValueError:
        print("Invalid input. Please enter a valid numerical value.")

# Prompt the user for the frequency for Channel 2
while True:
    try:
        user_input_2 = input(
            "Enter the frequency for Channel 2 in Hz (e.g., 1000 or 440): "
        )
        freq2 = float(user_input_2)
        if freq2 <= 0:
            print("Frequency must be greater than 0 Hz.")
            continue
        if freq2 > sample_rate / 2:
            print(
                f"Warning: Frequency exceeds the Nyquist limit ({sample_rate / 2} Hz) for this sample rate."
            )
            continue
        break
    except ValueError:
        print("Invalid input. Please enter a valid numerical value.")

# Dynamic filename generation based on both chosen frequencies[cite: 1]
filename = f"dual_sine_{format_num(freq1)}_ch1_{format_num(freq2)}_ch2.wav"

# 16-bit audio configuration: Maximum peak amplitude for signed 16-bit is 32767[cite: 1]
max_amplitude = 32767

num_samples = int(sample_rate * duration)

print(f"\nGenerating '{filename}'...")

# Open the WAV file for writing[cite: 1]
with wave.open(filename, "w") as wav_file:
    # Set parameters: (num_channels, sample_width_bytes, sample_rate, num_frames, compression_type, compression_name)[cite: 1]
    # 2 channels = Stereo, 2 bytes = 16-bit[cite: 1]
    wav_file.setparams(
        (2, 2, sample_rate, num_samples, "NONE", "not compressed")
    )

    # Generate and write frames block by block to save memory[cite: 1]
    frames = []
    for i in range(num_samples):
        t = float(i) / sample_rate

        # Calculate independent sine wave values for each channel
        sine_val_1 = math.sin(2.0 * math.pi * freq1 * t)
        sine_val_2 = math.sin(2.0 * math.pi * freq2 * t)

        # Scale to maximum 16-bit amplitude and convert to integer for each channel
        sample_1 = int(sine_val_1 * max_amplitude)
        sample_2 = int(sine_val_2 * max_amplitude)

        # Pack as a 16-bit signed integer ('h') in Little-Endian format ('<') for 2 separate channels[cite: 1]
        packed_sample = struct.pack("<hh", sample_1, sample_2)
        frames.append(packed_sample)

        # Flush the buffer to disk every time it reaches the size of one second[cite: 1]
        if len(frames) >= sample_rate:
            wav_file.writeframes(b"".join(frames))
            frames = []