#include "radiobuttons.h"
#include "drawingarea.h"
#include "Scales.h"
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <iostream>
#include <termios.h>
#include <fcntl.h>
#include <asoundlib.h>
#include <fftw3.h>
using namespace std;

#define DEBUG
#define NO_MARKER
#define NFFT                    4096
#define WINDOW_XSIZE            1320
#define WINDOW_YSIZE             500
#define AREA1_XSIZE               99
#define AREA1_YSIZE               50
#define WATERFALL_XSIZE          512
#define WATERFALL_YSIZE          768
#define WAVEFORM_LEN             128
#define BAUDRATE                B19200
#define TIMEOUT_VALUE           100
#define END_OF_COMMAND          0xfd

extern int fd;
extern unsigned int rate;
extern unsigned int channels;	/* count of channels */
extern int byte_per_sample;	/* 16 bit format */
extern unsigned int buffer_time;	/* ring buffer length in us */
extern unsigned int period_time;	/* period time in us */
extern int resample;	/* disable resample */
extern int period_event;	/* produce poll event after each period */
extern double audio_signal[NFFT];
extern double audio_signal_ffted[NFFT];
extern double fft_window[NFFT];
extern int cw_pitch;
extern int iwater;
extern int nsamples;
extern double bin_size, waterfall_scale_x;
extern double amax, amin;
extern long int ifreq_in_hz;
extern int s_meter;
extern int operating_mode;	/* CW=03, CW-REV=07, LSB=00, USB=01 */
extern int dsp_filter;		/* FIL1=01, FIL2=02, FIL3=03 */
extern snd_pcm_sframes_t buffer_size;
extern snd_pcm_sframes_t period_size;
extern snd_pcm_t *handle;
extern snd_pcm_hw_params_t *hwparams;
extern snd_pcm_sw_params_t *swparams;
extern double *in;
extern fftw_complex *out;
extern fftw_plan p;
extern int flag_togo1, flag_togo2;

int send_command (unsigned char *partial_command);
int receive_fb ();

int
rig_init_serial (char *serial_port)
{
  struct termios tio;

  std::cout << "rig_init_serial: begin ... \n";

  fd = open (serial_port, O_RDWR | O_NOCTTY);
  if (fd < 0)
    {
      std::cout << "error: can not open myrig. \n";
      return 1;
    }

  memset (&tio, 0, sizeof (tio));
  tio.c_cflag = CS8 | CLOCAL | CREAD;
  tio.c_cc[VEOL] = 0xfd;	/* IC-7410 postamble */
  tio.c_lflag = 0;		/* non canonical mode */
  tio.c_cc[VTIME] = 0;		/* non canonical mode */
  tio.c_cc[VMIN] = 1;		/* non canonical mode */

  tio.c_iflag = IGNPAR | ICRNL;
  cfsetispeed (&tio, BAUDRATE);
  cfsetospeed (&tio, BAUDRATE);
  tcsetattr (fd, TCSANOW, &tio);

  return 0;
}


int
mystrlen (unsigned char *string)
{
  unsigned char *t;
  for (t = string; *t != END_OF_COMMAND; t++)
    {;
    }
  return (t - string) + 1;	/* +1 to include EOC */
}

int
mystrcmp (unsigned char *string1, unsigned char *string2)
{
  unsigned char *t1;
  unsigned char *t2;

  for (t1 = string1, t2 = string2; *t1 == *t2 && *t1 != END_OF_COMMAND;
       t1++, t2++)
    {;
    }
  return *t1 - *t2;
}

int
myread (unsigned char *myresponse)
{
  int res = 0;
  unsigned char mybuf[256], *p;

  p = myresponse;
  do
    {
      int t = read (fd, mybuf, 1);
      if (t != 1)
	{
	  fprintf (stderr, "error in read \n");
	  return -1;
	}
      *p++ = mybuf[0];
      res++;
    }
  while (mybuf[0] != 0xfd);

  return res;
}

int
receive_fb (void)
{
  unsigned char response[256];
  unsigned char fb_message[6] = { 0xfe, 0xfe, 0xe0, 0x80, 0xfb, 0xfd };
  int n;

  n = myread (response);	/* get echo back */

  if (mystrcmp (response, fb_message) != 0)
    {
      fprintf (stderr, "*** error *** not a FB message. ");
      for (int i = 0; i < n; i++)
	{
	  fprintf (stderr, "%02x ", response[i]);
	}
      fprintf (stderr, "\n");
      return false;
    }

  return true;
}

