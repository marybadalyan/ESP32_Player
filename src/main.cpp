#include <Arduino.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <nvs_flash.h>
#include <BluetoothA2DPSource.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ONLY_JPEG          // we only need JPEG support
#include "stb_image.h"

#include "minimp3.h"

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

BluetoothA2DPSource a2dp;

#define MP3_PATH "/song.mp3"
#define ART_PATH "/art.jpg"

static File         mp3File;
static mp3dec_t     mp3d;
static volatile int mp3Channels = 2;

// ─────────────────────────────
// RING BUFFER  (thread-safe)
// ─────────────────────────────
#define PCM_SIZE 12288
static int16_t      pcmRing[PCM_SIZE];
static volatile int pcmRead  = 0;
static volatile int pcmWrite = 0;
static portMUX_TYPE pcmMux   = portMUX_INITIALIZER_UNLOCKED;

void pushPCM(const int16_t *data, int len) {
  portENTER_CRITICAL(&pcmMux);
  for (int i = 0; i < len; i++) {
    int next = (pcmWrite + 1) % PCM_SIZE;
    if (next == pcmRead) {
  pcmRead = (pcmRead + 1) % PCM_SIZE; // overwrite oldest sample
}
    pcmRing[pcmWrite] = data[i];
    pcmWrite = next;
  }
  portEXIT_CRITICAL(&pcmMux);
}

bool popPCM(int16_t &out) {
  portENTER_CRITICAL(&pcmMux);
  if (pcmRead == pcmWrite) { portEXIT_CRITICAL(&pcmMux); return false; }
  out = pcmRing[pcmRead];
  pcmRead = (pcmRead + 1) % PCM_SIZE;
  portEXIT_CRITICAL(&pcmMux);
  return true;
}

// ─────────────────────────────
// ALBUM ART  (stb_image → OLED)
// ─────────────────────────────
void renderAlbumArt() {
  display.clearDisplay();

  bool artDrawn = false;

  if (SPIFFS.exists(ART_PATH)) {

    // ── 1. Load the whole JPEG into RAM ──────────────────────────
    File f = SPIFFS.open(ART_PATH, FILE_READ);
    size_t fileSize = f.size();
    Serial.printf("[ART] File size: %u bytes\n", fileSize);

    uint8_t *jpegBuf = (uint8_t *)malloc(fileSize);
    if (!jpegBuf) {
      Serial.println("[ART] malloc failed for JPEG buffer");
      f.close();
      goto fallback;
    }
    f.read(jpegBuf, fileSize);
    f.close();

    // ── 2. Decode with stb_image ─────────────────────────────────
    int srcW, srcH, channels;
    // Force 3 channels (RGB) regardless of source format
    uint8_t *pixels = stbi_load_from_memory(jpegBuf, fileSize,
                                             &srcW, &srcH, &channels, 3);
    free(jpegBuf); // done with raw JPEG bytes

    if (!pixels) {
      Serial.printf("[ART] stb_image decode failed: %s\n", stbi_failure_reason());
      goto fallback;
    }
    Serial.printf("[ART] Decoded: %dx%d px, %d ch\n", srcW, srcH, channels);

    // ── 3. Fit into 128×48 keeping aspect ratio ───────────────────
    const int DISP_W = 128;
    const int DISP_H = 64;

    float scaleX = (float)srcW / DISP_W;
    float scaleY = (float)srcH / DISP_H;
    float scale  = (scaleX > scaleY) ? scaleX : scaleY; // fit, not fill
    if (scale < 1.0f) scale = 1.0f;                     // never upscale

    int dstW = (int)(srcW / scale);
    int dstH = (int)(srcH / scale);
    int offX = (DISP_W - dstW) / 2;
    int offY = (DISP_H - dstH) / 2;
    Serial.printf("[ART] Rendering %dx%d at offset (%d,%d)\n",
                  dstW, dstH, offX, offY);

    // ── 4. Nearest-neighbour sample → grayscale → threshold ───────
    for (int dy = 0; dy < dstH; dy++) {
      // Map destination row back to source row
      int sy = (int)(dy * scale);
      if (sy >= srcH) sy = srcH - 1;

      for (int dx = 0; dx < dstW; dx++) {
        int sx = (int)(dx * scale);
        if (sx >= srcW) sx = srcW - 1;

        uint8_t *p = pixels + (sy * srcW + sx) * 3;
        // Standard luminance weights
        uint8_t gray = (uint8_t)(p[0] * 0.299f +
                                  p[1] * 0.587f +
                                  p[2] * 0.114f);

        // Simple threshold — tweak 128 up/down if too bright/dark
        display.drawPixel(offX + dx, offY + dy,
                          (gray > 128) ? WHITE : BLACK);
      }
    }

    stbi_image_free(pixels);
    artDrawn = true;
  }

  fallback:
  if (!artDrawn) {
    display.setCursor(20, 16);
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.print("BT Play");
  }

  display.display();
}

