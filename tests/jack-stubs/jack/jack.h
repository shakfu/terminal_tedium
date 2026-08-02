/* Minimal JACK API stub, for parsing bindings/jack on a machine without
 * JACK installed. Signatures follow the public jack/jack.h. Not for linking. */
#ifndef _STUB_JACK_H
#define _STUB_JACK_H
#include <stdint.h>
typedef uint32_t jack_nframes_t;
typedef uint64_t jack_time_t;
typedef struct _jack_client jack_client_t;
typedef struct _jack_port jack_port_t;
typedef enum { JackNoStartServer = 0x01 } jack_options_t;
typedef enum { JackFailure = 0x01 } jack_status_t;
enum { JackPortIsInput = 0x1, JackPortIsOutput = 0x2 };
#define JACK_DEFAULT_AUDIO_TYPE "32 bit float mono audio"
#define JACK_DEFAULT_MIDI_TYPE  "8 bit raw midi"
typedef int  (*JackProcessCallback)(jack_nframes_t, void *);
typedef int  (*JackSampleRateCallback)(jack_nframes_t, void *);
typedef int  (*JackBufferSizeCallback)(jack_nframes_t, void *);
typedef void (*JackShutdownCallback)(void *);
jack_client_t *jack_client_open(const char *, jack_options_t, jack_status_t *, ...);
int   jack_client_close(jack_client_t *);
int   jack_activate(jack_client_t *);
int   jack_deactivate(jack_client_t *);
int   jack_set_process_callback(jack_client_t *, JackProcessCallback, void *);
int   jack_set_sample_rate_callback(jack_client_t *, JackSampleRateCallback, void *);
int   jack_set_buffer_size_callback(jack_client_t *, JackBufferSizeCallback, void *);
void  jack_on_shutdown(jack_client_t *, JackShutdownCallback, void *);
jack_nframes_t jack_get_sample_rate(jack_client_t *);
jack_nframes_t jack_get_buffer_size(jack_client_t *);
jack_port_t *jack_port_register(jack_client_t *, const char *, const char *,
                                unsigned long, unsigned long);
void *jack_port_get_buffer(jack_port_t *, jack_nframes_t);
const char *jack_port_name(const jack_port_t *);
const char **jack_get_ports(jack_client_t *, const char *, const char *, unsigned long);
int   jack_connect(jack_client_t *, const char *, const char *);
void  jack_free(void *);
int   jack_get_cycle_times(const jack_client_t *, jack_nframes_t *,
                           jack_time_t *, jack_time_t *, float *);
#endif
