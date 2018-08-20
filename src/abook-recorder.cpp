/** @file simple_client.c
 *
 * @brief This simple client demonstrates the basic features of JACK
 * as they would be used by many applications.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_video.h>
#include <SDL2/SDL_ttf.h>
#include <time.h>
#include <sys/stat.h>

// Maximum 60 seconds of recording per segment
#define MAX_SAMPLES (48000 * 60)

int16_t recordingBuffer[MAX_SAMPLES * 2];

int fullScreen = 0;

#include "alsa/asoundlib.h"

#ifdef __ARMEL__
void text(const char *message, int x, int y);

struct ButtonMap {
    int gpio;
    int key;
    const char *label;
    int x;
    int y;
    int state;
};

struct ButtonMap buttons[] = {
    { 23, SDLK_r, "R", 0, 0, 0},  // Circle: record
    { 22, SDLK_d, "D", 0, 80, 0},  // Square: delete
    { 24, SDLK_c, "C", 0, 160, 0},  // Triangle: combine
    { 5,  SDLK_q, "Q", 0, 220, 0},  // Cross: quit
    { 17, SDLK_n, "N", 300, 40, 0},  // End 1: noise
    { 4,  SDLK_b, "Bl", 290, 180, 0},  // End 2: nothing
    { 0, 0, 0, 0, 0, 0} // End of list
};

#include <pigpio.h>

int backlight = 1;

void initBacklight() {
        gpioSetMode(27, PI_OUTPUT);
}

void toggleBacklight() {
    backlight = 1 - backlight;
    gpioWrite(27, backlight);
}

void initButtons() {

    int uid = getuid();
    if (uid != 0) {
        printf("Error: this must be run as root to access GPIO.\n");
        exit(10);
    }

    gpioInitialise();
    for (int i = 0; buttons[i].label != 0; i++) {
        gpioSetMode(buttons[i].gpio, PI_INPUT);
        gpioSetPullUpDown(buttons[i].gpio, PI_PUD_UP);
        buttons[i].state = gpioRead(buttons[i].gpio);
    }
}

void mapButtons() {
    for (int i = 0; buttons[i].label != 0; i++) {
        int r = gpioRead(buttons[i].gpio);
        if (buttons[i].state != r) {
            buttons[i].state = r;
            if (r == 0) {
                SDL_Event e;
                e.type = SDL_KEYDOWN;
                e.key.keysym.sym = buttons[i].key;
                e.key.repeat = 0;
                SDL_PushEvent(&e);
            } else {
                SDL_Event e;
                e.type = SDL_KEYUP;
                e.key.keysym.sym = buttons[i].key;
                e.key.repeat = 0;
                SDL_PushEvent(&e);
            }
        }
    }
}

void drawButtons() {
    for (int i = 0; buttons[i].label != 0; i++) {
        text(buttons[i].label, buttons[i].x, buttons[i].y);
    }
}
#else
void initBacklight() {
}

void toggleBacklight() {
}

void initButtons() {
}

void mapButtons() {
}

void drawButtons() {
}
#endif

//#include <samplerate.h>

const uint8_t mainfont[] = {
#include "LiberationSans-Regular.h"
};

snd_pcm_t *alsa_handle;

SDL_Window *_window;
SDL_Surface *_display;
SDL_Surface *_backing;

struct wav {
	// RIFF header
	uint32_t riff_chunkid;
	uint32_t riff_chunksize;
	uint32_t riff_format;

	// Format header
	uint32_t fmt_chunkid;
	uint32_t fmt_chunksize;
	uint16_t fmt_audioformat;
	uint16_t fmt_numchannels;
	uint32_t fmt_samplerate;
	uint32_t fmt_byterate;
	uint16_t fmt_blockalign;
	uint16_t fmt_bitspersample;

	// Data chunk
	uint32_t data_chunkid;
	uint32_t data_chunksize;
};

int quit = 0;
double resample_mean = 1.0;
double static_resample_factor = 1.0;
double resample_lower_limit = 0.25;
double resample_upper_limit = 4.0;

double *offset_array;
double *window_array;
int offset_differential_index = 0;

double offset_integral = 0;

// ------------------------------------------------------ commandline parameters

int sample_rate = 48000;				 /* stream rate */
int num_channels = 2;				 /* count of channels */
int period_size = 1024;
int num_periods = 2;