int send_command (unsigned char *partial_command)
{
  int n_partial, n_command;
  int n_echoback;
  unsigned char command[256] = { 0xfe, 0xfe, 0x80, 0xe0 };	/* preamble */
  unsigned char echoback[256];

  n_partial = mystrlen (partial_command);
  n_command = n_partial + 4;	/* add preamble(4) */
  for (int i = 0; i < n_partial; i++)
    {
      command[4 + i] = partial_command[i];
    }
  command[n_command - 1] = 0xfd;

  write (fd, command, n_command);
  n_echoback = myread (echoback);

#ifdef DEBUG
  unsigned char *s;
  s = command;
  fprintf (stderr, "send_command: n_command  = %2d, command  = ", n_command);
  for (int i = 0; i < n_command; i++)
    {
      fprintf (stderr, "[%02x] ", *s++);
    }
  fprintf (stderr, "\n");
  fprintf (stderr, "              n_echoback = %2d, echoback = ", n_echoback);
  s = echoback;
  for (int i = 0; i < n_echoback; i++)
    {
      fprintf (stderr, "[%02x] ", *s++);
    }
  fprintf (stderr, "\n");
#endif

  if ((n_echoback != n_command) || (mystrcmp (command, echoback) != 0))
    {
      fprintf (stderr,
	       "   send_command():  *** error *** echoback does not much. \n");
      return false;
    }

  return true;
}

int
send_commandx (const vector < unsigned char >&partial_command)
{
  int n_partial, n_command;
  int n_echoback;
  unsigned char command[256] = { 0xfe, 0xfe, 0x80, 0xe0 };	/* preamble */
  unsigned char echoback[256];

  n_partial = partial_command.size ();
  cout << "send_commandx: n_partial = " << n_partial << endl;

  n_command = 4 + n_partial + 1;	/* add preamble(4) and EOC(1) */
  for (int i = 0; i < n_partial; i++)
    {
      command[4 + i] = partial_command.at (i);
    }
  command[n_command - 1] = 0xfd;	/* End_Of_Command */

  write (fd, command, n_command);
  n_echoback = myread (echoback);

#ifdef DEBUG
  unsigned char *s;
  s = command;
  fprintf (stderr, "send_command: n_command  = %2d, command  = ", n_command);
  for (int i = 0; i < n_command; i++)
    {
      fprintf (stderr, "[%02x] ", *s++);
    }
  fprintf (stderr, "\n");
  fprintf (stderr, "              n_echoback = %2d, echoback = ", n_echoback);
  s = echoback;
  for (int i = 0; i < n_echoback; i++)
    {
      fprintf (stderr, "[%02x] ", *s++);
    }
  fprintf (stderr, "\n");
#endif

  if ((n_echoback != n_command) || (mystrcmp (command, echoback) != 0))
    {
      fprintf (stderr,
   	       "   send_commandx():  *** error *** echoback does not much. \n");
      return false;
    }

  return true;
}

void
set_operating_mode (void)
{
  static unsigned char command1[4] = { 0x06, 0x03, 0x01, 0xfd };

  command1[1] = operating_mode;
  command1[2] = dsp_filter;;
  send_command (command1);
  receive_fb ();
}

void
set_operating_modex (void)
{
  vector < unsigned char >command1 = { 0x06, 0x03, 0x01 };	/* without EOC */

  command1[1] = operating_mode;
  command1[2] = dsp_filter;
  send_commandx (command1);
  receive_fb ();
}

