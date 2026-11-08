#include "hal/Audio.h"

#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

static const uint8_t kToneWav[] PROGMEM = {
#include "tone440_wav.inc"
};

void Audio::begin() {
  // Amp SD_MODE appears hardwired/float-enabled; nothing to init.
  pinMode(kLrck, INPUT_PULLUP);  // shared with touch when idle
}

void Audio::playBeep() {
  if (volume_ == 0) {
    return;
  }
  // I2S takes BCLK=EPD CS and LRCK=touch GPIO2
  AudioOutputI2S *out = new AudioOutputI2S();
  out->SetPinout(kBclk, kLrck, kDout);
  out->SetGain(volume_ * 0.08f);  // 6 → 0.48, the previous fixed level

  AudioFileSourcePROGMEM *src =
      new AudioFileSourcePROGMEM(kToneWav, sizeof(kToneWav));
  AudioGeneratorWAV *wav = new AudioGeneratorWAV();

  if (wav->begin(src, out)) {
    uint32_t t0 = millis();
    while (wav->isRunning() && (millis() - t0) < 500) {
      if (!wav->loop()) {
        break;
      }
      yield();
      ESP.wdtFeed();
    }
    wav->stop();
  }

  delete wav;
  delete src;
  delete out;

  // Restore shared pins for EPD + touch
  pinMode(kBclk, OUTPUT);
  digitalWrite(kBclk, HIGH);  // EPD CS inactive
  pinMode(kLrck, INPUT_PULLUP);
}