int target_delay = 0;	    /* the delay which the program should try to approach. */
int max_diff = 0;	    /* the diff value, when a hard readpointer skip should occur */
int catch_factor = 100000;
int catch_factor2 = 10000;
double pclamp = 15.0;
double controlquant = 10000.0;
int smooth_size = 256;
int good_window=0;
int verbose = 0;
int instrument = 0;
int samplerate_quality = 2;

// Debug stuff:

volatile float output_resampling_factor = 1.0;
volatile int output_new_delay = 0;
volatile float output_offset = 0.0;
volatile float output_integral = 0.0;
volatile float output_diff = 0.0;
volatile int running_freewheel = 0;

snd_pcm_uframes_t real_buffer_size;
snd_pcm_uframes_t real_period_size;

// buffers

// format selection, and corresponding functions from memops in a nice set of structs.

// Alsa stuff... i dont want to touch this bullshit in the next years.... please...

static int xrun_recovery(snd_pcm_t *handle, int err) {
//    printf( "xrun !!!.... %d\n", err );
	if (err == -EPIPE) {	/* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0)
			printf("Can't recover from underrun, prepare failed: %s\n", snd_strerror(err));
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			usleep(100);	/* wait until the suspend flag is released */
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recover from suspend, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	}
	return err;
}

static int set_hwformat( snd_pcm_t *handle, snd_pcm_hw_params_t *params )
{
	return snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16);
}

