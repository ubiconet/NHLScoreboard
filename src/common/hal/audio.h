#pragma once

#include <Arduino.h>

// MAX98357 I2S amplifier driver (goal horn / cues). 16-bit mono PCM clips
// (PROGMEM) stream over I2S TX with DMA; playback is NON-BLOCKING —
// startClip() hands the clip to updateAudio(), which the render loop
// calls every tick to top up the DMA buffers while the scoreboard keeps
// drawing.

// Prepares the I2S peripheral. Pins come from the sport's config.
void initAudio(int bclkPin, int lrcPin, int dinPin);

// Starts playing a PCM clip (16-bit mono at its own sample rate).
// Replaces any clip already playing. Streams from a dedicated task on
// core 0 — playback is unaffected by whatever the render core is doing.
void startClip(const int16_t* data, size_t count, int sampleRate);

// True while a clip is still playing.
bool audioPlaying();
