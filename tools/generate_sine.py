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

# Prompt the user for the frequency via terminal input
while True:
    try:
        user_input = input(
            "Enter the frequency to generate in Hz (e.g., 1000 or 440): "
        )
        frequency = float(user_input)
        if frequency <= 0:
            print("Frequency must be greater than 0 Hz.")
            continue
        if frequency > sample_rate / 2:
            print(
                f"Warning: Frequency exceeds the Nyquist limit ({sample_rate / 2} Hz) for this sample rate."
            )
            continue
        break
    except ValueError:
        print("Invalid input. Please enter a valid numerical value.")

# Dynamic filename generation based purely on the chosen frequency
filename = f"sine_{format_num(frequency)}.wav"

# 16-bit audio configuration: Maximum peak amplitude for signed 16-bit is 32767
max_amplitude = 32767

num_samples = int(sample_rate * duration)

print(f"\nGenerating '{filename}'...")

# Open the WAV file for writing
with wave.open(filename, "w") as wav_file:
    # Set parameters: (num_channels, sample_width_bytes, sample_rate, num_frames, compression_type, compression_name)
    # 2 channels = Stereo, 2 bytes = 16-bit
    wav_file.setparams(
        (2, 2, sample_rate, num_samples, "NONE", "not compressed")
    )

    # Generate and write frames block by block to save memory
    frames = []
    for i in range(num_samples):
        # Calculate the current sine wave value
        t = float(i) / sample_rate
        sine_val = math.sin(2.0 * math.pi * frequency * t)

        # Scale to maximum 16-bit amplitude and convert to integer
        sample = int(sine_val * max_amplitude)

        # Pack as a 16-bit signed integer ('h') in Little-Endian format ('<')
        # Duplicate the sample for identical Left and Right channels (phase-aligned Stereo)
        packed_sample = struct.pack("<hh", sample, sample)
        frames.append(packed_sample)

        # Flush the buffer to disk every time it reaches the size of one second
        if len(frames) >= sample_rate:
            wav_file.writeframes(b"".join(frames))
            frames = []