void
myfunc (int index)
{
  static vector < unsigned char >command1x = { 0x06, 0x03, 0x01 };	/* OP MODE & FIL SET */
  static vector < unsigned char >command6x = { 0x16, 0x55, 0x00 };	/* IF FIL1/2/3 */
  static vector < unsigned char >command19x = { 0x16, 0x56, 0x00 };	/* DSP SHARP/SOFT */
  static vector < unsigned char >command9x = { 0x16, 0x02, 0x00 };	/* PRE-AMP OFF/1/2 */
  static vector < unsigned char >command12x = { 0x11, 0x00 };	/* ATT OFF  */
  static vector < unsigned char >command16x = { 0x16, 0x12, 0x01 };	/* AGC FAST  */
  static vector < unsigned char >command14x = { 0x12, 0x00 };	/* ANT 1/2 */
  static vector < unsigned char >command3x = { 0x16, 0x47, 0x00 };	/* BKIN OFF/SEMI/FULL  */

  std::cout << "myfunc: index = " << index << std::endl;
  if (index >= 0 && index <= 7)
    {				/* OP MODE */
      switch (index)
	{
	case 0:		/* CW */
	  operating_mode = 0x03;
	  break;
	case 1:		/* CW-R */
	  operating_mode = 0x07;
	  break;
	case 2:		/* RTTY */
	  operating_mode = 0x04;
	  break;
	case 3:		/* RTTY-R */
	  operating_mode = 0x08;
	  break;
	case 4:		/* LSB */
	  operating_mode = 0x00;
	  break;
	case 5:		/* USB */
	  operating_mode = 0x01;
	  break;
	case 6:		/* AM */
	  operating_mode = 0x02;
	  break;
	case 7:		/* FM */
	  operating_mode = 0x05;
	  break;
	}
      command1x[1] = operating_mode;
      send_commandx (command1x);
      receive_fb ();
      return;
    }

  if (index >= 8 && index <= 10)
    {				/* DAP SHARP/SOFT */
      command1x[2] = (index - 8) + 1;	/* start from 1 */
      send_commandx (command1x);
      receive_fb ();
      return;
    }

  if (index >= 11 && index <= 12)
    {				/* DAP SHARP/SOFT */
      command19x[2] = index - 11;
      send_commandx (command19x);
      receive_fb ();
      return;
    }

  if (index >= 13 && index <= 15)
    {				/* IF FIL 1/2/3 */
      command6x[2] = index - 13;
      send_commandx (command6x);
      receive_fb ();
      return;
    }

  if (index >= 16 && index <= 18)
    {				/* PRE-AMP OFF/1/2 */
      command9x[2] = index - 16;
      send_commandx (command9x);
      receive_fb ();
      return;
    }

  if (index >= 19 && index <= 20)
    {				/* PRE-AMP OFF/1/2 */
      command12x[1] = (index - 19) * 0x20;
      send_commandx (command12x);
      receive_fb ();
      return;
    }

  if (index >= 21 && index <= 23)
    {				/* AGC F/M/S */
      command16x[2] = (index - 21) + 1;	/* start from 1 */
      send_commandx (command16x);
      receive_fb ();
      return;
    }

  if (index >= 24 && index <= 25)
    {				/* ANT 1/2 */
      command14x[1] = index - 24;
      send_commandx (command14x);
      receive_fb ();
      return;
    }

  if (index >= 26 && index <= 28)
    {				/* BKIN OFF/SEMI/FULL */
      command3x[2] = index - 26;
      send_commandx (command3x);
      receive_fb ();
      return;
    }

}

static void
async_callback (snd_async_handler_t * ahandler)
{
  static int icount = 0;
  cout << "async_callback() is called. icount = " << icount++ << "\n";

  flag_togo1 = 1; /* to activate DrawArea::on_draw() */
  flag_togo2 = 1; /* to activate Waterfall::on_draw() */

  snd_pcm_t *handle = snd_async_handler_get_pcm (ahandler);
  signed short *samples = snd_async_handler_get_callback_private (ahandler);
  snd_pcm_sframes_t avail;
  int err;

  avail = snd_pcm_avail_update (handle);
  cout << "avail = " << avail << endl;
  cout << "period_size = " << period_size << endl;

  while (avail >= period_size)
    {
      err = snd_pcm_readi (handle, samples, period_size);

      if (err < 0)
	{
	  fprintf (stderr, "Write error: %s\n", snd_strerror (err));
	  exit (EXIT_FAILURE);
	}
      if (err != period_size)
	{
	  fprintf (stderr, "Write error: written %i expected %li\n", err,
		   period_size);
	  exit (EXIT_FAILURE);
	}

      for (int i = 0; i < NFFT; i++)
	{			/* NFFT=period_size */
	  audio_signal[i] = samples[i];
#ifdef MARKER
	  audio_signal[i] +=
	    16384.0 * (0.25 * sin (2.0 * 3.14 * 600.0 * (double) i / rate)
		       + 0.25 * sin (2.0 * 3.14 * 500.0 * (double) i / rate)
		       + 0.25 * sin (2.0 * 3.14 * 450.0 * (double) i / rate)
		       + 0.25 * sin (2.0 * 3.14 * 750.0 * (double) i / rate));
#endif

	}

      avail = snd_pcm_avail_update (handle);
    }
    cout << "async_call back end.. \n";
}

