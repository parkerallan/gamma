// miniaudio implementation translation unit.
//
// Built as C (not C++) so the single-header library compiles in its native
// language. AudioEngine.cpp includes miniaudio.h normally; this file is the
// one place where MA_IMPLEMENTATION expands the source.

#define MA_IMPLEMENTATION
#define MA_NO_FLAC          // we only support wav / mp3 / ogg
// Disable backends that require headers/libraries we don't ship with.
// On Windows the WASAPI backend is what we use; the others are not needed.
#define MA_NO_JACK
#define MA_NO_PULSEAUDIO
#define MA_NO_ALSA
#define MA_NO_COREAUDIO
#define MA_NO_SNDIO
#define MA_NO_AUDIO4
#define MA_NO_OSS
#define MA_NO_AAUDIO
#define MA_NO_OPENSL

#include "miniaudio.h"