// ─────────────────────────────
// MP3 DECODE TASK  (Core 0)
// ─────────────────────────────
static uint8_t mp3InBuf[4096];
static int16_t mp3PcmBuf[1152 * 2];

void mp3Task(void *p) {
  int inLen = 0;
  mp3File.seek(0);

  while (true) {
    int space = (int)sizeof(mp3InBuf) - inLen;
    if (space > 0 && mp3File.available()) {
      inLen += mp3File.read(mp3InBuf + inLen, space);
    } else if (!mp3File.available()) {
      mp3File.seek(0);
      inLen = 0;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&mp3d, mp3InBuf, inLen, mp3PcmBuf, &info);

    if (info.frame_bytes > 0) {
      memmove(mp3InBuf, mp3InBuf + info.frame_bytes, inLen - info.frame_bytes);
      inLen -= info.frame_bytes;
    } else if (samples == 0 && info.frame_bytes == 0) {
      if (inLen > 0) { memmove(mp3InBuf, mp3InBuf + 1, inLen - 1); inLen--; }
    }

    if (samples > 0) {
      if (info.channels != mp3Channels) mp3Channels = info.channels;
      pushPCM(mp3PcmBuf, samples * info.channels);
    }

    int used = (pcmWrite - pcmRead + PCM_SIZE) % PCM_SIZE;
    vTaskDelay(used > (PCM_SIZE * 3 / 4) ? pdMS_TO_TICKS(20) : 1);
  }
}

// ─────────────────────────────
// A2DP AUDIO CALLBACK
// ─────────────────────────────
int32_t get_audio(Frame *frame, int32_t count) {
  int ch = mp3Channels;
  for (int i = 0; i < count; i++) {
    int16_t sL = 0, sR = 0;
    popPCM(sL);
    if (ch == 2) popPCM(sR); else sR = sL;
    frame[i].channel1 = sL;
    frame[i].channel2 = sR;
  }
  return count;
}

// ─────────────────────────────
// SETUP
// ─────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  // 1. NVS safe init
  esp_err_t nvsRet = nvs_flash_init();
  if (nvsRet == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvsRet == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("[NVS] corrupt — erasing");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(nvsRet);
  }

  // 2. SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS mount failed");
    while (true) delay(1000);
  }
  Serial.println("[SPIFFS] OK");

  // 3. Open MP3
  mp3File = SPIFFS.open(MP3_PATH, FILE_READ);
  if (!mp3File) {
    Serial.printf("[ERROR] Cannot open %s\n", MP3_PATH);
    while (true) delay(1000);
  }
  Serial.printf("[MP3] Opened: %d bytes\n", (int)mp3File.size());
  mp3dec_init(&mp3d);

  // 4. OLED + render art (before tasks — TJpgDec is not thread-safe)
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[WARN] SSD1306 not found");
  } else {
    Serial.println("[OLED] OK");
    renderAlbumArt();
  }

  // 5. Bluetooth A2DP
  a2dp.set_auto_reconnect(true);
  a2dp.start("WH-1000XM4", get_audio);
  Serial.println("[BT] A2DP started");

  // 6. MP3 decode task (Core 0)
  xTaskCreatePinnedToCore(mp3Task, "mp3", 32768, NULL, 2, NULL, 0);

  Serial.println("[SETUP] Complete");
}

void loop() {
  delay(1000);
}