static int
async_loop (snd_pcm_t * handle, signed short *samples)
{
  snd_async_handler_t *ahandler;
  int err;
  cout << "async_loop begin.. \n";
  err = snd_async_add_pcm_handler (&ahandler, handle, async_callback, samples);
  if (err < 0)
    {
      fprintf (stderr, "Unable to register async handler\n");
      exit (EXIT_FAILURE);
    }

  if (snd_pcm_state (handle) == SND_PCM_STATE_PREPARED)
    {
      err = snd_pcm_start (handle);
      if (err < 0)
	{
	  fprintf (stderr, "Start error: %s\n", snd_strerror (err));
	  exit (EXIT_FAILURE);
	}
    }
  cout << "async_loop end.. \n";
  return 0;
}

void
set_freq (long int ifreq_in_hz)
{
  fprintf (stderr, "freq set to %12.3f [kHz] \n",
	   (double) ifreq_in_hz / 1000.0);
  static unsigned char command1[7] =
    { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd };
  long int ifreq_wrk;
  int idigit[8];

  ifreq_wrk = ifreq_in_hz;

  for (int i = 0; i < 8; i++)
    {
      idigit[i] = ifreq_wrk % 10;
      ifreq_wrk /= 10;
    }
  command1[1] = 16 * idigit[1] + idigit[0];
  command1[2] = 16 * idigit[3] + idigit[2];
  command1[3] = 16 * idigit[5] + idigit[4];
  command1[4] = 16 * idigit[7] + idigit[6];
  send_command (command1);
  receive_fb ();
}

int colormap_r (double tmp)
{
  double val;
  if (tmp < 0.50) {
    val = 0.0;
  } else if (tmp > 0.75) {
    val = 1.0;
  } else {
   val = 4.0 * tmp - 2.0;
  }
  return (int) (255.0*val);
}

int colormap_g (double tmp)
{
  double val;
  if (tmp < 0.25) {
  val = 4.0 * tmp;
  } else if (tmp > 0.75) {
  val = -4.0 * tmp + 4.0;
  } else {
  val =  1.0;
  }
  return (int) (255.0*val);
}

int colormap_b (double tmp)
{
  double val;
  if (tmp < 0.25) {
  val = 1.0;
  } else if (tmp > 0.50) {
  val = 0.0;
  } else {
  val = -4.0 * tmp + 2.0;
  }
  return (int) (255.0*val);
}

