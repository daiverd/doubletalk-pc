#ifndef DOUBLETALK_SPD_AUDIO_COMPAT_H
#define DOUBLETALK_SPD_AUDIO_COMPAT_H

/*
 * Speech Dispatcher 0.12.1 installs spd_module_main.h and
 * spd_audio_plugin.h, but some distributions omit the small spd_audio.h
 * wrapper included by spd_module_main.h. The module basis only needs the
 * AudioTrack and AudioFormat declarations from the installed plugin header.
 */
#include <spd_audio_plugin.h>

#endif
