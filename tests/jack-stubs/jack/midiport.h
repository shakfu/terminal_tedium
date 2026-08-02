#ifndef _STUB_JACK_MIDIPORT_H
#define _STUB_JACK_MIDIPORT_H
#include <jack/jack.h>
typedef unsigned char jack_midi_data_t;
void jack_midi_clear_buffer(void *);
jack_midi_data_t *jack_midi_event_reserve(void *, jack_nframes_t, size_t);
#endif