static int
set_hwparams (snd_pcm_t * handle, snd_pcm_hw_params_t * params)
{
  unsigned int rrate;
  snd_pcm_uframes_t size;
  int err, dir;
  std::cout << "set_hwparams:    begin... \n";

  /* choose all parameters */
  err = snd_pcm_hw_params_any (handle, params);
  if (err < 0)
    {
      fprintf (stderr,
	       "Broken configuration for playback: no configurations available: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* set hardware resampling disabled */
  err = snd_pcm_hw_params_set_rate_resample (handle, params, resample);
  if (err < 0)
    {
      fprintf (stderr, "Resampling setup failed for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* set the interleaved read/write format */
  err =
    snd_pcm_hw_params_set_access (handle, params,
				  SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0)
    {
      fprintf (stderr, "Access type not available for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* set the sample format */
  err = snd_pcm_hw_params_set_format (handle, params, SND_PCM_FORMAT_S16);
  if (err < 0)
    {
      fprintf (stderr, "Sample format not available for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* set the count of channels */
  err = snd_pcm_hw_params_set_channels (handle, params, channels);
  if (err < 0)
    {
      fprintf (stderr,
	       "Channels count (%i) not available for playbacks: %s\n",
	       channels, snd_strerror (err));
      return err;
    }

  /* set the stream rate */
  rrate = rate;
  err = snd_pcm_hw_params_set_rate_near (handle, params, &rrate, 0);
  if (err < 0)
    {
      fprintf (stderr, "Rate %iHz not available for playback: %s\n", rate,
	       snd_strerror (err));
      return err;
    }
  if (rrate != rate)
    {
      fprintf (stderr, "Rate doesn't match (requested %iHz, get %iHz)\n",
	       rate, err);
      return -EINVAL;
    }

  /* set the buffer time */
  err =
    snd_pcm_hw_params_set_buffer_time_near (handle, params, &buffer_time,
					    &dir);
  if (err < 0)
    {
      fprintf (stderr, "Unable to set buffer time %i for playback: %s\n",
	       buffer_time, snd_strerror (err));
      return err;
    }
  fprintf (stderr, "buffer_time = %8d, dir   = %d \n", buffer_time, dir);

  err = snd_pcm_hw_params_get_buffer_size (params, &size);
  if (err < 0)
    {
      fprintf (stderr, "Unable to get buffer size for playback: %s\n",
	       snd_strerror (err));
      return err;
    }
  buffer_size = size;
  fprintf (stderr, "buffer_size = %8d             \n", (int) buffer_size);

  /* set the period time */
  err =
    snd_pcm_hw_params_set_period_time_near (handle, params, &period_time,
					    &dir);
  if (err < 0)
    {
      fprintf (stderr, "Unable to set period time %i for playback: %s\n",
	       period_time, snd_strerror (err));
      return err;
    }
  fprintf (stderr, "period_time = %8d, dir   = %d \n", period_time, dir);

  err = snd_pcm_hw_params_get_period_size (params, &size, &dir);
  if (err < 0)
    {
      fprintf (stderr, "Unable to get period size for playback: %s\n",
	       snd_strerror (err));
      return err;
    }
  period_size = size;
  fprintf (stderr, "period_size = %8d, dir   = %d \n", (int) period_size,
	   dir);

  if (period_size < NFFT)
    {
      fprintf (stderr,
	       "error: period_size = %8d, but less than NFFT  = %d \n",
	       (int) period_size, NFFT);
      exit (1);
    }

  /* write the parameters to device */
  err = snd_pcm_hw_params (handle, params);
  if (err < 0)
    {
      fprintf (stderr, "Unable to set hw params for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  return 0;
}

static int
set_swparams (snd_pcm_t * handle, snd_pcm_sw_params_t * swparams)
{
  int err;
  std::cout << "set_swparams:    begin... \n";

  /* get the current swparams */
  err = snd_pcm_sw_params_current (handle, swparams);
  if (err < 0)
    {
      fprintf (stderr,
	       "Unable to determine current swparams for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* start the transfer when the buffer is almost full: */
  /* (buffer_size / avail_min) * avail_min */
  err =
    snd_pcm_sw_params_set_start_threshold (handle, swparams,
					   (buffer_size / period_size) *
					   period_size);
  if (err < 0)
    {
      fprintf (stderr,
	       "Unable to set start threshold mode for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* allow the transfer when at least period_size samples can be processed */
  /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
  err =
    snd_pcm_sw_params_set_avail_min (handle, swparams,
				     period_event ? buffer_size :
				     period_size);
  if (err < 0)
    {
      fprintf (stderr, "Unable to set avail min for playback: %s\n",
	       snd_strerror (err));
      return err;
    }

  /* enable period events when requested */
  if (period_event)
    {
      err = snd_pcm_sw_params_set_period_event (handle, swparams, 1);
      if (err < 0)
	{
	  fprintf (stderr, "Unable to set period event: %s\n",
		   snd_strerror (err));
	  return err;
	}
    }

  /* write the parameters to the playback device */
  err = snd_pcm_sw_params (handle, swparams);
  if (err < 0)
    {
      fprintf (stderr, "Unable to set sw params for playback: %s\n",
	       snd_strerror (err));
      return err;
    }
  return 0;
}

int
rig_init_sound (char *sound_device)
{

  int err = 0;

  std::cout << "rig_init_sound:  begin ... \n";

  snd_pcm_hw_params_alloca (&hwparams);
  snd_pcm_sw_params_alloca (&swparams);

  std::cout << " rate = " << rate << " Hz, " << channels << " channel(s). \n";

  if ((err =
       snd_pcm_open (&handle, sound_device, SND_PCM_STREAM_CAPTURE, 0)) < 0)
    {
      fprintf (stderr, "Capture open error: %s\n", snd_strerror (err));
      return 0;
    }

  if ((err = set_hwparams (handle, hwparams)) < 0)
    {
      fprintf (stderr, "Setting of hwparams failed: %s\n",
	       snd_strerror (err));
      exit (EXIT_FAILURE);
    }

  if ((err = set_swparams (handle, swparams)) < 0)
    {
      fprintf (stderr, "Setting of swparams failed: %s\n",
	       snd_strerror (err));
      exit (EXIT_FAILURE);
    }

  nsamples = period_size * channels * byte_per_sample;
  fprintf (stderr, "nsamples = %d \n", nsamples);

//static  signed short *samples;
//  samples = malloc (nsamples);

  static signed short samples[99999];
  if (samples == NULL)
    {
      fprintf (stderr, "No enough memory\n");
      exit (EXIT_FAILURE);
    }
  cout << "going to async_loop() from rig_init_sound() \n";
  err = async_loop (handle, samples);
  cout << "returned from async_loop \n";

  return 0;
}

void
set_cw_speed (int wpm)
{
  static unsigned char command1[5] = { 0x14, 0x0c, 0x00, 0x32, 0xfd };
  int iii, i100, i10, i1;

  if (wpm < 6)
    wpm = 6;
  if (wpm > 48)
    wpm = 48;
  iii = 255 * (wpm - 6) / (48 - 6);
  i100 = iii / 100;
  i10 = (iii - 100 * i100) / 10;
  i1 = iii % 10;
//  fprintf(stderr, "wpm changed %d %d %d %d \n", wpm, i100, i10, i1);
  command1[2] = i100;
  command1[3] = 16 * i10 + i1;
  send_command (command1);
  receive_fb ();
}

void
set_tx_power (int txp)
{
  static unsigned char command1[5] = { 0x14, 0x0a, 0x00, 0x32, 0xfd };
  int iii, i100, i10, i1;

  if (txp < 2)
    txp = 2;
  if (txp > 100)
    txp = 100;
  iii = 255.0 * (txp - 2) / 100.0;
  i100 = iii / 100;
  i10 = (iii - 100 * i100) / 10;
  i1 = iii % 10;
//  fprintf(stderr, "txpower changed %d %d %d %d \n", txpower, i100, i10, i1);
  command1[2] = i100;
  command1[3] = 16 * i10 + i1;
  send_command (command1);
  receive_fb ();
}

bool
myclock ()
{
  static unsigned char command1[2] = { 0x03, 0xfd };
  static unsigned char command2[3] = { 0x15, 0x02, 0xfd };
  static unsigned char command3[2] = { 0x04, 0xfd };
  char string[256];
  unsigned char buf[255];
  int res;

  cout << "myclock(): begin.. \n";

/* read operating mode, response in char[5]-char[6] */

//  send_command (command3);
//  res = myread (buf);
//  if (res != 8)
//    {
//      fprintf (stderr, "operating mode response is wrong! \n");
//    }
//  operating_mode = buf[5];
//  dsp_filter = buf[6];
//  if (operating_mode == 0x03)
//    {				/* CW */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[0][0]),
//				    TRUE);
//    }
//  if (operating_mode == 0x07)Hi there!
//    {				/* CW-R */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[0][1]),
//				    TRUE);
//    }
//  if (operating_mode == 0x00)
//    {				/* LSB */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[0][2]),
//				    TRUE);
//    }
//  if (operating_mode == 0x01)
//    {				/* USB */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[0][3]),
//				    TRUE);
//    }
//  if (dsp_filter == 0x01)
//    {				/* FIL1 */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[6][0]),
//				    TRUE);
//    }
//  if (dsp_filter == 0x02)
//    {				/* FIL2 */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[6][1]),
//				    TRUE);
//    }
//  if (dsp_filter == 0x03)
//    {				/* FIL3 */
//      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button2dim[6][2]),
//				    TRUE);
//    }

/* freq response in char[8]-char[5] */
  cout << "myclock(): going to send_command() \n";
  send_command (command1);
  cout << "myclock(): going to myread() \n";
  res = myread (buf);
  cout << "myclock(): returned from myread() \n";

#ifdef DEBUG
  unsigned char *s;
  fprintf (stderr, "response for frequncy read, res = %2d : ", res);
  s = buf;
  for (int i = 0; i < res; i++)
    {
      fprintf (stderr, "[%02x] ", *s++);
    }
  fprintf (stderr, "\n");
#endif

  if (res != 11)
    {
      fprintf (stderr, "frequency response is wrong! \n");
    }
  sprintf (string, "%02x%02x%02x%02x", buf[8], buf[7], buf[6], buf[5]);
  ifreq_in_hz = atoi (string);
  cout << "ifreq_in_hz = " << ifreq_in_hz << endl;

/* S-meter response in char[6]-char[5] */

  send_command (command2);
  res = myread (buf);

  if (res != 9)
    {
      fprintf (stderr, "S-meter response is wrong! \n");
    }

  sprintf (string, "%02x%02x", buf[6], buf[7]);
  s_meter = atoi (string);
  cout << "s_meter = " << s_meter << endl;

  cout << "myclock(): end.. \n";

  return true;
}