static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *params, snd_pcm_access_t access, int rate, int channels, int period, int nperiods ) {
	int err, dir=0;
	unsigned int buffer_time;
	unsigned int period_time;
	unsigned int rrate;
	unsigned int rchannels;

	/* choose all parameters */
	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
		return err;
	}
	/* set the interleaved read/write format */
	err = snd_pcm_hw_params_set_access(handle, params, access);
	if (err < 0) {
		printf("Access type not available for playback: %s\n", snd_strerror(err));
		return err;
	}

	/* set the sample format */
	err = set_hwformat(handle, params);
	if (err < 0) {
		printf("Sample format not available for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* set the count of channels */
	rchannels = channels;
	err = snd_pcm_hw_params_set_channels_near(handle, params, &rchannels);
	if (err < 0) {
		printf("Channels count (%i) not available for record: %s\n", channels, snd_strerror(err));
		return err;
	}
	if (rchannels != channels) {
		printf("WARNING: chennel count does not match (requested %d got %d)\n", channels, rchannels);
		num_channels = rchannels;
	}
	/* set the stream rate */
	rrate = rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
	if (err < 0) {
		printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
		return err;
	}
	if (rrate != rate) {
		printf("WARNING: Rate doesn't match (requested %iHz, get %iHz)\n", rate, rrate);
		sample_rate = rrate;
	}
	/* set the buffer time */

	buffer_time = 1000000*(uint64_t)period*nperiods/rate;
	err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir);
	if (err < 0) {
		printf("Unable to set buffer time %i for playback: %s\n",  1000000*period*nperiods/rate, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_get_buffer_size( params, &real_buffer_size );
	if (err < 0) {
		printf("Unable to get buffer size back: %s\n", snd_strerror(err));
		return err;
	}
	if( real_buffer_size != nperiods * period ) {
	    printf( "WARNING: buffer size does not match: (requested %d, got %d)\n", nperiods * period, (int) real_buffer_size );
	}
	/* set the period time */
	period_time = 1000000*(uint64_t)period/rate;
	err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, &dir);
	if (err < 0) {
		printf("Unable to set period time %i for playback: %s\n", 1000000*period/rate, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_get_period_size(params, &real_period_size, NULL );
	if (err < 0) {
		printf("Unable to get period size back: %s\n", snd_strerror(err));
		return err;
	}
	if( real_period_size != period ) {
	    printf( "WARNING: period size does not match: (requested %i, got %i)\n", period, (int)real_period_size );
	}
	/* write the parameters to device */
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}

static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams, int period) {
	int err;

	/* get the current swparams */
	err = snd_pcm_sw_params_current(handle, swparams);
	if (err < 0) {
		printf("Unable to determine current swparams for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* start the transfer when the buffer is full */
	err = snd_pcm_sw_params_set_start_threshold(handle, swparams, period );
	if (err < 0) {
		printf("Unable to set start threshold mode for capture: %s\n", snd_strerror(err));
		return err;
	}
	err = snd_pcm_sw_params_set_stop_threshold(handle, swparams, -1 );
	if (err < 0) {
		printf("Unable to set start threshold mode for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* allow the transfer when at least period_size samples can be processed */
	err = snd_pcm_sw_params_set_avail_min(handle, swparams, 2*period );
	if (err < 0) {
		printf("Unable to set avail min for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* align all transfers to 1 sample */
	err = snd_pcm_sw_params_set_xfer_align(handle, swparams, 1);
	if (err < 0) {
		printf("Unable to set transfer align for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* write the parameters to the playback device */
	err = snd_pcm_sw_params(handle, swparams);
	if (err < 0) {
		printf("Unable to set sw params for capture: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}

// ok... i only need this function to communicate with the alsa bloat api...

static snd_pcm_t *open_audiofd( char *device_name, int capture, int rate, int channels, int period, int nperiods ) {
  int err;
  snd_pcm_t *handle;
  snd_pcm_hw_params_t *hwparams;
  snd_pcm_sw_params_t *swparams;

  snd_pcm_hw_params_alloca(&hwparams);
  snd_pcm_sw_params_alloca(&swparams);

  if ((err = snd_pcm_open(&(handle), device_name, capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK )) < 0) {
      printf("Capture open error: %s\n", snd_strerror(err));
      return NULL;
  }

  if ((err = set_hwparams(handle, hwparams,SND_PCM_ACCESS_RW_INTERLEAVED, rate, channels, period, nperiods )) < 0) {
      printf("Setting of hwparams failed: %s\n", snd_strerror(err));
      return NULL;
  }
  if ((err = set_swparams(handle, swparams, period)) < 0) {
      printf("Setting of swparams failed: %s\n", snd_strerror(err));
      return NULL;
  }

  snd_pcm_start( handle );
  snd_pcm_wait( handle, 200 );

  return handle;
}

double hann( double x )
{
	return 0.5 * (1.0 - cos( 2*M_PI * x ) );
}

void printUsage() {
fprintf(stderr, "usage: alsa_out [options]\n"
		"\n"
		"  -d <alsa_device> \n"
		"\n");
}


/**
 * the main function....
 */

void
sigterm_handler( int signal )
{
	quit = 1;
}

int recording = 0;
int recordingRoomNoise = 0;
int recordFd = 0;
uint32_t samples = 0;
char filename[1024] = {0};
int firstSample = 0;
int lastSample = 0;

TTF_Font *filenameFont;

int segmentNo = 0;
struct tm *sessionTime;


SDL_Color white = {255, 255, 255};

int noiseFloor = 0;



void initSDL() {
    atexit(SDL_Quit);

    SDL_SetHintWithPriority(SDL_HINT_NO_SIGNAL_HANDLERS, "1", SDL_HINT_OVERRIDE);

    SDL_Init(SDL_INIT_EVERYTHING);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();


    if (fullScreen) {
	    _window = SDL_CreateWindow("Audiobook Recorder",
		SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED,
		320,
		240,
		SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN
	    );
    } else {
        _window = SDL_CreateWindow("Audiobook Recorder",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            320,
            240,
            SDL_WINDOW_SHOWN
        );
    }
    if (!_window) {
        printf("Unable to create screen: %s\n", SDL_GetError());
        SDL_Quit();
    exit(10);
    }

    _backing = SDL_GetWindowSurface(_window);
    if (!_backing) {
        printf("Unable to get backing surface: %s\n", SDL_GetError());
        SDL_Quit();
        exit(10);
    }

    _display = SDL_CreateRGBSurfaceWithFormat(0, 320, 240, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!_display) {
        printf("Unable to create framebuffer surface: %s\n", SDL_GetError());
        SDL_Quit();
        exit(10);
    }

	filenameFont = TTF_OpenFontRW(SDL_RWFromConstMem(mainfont, sizeof(mainfont)), 1, 14);
}

void clearScreen() {
    SDL_FillRect(_display, NULL, 0xFF000000);
}

void text(const char *message, int x, int y) {
	SDL_Surface *fn = TTF_RenderText_Blended(filenameFont, message, white);
	SDL_Rect r;
	r.x = x;
	r.y = y;
	r.w = fn->w;
	r.h = fn->h;
	SDL_BlitSurface(fn, NULL, _display, &r);
	SDL_FreeSurface(fn);
}

void displaySummary() {
    char temp[1024];
//    clearScreen();


    if (samples > 0) {
        int px = 0;
        int div = samples / 320;
        SDL_Rect r;

        for (int i = 0; i < samples; i += div) {
            int maxval = 0;
            int minval = 0;
            int upav = 0;
	    int upnum = 0;
            int downav = 0;
	    int downnum = 0;
            for (int j = 0; j < div; j++) {
		int left = recordingBuffer[(i + j) * 2];
		int right = recordingBuffer[(i + j) * 2 + 1];

                if (left > maxval) maxval = left;
		if (right > maxval) maxval = right;
		if (left < minval) minval = left;
		if (right < minval) minval = right;
		if (left > 0) { upav += left; upnum++; }
		if (left < 0) { downav += left; downnum++; }
		if (right > 0) { upav += right; upnum++; }
		if (right < 0) { downav += right; downnum++; }
            }

	    if (upnum > 0) {
		    upav /= upnum;
            } else {
                    upav = 0;
            }
	    if (downnum > 0) {
		    downav /= downnum;
            } else {
                    downav = 0;
            }

            maxval /= 250;
            minval /= 250;

            upav /= 250;
            downav /= 250;

            r.x = px;
            r.w = 1;
            r.y = 120 - maxval;
            r.h = abs(maxval - minval) + 1;
            SDL_FillRect(_display, &r, 0xFF004080);

            r.x = px;
            r.w = 1;
            r.y = 120 - upav;
            r.h = abs(upav - downav) + 1;
            SDL_FillRect(_display, &r, 0xFF4080F0);

            px++;
        }

        r.x = firstSample / div;
        r.y = 120 - 30;
        r.h = 61;
        r.w = 1;
        SDL_FillRect(_display, &r, 0xFF804000);

        r.x = lastSample / div;
        r.y = 120 - 30;
        r.h = 61;
        r.w = 1;
        SDL_FillRect(_display, &r, 0xFF804000);

        
    }


    sprintf(temp, "Session: %s", filename);
    text(temp, 20, 60);
    sprintf(temp, "Segments: %d", segmentNo);
    text(temp, 20, 80);
    sprintf(temp, "Noise floor: %d", noiseFloor);
    text(temp, 20, 100);

    text("Press N to record room noise", 20, 120);
    text("Press C to combine session to WAV", 20, 140);
    text("Press R to record a new segment", 20, 160);
    text("Press D to delete last segment", 20, 180);
    text("Press Q to quit", 20, 200);

//    updateScreen();
}

void updateScreen() {
    displaySummary();
    drawButtons();
    SDL_BlitSurface(_display, NULL, _backing, NULL);
    SDL_UpdateWindowSurface(_window);
}

void flushRecordingDevice() {
    int numSamples = snd_pcm_avail( alsa_handle );
	if (numSamples == 0) return;
	if (numSamples > MAX_SAMPLES) numSamples = MAX_SAMPLES;
    do {
        snd_pcm_readi( alsa_handle, (char *)&recordingBuffer[0], numSamples);
        numSamples = snd_pcm_avail( alsa_handle );
        if (numSamples > MAX_SAMPLES) numSamples = MAX_SAMPLES;
    } while (numSamples > 0);
}

void recordRoomNoise() {
	noiseFloor = 0;
	samples = 0;

	mkdir(filename, 0777);

	SDL_FillRect(_display, NULL, 0xFFFF0000);

	text("Recording Room Noise. Be Silent!", 20, 20);
	
    SDL_Delay(100); // Little delay to avoid recording the click.

    flushRecordingDevice();
	updateScreen();
	recording = 1;
	recordingRoomNoise = 1;
}

void startRecording() {

    if (noiseFloor == 0) {
        clearScreen();
        text("No room noise recorded!", 20, 20);
        updateScreen();
        return;
    }

    SDL_Delay(100); // Little delay to avoid recording the click.

    flushRecordingDevice();
	samples = 0;

	segmentNo++;

	char temp[100];
	sprintf(temp, "Segment %d", segmentNo);

	SDL_FillRect(_display, NULL, 0xFFFF0000);
	text(temp, 20, 20);

	updateScreen();
	recording = 1;
	recordingRoomNoise = 0;
}

int loadFileToBuffer(const char *fn) {
    int afd = open(fn, O_RDONLY);
    struct wav header;
    read(afd, &header, sizeof(header));
    read(afd, recordingBuffer, header.data_chunksize);
    close(afd);
    return header.data_chunksize / 4;
}

void loadRoomNoise() {
    char temp[1024];
    sprintf(temp, "%s/room-noise.wav", filename);
    samples = loadFileToBuffer(temp);
    noiseFloor = 0;
    for (int i = 0; i < samples; i++) {
        if (abs(recordingBuffer[i * 2]) > noiseFloor) {
            noiseFloor = abs(recordingBuffer[i * 2]);
        }
        if (abs(recordingBuffer[i * 2 + 1]) > noiseFloor) {
            noiseFloor = abs(recordingBuffer[i * 2 + 1]);
        }
    }

    noiseFloor *= 10;
    noiseFloor /= 9;
}

void stopRecording() {

	firstSample = 0;
	lastSample = samples - 1;

    int validSamples = samples;

	char temp[1024];
	if (recordingRoomNoise) {

		for (int i = 0; i < samples; i++) {
			if (abs(recordingBuffer[i * 2]) > noiseFloor) {
				noiseFloor = abs(recordingBuffer[i * 2]);
			}
			if (abs(recordingBuffer[i * 2 + 1]) > noiseFloor) {
				noiseFloor = abs(recordingBuffer[i * 2 + 1]);
			}
		}

        noiseFloor *= 10;
        noiseFloor /= 9;

		sprintf(temp, "%s/room-noise.wav", filename);

	} else {
		sprintf(temp, "%s/segment-%04d.wav", filename, segmentNo);
#if 1
		while (
			(abs(recordingBuffer[firstSample * 2]) <= noiseFloor) &&
			(abs(recordingBuffer[firstSample * 2 + 1]) <= noiseFloor) &&
			(firstSample < lastSample)) {
				firstSample ++;
		}
		while (
			(abs(recordingBuffer[lastSample * 2]) <= noiseFloor) &&
			(abs(recordingBuffer[lastSample * 2 + 1]) <= noiseFloor) &&
			(lastSample > firstSample)) {
				lastSample --;
		}

        firstSample -= 4800;
        if (firstSample < 0) firstSample = 0;

        lastSample += 4800;
        if (lastSample > samples - 1) lastSample = samples - 1;
#endif
	}

	validSamples = lastSample - firstSample + 1;

	recordFd = open(temp, O_RDWR | O_CREAT, 0666);

	struct wav header;
	header.riff_chunkid = 0x46464952;
	header.riff_chunksize = validSamples * 2 * 2 + 36;
	header.riff_format = 0x45564157;

	header.fmt_chunkid = 0x20746d66;
	header.fmt_chunksize = 16;
	header.fmt_audioformat = 1;
	header.fmt_numchannels =  2;
	header.fmt_samplerate = 48000;
	header.fmt_byterate = 48000 * 2 * 2;
	header.fmt_blockalign = 2 * 2;
	header.fmt_bitspersample = 16;

	header.data_chunkid = 0x61746164;
	header.data_chunksize = validSamples * 2 * 2;
	write(recordFd, &header, sizeof(header));

	write(recordFd, &recordingBuffer[firstSample * 2], validSamples * 4);

	close(recordFd);
   // displaySummary();
	clearScreen();
	updateScreen();
	samples = 0;
	recordingRoomNoise = 0;
	recording = 0;

}

void doRecording() {

	if (recordingRoomNoise) {
		if (samples >= 48000 * 5) {
			stopRecording();
		}
	}

        int numSamples = snd_pcm_avail( alsa_handle );

	if (numSamples == 0) return;

	int samplesLeft = MAX_SAMPLES - samples;

	if (numSamples > samplesLeft) {
		numSamples = samplesLeft;
	}
	if (recording) {
		snd_pcm_readi( alsa_handle, (char *)&recordingBuffer[samples * 2], numSamples);
		samples += numSamples;
	} else {
		snd_pcm_readi( alsa_handle, (char *)&recordingBuffer[0], numSamples);
	}
	if (samples >= MAX_SAMPLES) {
		stopRecording();
	}
}

void undoRecording() {
	char temp[1024];
	sprintf(temp, "%s/segment-%4d.wav", filename, segmentNo);
	unlink(temp);
	if (segmentNo > 0) {
		segmentNo--;
	}
	clearScreen();
	updateScreen();
}

int addRoomNoise(int fd, int seconds, int16_t *roomNoiseSamples) {
    int len = 48000 * seconds;
    int maxsamp = 48000 * 5 - len;

    int pos = rand() % maxsamp;
    
	write(fd, &roomNoiseSamples[pos * 2], 48000 * 4 * seconds);
	return 48000 * seconds;
}

int appendFile(int fd, const char *fn) {
	int afd = open(fn, O_RDONLY);
	struct wav header;
	read(afd, &header, sizeof(header));
	read(afd, recordingBuffer, header.data_chunksize);
	write(fd, recordingBuffer, header.data_chunksize);
	close(afd);
	return header.data_chunksize / 4;
}

void combineSession() {
	clearScreen();
	text("Combining session...", 20, 20);
	updateScreen();

	char temp[1024];

	int16_t roomNoiseSamples[48000 * 5 * 2];

	sprintf(temp, "%s/room-noise.wav", filename);
	int fd = open(temp, O_RDONLY);
	lseek(fd, 44, SEEK_SET);
	read(fd, roomNoiseSamples, 48000 * 5 * 2 * 2);
	close(fd);

	sprintf(temp, "%s.wav", filename);
	int masterFd = open(temp, O_RDWR | O_CREAT, 0666);

	struct wav header;

	header.riff_chunkid = 0x46464952;
	header.riff_chunksize = 0; //samples * 2 * 2 + 36;
	header.riff_format = 0x45564157;

	header.fmt_chunkid = 0x20746d66;
	header.fmt_chunksize = 16;
	header.fmt_audioformat = 1;
	header.fmt_numchannels =  2;
	header.fmt_samplerate = 48000;
	header.fmt_byterate = 48000 * 2 * 2;
	header.fmt_blockalign = 2 * 2;
	header.fmt_bitspersample = 16;

	header.data_chunkid = 0x61746164;
	header.data_chunksize = 0; //samples * 2 * 2;

	write(masterFd, &header, sizeof(header));
	int nsamp = 0;

	nsamp += addRoomNoise(masterFd, 2, roomNoiseSamples);

	for (int i = 1; i <= segmentNo; i++) {
		sprintf(temp, "%s/segment-%04d.wav", filename, i);
		nsamp += appendFile(masterFd, temp);
		nsamp += addRoomNoise(masterFd, 1, roomNoiseSamples);
	}

	lseek(masterFd, 0, SEEK_SET);
	header.riff_chunksize = nsamp * 2 * 2 + 36;
	header.data_chunksize = nsamp * 2 * 2;
	write(masterFd, &header, sizeof(header));
	close(masterFd);

	clearScreen();
	text("Combining complete.", 20, 20);
	updateScreen();
}

void reopenSession() {
    int fd;
    segmentNo = 1;

    loadRoomNoise();

    char temp[1024];
    sprintf(temp, "%s/segment-%04d.wav", filename, segmentNo);
    while (access(temp, F_OK) != -1) {
        segmentNo++;
        sprintf(temp, "%s/segment-%04d.wav", filename, segmentNo);
    }
    segmentNo--;
}

int main (int argc, char *argv[]) {
    char alsa_device[30] = "hw:0";

    extern char *optarg;
    extern int optind, optopt;
    int errflg=0;
    int c;

    while ((c = getopt(argc, argv, "fd:n:")) != -1) {
        switch(c) {
            case 'd':
                strcpy(alsa_device,optarg);
                break;
            case 'n':
                strcpy(filename, optarg);
                break;
            case 'f':
		fullScreen++;
		break;
        }
    }

    if (filename[0] != 0) {
        char temp[1024];
        sprintf(temp, "%s/room-noise.wav", filename);
        int fd;
        if (access(temp, F_OK) != -1) {
            reopenSession();
        }
    } else {
        time_t t = time(NULL);
        sessionTime = localtime(&t);

        if (filename[0] == 0) {
            strftime(filename, 1024, "%Y%m%d-%H%M%S", sessionTime);
        }
    }

    alsa_handle = open_audiofd( alsa_device, 1, sample_rate, num_channels, period_size, num_periods);
    if( alsa_handle == 0 )
	exit(20);

    signal( SIGTERM, sigterm_handler );
    signal( SIGINT, sigterm_handler );

    initButtons();

	initSDL();
    SDL_Delay(100);
	updateScreen();
	SDL_Delay(100);
	updateScreen();
	SDL_Delay(100);
	updateScreen();
	SDL_Delay(100);


    while (quit == 0) {

    mapButtons();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_QUIT:
				quit = 1;
				break;
			case SDL_KEYDOWN:
				if (event.key.repeat == 0) {
					switch (event.key.keysym.sym) {
						case SDLK_c:
							if (!recording) {
								combineSession();
							}
							break;
						case SDLK_r:
							if (!recording) 
								startRecording();
							break;
						case SDLK_q:
							quit = 1;
							break;
						case SDLK_d:
							if (!recording)
								undoRecording();
							break;
                        case SDLK_b:
                            toggleBacklight();
                            break;
					}
				}
				break;
			case SDL_KEYUP:
				if (event.key.repeat == 0) {
					switch (event.key.keysym.sym) {
						case SDLK_n:
							if (!recording)
								recordRoomNoise();
							break;
						case SDLK_r:
							if (recording)
								stopRecording();
							break;
					}
				}
				break;
		}
	}

	doRecording();

	SDL_Delay(1);
    }

	if (recording) {
		stopRecording();
	}


	SDL_DestroyWindow(_window);

    exit (0);
}

