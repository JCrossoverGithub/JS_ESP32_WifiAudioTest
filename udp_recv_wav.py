import socket
import wave
import struct
import time

PORT = 3333
SAMPLE_RATE = 20000
CHANNELS = 1
SAMPWIDTH = 2
OUT_WAV = "capture.wav"

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(2.0)

wf = wave.open(OUT_WAV, "wb")
wf.setnchannels(CHANNELS)
wf.setsampwidth(SAMPWIDTH)
wf.setframerate(SAMPLE_RATE)

print(f"Listening on UDP :{PORT} -> writing {OUT_WAV}")
print("Ctrl+C to stop.")

last_seq = None
packets = 0
drops = 0
start = time.time()
got_any = False

try:
    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except TimeoutError:
            print("...waiting for packets (is ESP32 streaming?)")
            continue

        if len(data) < 4:
            continue

        got_any = True
        (seq,) = struct.unpack("<I", data[:4])
        payload = data[4:]

        if last_seq is not None and seq != last_seq + 1:
            drops += max(0, seq - (last_seq + 1))
        last_seq = seq

        wf.writeframesraw(payload)
        packets += 1

        if packets % 100 == 0:
            dt = time.time() - start
            print(f"pkts={packets} drops={drops} secs={dt:.1f} from {addr}")

except KeyboardInterrupt:
    pass
finally:
    wf.close()
    sock.close()
    if got_any:
        print("Done. Play:", OUT_WAV)
    else:
        print("No packets received. Check DEST_IP / firewall / same Wi-Fi.")
