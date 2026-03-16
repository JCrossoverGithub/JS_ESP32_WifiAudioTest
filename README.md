# ESP32 UDP Audio Streamer

An embedded audio streaming project that turns an **ESP32 + analog microphone** into a simple **wireless microphone / audio capture device**.

The ESP32 samples audio from a **MAX4466 analog microphone**, converts the raw ADC readings into **16-bit PCM mono audio**, groups the samples into small frames, and streams them over **Wi-Fi using UDP** to a computer. A Python receiver script listens for the packets and writes the incoming stream to a **WAV file** for playback and testing.

![Breadboard prototype](breadboard-setup.png)

## What this project does

- Captures live microphone audio on an ESP32
- Uses the ESP32 ADC in **continuous mode**
- Converts 12-bit ADC readings into **PCM16** samples
- Applies a lightweight **DC offset removal** step before transmission
- Sends audio frames over **UDP** to a PC on the same network
- Prepends each packet with a **sequence number** so packet loss can be detected
- Reconstructs the stream on the PC and saves it as `capture.wav`

## Why this project is useful

This project is a good example of an **end-to-end embedded systems pipeline**. It combines:

- **Embedded C / ESP-IDF**
- **ADC-based signal capture**
- **basic digital audio handling**
- **Wi-Fi networking with UDP sockets**
- **Python tooling for host-side capture and debugging**

This shows both **firmware development** and **computer-side integration**, rather than simply reading sensor output.

## System overview

### ESP32 side (`main.c`)

The firmware:

1. Connects the ESP32 to Wi-Fi in station mode
2. Configures the ADC for continuous sampling on **GPIO34 / ADC1_CH6**
3. Samples audio at **20,000 Hz**
4. Converts raw ADC readings into signed **16-bit PCM** audio
5. Packs each audio frame into a UDP packet with this layout:

```text
[u32 sequence number][PCM16 audio payload]
```

6. Streams packets to a destination PC at **UDP port 3333**

### PC side (`udp_recv_wav.py`)

The Python receiver:

1. Binds to UDP port **3333**
2. Receives audio packets from the ESP32
3. Extracts the sequence number
4. Detects dropped packets
5. Appends the payload to a WAV file named **`capture.wav`**

## Audio format

- **Sample rate:** 20,000 Hz
- **Channels:** 1 (mono)
- **Sample width:** 16-bit PCM
- **Frame length:** 20 ms
- **Samples per frame:** 400
- **UDP audio payload:** 800 bytes per packet
- **UDP header field:** 4-byte sequence number

## Hardware

### Main components

- ESP32 development board
- MAX4466 analog microphone breakout
- Breadboard and jumper wires
- USB cable for programming/power
- Computer on the same Wi-Fi network

### Signal path

```text
Microphone -> ESP32 ADC -> PCM conversion -> UDP over Wi-Fi -> Python receiver -> WAV file
```

## Repository files

```text
main.c            ESP32 firmware for audio capture and UDP streaming
udp_recv_wav.py   Python script that receives UDP packets and writes capture.wav
```

## Setup

### 1. Configure the ESP32 firmware

In `main.c`, update these values before building:

```c
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASS "your_wifi_password"
#define DEST_IP   "your_computer_ipv4"
```

`DEST_IP` should be the IPv4 address of the computer running `udp_recv_wav.py`.

### 2. Connect the hardware

The code is written for:

- **Microphone output -> GPIO34**
- **ADC input channel -> ADC1_CH6**

Typical analog mic breakout connections:

- `VCC -> 3.3V`
- `GND -> GND`
- `OUT -> GPIO34`

### 3. Build and flash the ESP32

This project is intended for **ESP-IDF**.

Typical workflow:

```bash
idf.py build
idf.py -p <your-port> flash monitor
```

### 4. Run the Python receiver on your PC

```bash
python udp_recv_wav.py
```

When packets are received, the script will begin writing audio to:

```text
capture.wav
```

## How it works internally

### ADC to PCM conversion

The firmware uses a small IIR-based DC estimator to remove offset from the microphone signal before scaling it into the signed 16-bit range. This helps center the waveform around zero and makes the saved audio more usable.

### Packetization

Audio samples are buffered into 20 ms frames. Each frame is transmitted in a single UDP packet. A 32-bit sequence number is added to the front of the packet so the receiver can track missing packets.

The firmware also reduces Wi-Fi TX power and disables Wi-Fi power saving to improve streaming stability.

## Future improvements

- Add gain control or filtering
- Stream to a live playback app instead of only writing a WAV file
- Add buffering / jitter handling on the receiver
- Use I2S microphones for improved audio quality
- Add compression such as ADPCM or Opus on a stronger platform
- Build a web dashboard for recording and monitoring

## Skills demonstrated

- Embedded C
- ESP-IDF
- ESP32 ADC continuous mode
- Wi-Fi socket programming
- UDP packet streaming
- Python networking
- WAV file generation
- Debugging hardware/software integration

## License

Add the license of your choice if you plan to publish this publicly.
