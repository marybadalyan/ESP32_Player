# ESP32 Bluetooth MP3 Player

Plays MP3 files from SPIFFS over Bluetooth A2DP to any BT headphones, and displays album art on a 0.96" SSD1306 OLED.

---

## Explanation

1. Music is stored in an MP3 file as compressed digital data.

2. The MP3 contains information describing which frequencies
   exist and how strong they are over small periods of time.

3. The music player decodes the MP3 and reconstructs PCM samples.

4. PCM samples are numbers representing the instantaneous
   amplitude of the sound wave.

5. For CD quality audio, PCM typically consists of:

      44,100 samples/sec
      16 bits/sample
      2 channels (stereo)

   resulting in about 176 KB/s of raw audio data.

6. A DAC converts the PCM samples into a rapidly changing voltage,
   creating a staircase-like waveform.

7. Analog filters smooth the staircase into a continuous analog
   waveform.

8. For Bluetooth audio, the PCM is compressed again using SBC.

9. SBC analyzes the audio and removes information considered less
   important to human hearing, reducing bandwidth requirements.

10. Bluetooth sends the compressed data as digital radio packets.

11. The headphones receive the packets, decode SBC back into PCM,
    and use a DAC to recreate the analog waveform.

12. The analog signal drives a coil inside the speaker driver.

13. The coil interacts with a magnet, moving the diaphragm.

14. The diaphragm moves air, producing pressure waves that reach
    your ears and are perceived as sound.

---

## Hardware

| ESP32 Pin | OLED (SSD1306) |
|-----------|---------------|
| 3.3V      | VCC           |
| GND       | GND           |
| GPIO 21   | SDA           |
| GPIO 22   | SCL           |

- OLED: 0.96" 128x64 I2C SSD1306
- ESP32: any standard 30-pin ESP32 dev board
- Headphones: any Bluetooth A2DP device (tested on Sony WH-1000XM4)

---

## Prerequisites

Install these on your computer before anything else.

### yt-dlp

- macOS: `brew install yt-dlp`
- Windows: `winget install yt-dlp.yt-dlp`
- Linux: `sudo apt install yt-dlp`

### FFmpeg

- macOS: `brew install ffmpeg`
- Windows: `winget install Gyan.FFmpeg`
- Linux: `sudo apt install ffmpeg`

### PlatformIO

Install the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode) for VS Code, or the CLI:

```bash
pip install platformio
```

---

## Adding a Song

Use the included script to download a song from YouTube and prepare it for the device. It downloads the audio at a given bitrate, grabs the thumbnail, resizes it to fit the OLED, and drops both files into `data/`.

```bash
./fetch_song.sh "<youtube_url>" <bitrate> <img_width> <img_height>
```

**Example:**
```bash
./fetch_song.sh "https://www.youtube.com/watch?v=sMlwsCjvV2Y" 64K 128 48
```

| Argument     | Default | Description                          |
|--------------|---------|--------------------------------------|
| youtube_url  | —       | Full YouTube URL (required)          |
| bitrate      | 128K    | Audio quality: 64K, 128K, 192K, 320K |
| img_width    | 128     | Thumbnail width in pixels            |
| img_height   | 48      | Thumbnail height in pixels (OLED top area) |

The script outputs:
- `data/song.mp3` — the audio file
- `data/art.jpg`  — the resized album art

---

## Building and Flashing

### 1. Upload the filesystem (song + art)

This uploads everything in `data/` to SPIFFS on the ESP32:

```bash
pio run --target uploadfs
```

### 2. Compile and flash the firmware

```bash
pio run --target upload
```

### 3. Monitor serial output (optional)

```bash
pio device monitor
```

You should see:

```
[SPIFFS] OK
[MP3] Opened: XXXXXX bytes
[OLED] OK
[ART] Raw: 128x48
[ART] Drawn 128x48 (÷1) at (0,0)
[BT] A2DP started
[SETUP] Complete
```

### Do both in one command

```bash
pio run --target uploadfs && pio run --target upload
```

---

## Changing the Target Headphones

In `src/main.cpp`, find this line in `setup()` and change the name to match your headphones exactly as it appears in your phone's Bluetooth settings:

```cpp
a2dp.start("WH-1000XM4", get_audio);
```

---

## Partition Layout

The ESP32 flash is split as follows (`partitions.csv`):

| Name     | Size  | Purpose                  |
|----------|-------|--------------------------|
| nvs      | 24 KB | Bluetooth pairing data   |
| app0     | 1.875 MB | Firmware              |
| coredump | 64 KB | Crash logs               |
| spiffs   | 2 MB  | song.mp3 + art.jpg       |

The SPIFFS partition fits roughly one song at 64 kbps (≈ 1.5 MB for a 3-minute track) plus the art.

---

## Troubleshooting

**No sound / wrong speed**
Run `pio device monitor` and check the MP3 probe line. The A2DP stack always runs at 44100 Hz stereo — if your MP3 is a different rate the ring buffer yield logic handles it.

**Art not showing**
Make sure `art.jpg` is a standard baseline JPEG (not progressive). The `fetch_song.sh` script handles this automatically via FFmpeg. If you're supplying your own image, convert it:
```bash
ffmpeg -i input.jpg -q:v 2 art.jpg
```

**SPIFFS mount failed**
Run `pio run --target uploadfs` again. If it still fails, erase the flash first:
```bash
pio run --target erase
pio run --target uploadfs
pio run --target upload
```

**Bluetooth won't connect**
The device scans for the headphone name on boot. Make sure the headphones are in pairing mode and the name in `a2dp.start()` matches exactly.
