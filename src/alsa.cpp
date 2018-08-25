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
#include <alsa/asoundlib.h>

// Maximum 60 seconds of recording per segment
#define MAX_SAMPLES (48000 * 60)

snd_pcm_uframes_t real_buffer_size;
snd_pcm_uframes_t real_period_size;

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
	/* set the stream rate */
	rrate = rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
	if (err < 0) {
		printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
		return err;
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

snd_pcm_t *open_audiofd( char *device_name, int capture, int rate, int channels, int period, int nperiods ) {
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
