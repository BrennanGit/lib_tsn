// Copyright (c) 2016, XMOS Ltd, All rights reserved
#include <print.h>
#include <xccompat.h>
#include <xscope.h>
#include "audio_output_fifo.h"
#include "avb_1722_def.h"
#include "media_clock_client.h"

#define OUTPUT_DURING_LOCK 0
#define NOTIFICATION_PERIOD 250

// Volume is represented as a 2.30 signed fixed point number.
//    SIFFFFFF.FFFFFFFF.FFFFFFFF.FFFFFFFF
//
//    Clearly, apart from weird stuff, we should restrict the values to 0 -> 1, instead
//    of -2 to 2
#define MAX_VOLUME 0x40000000

void
audio_output_fifo_init(buffer_handle_t s0, unsigned index)
{
  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];

  s->state = DISABLED;
  s->dptr = START_OF_FIFO(s);
  s->wrptr = START_OF_FIFO(s);
  s->media_clock = -1;
  s->pending_init_notification = 0;
  s->last_notification_time = 0;
  s->volume = MAX_VOLUME;

  for( int ii = 0; ii < AUDIO_OUTPUT_FIFO_WORD_SIZE; ++ii ) s->fifo[ii] = s->buffer[ii];
}

void
disable_audio_output_fifo(buffer_handle_t s0, unsigned index)
{
  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];

  s->state = DISABLED;
  s->zero_flag = 1;
}

void
enable_audio_output_fifo(buffer_handle_t s0, unsigned index, int media_clock)
{
  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];

  s->state = ZEROING;
  s->dptr = START_OF_FIFO(s);
  s->wrptr = START_OF_FIFO(s);
  s->marker = (unsigned int**) 0;
  s->local_ts = 0;
  s->ptp_ts = 0;
  s->zero_marker = END_OF_FIFO(s)-1;
  s->zero_flag = 1;
  **s->zero_marker = 1;
  s->sample_count = 0;
  s->media_clock = media_clock;
  s->pending_init_notification = 1;
}


// 1722 thread
void audio_output_fifo_set_ptp_timestamp(buffer_handle_t s0,
                                         unsigned int index,
                                         unsigned int ptp_ts,
                                         unsigned sample_number)
{
  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];

  if (s->marker == 0) {
	unsigned int** new_marker = s->wrptr + sample_number;
	if (new_marker >= END_OF_FIFO(s)) new_marker -= AUDIO_OUTPUT_FIFO_WORD_SIZE;

	if (ptp_ts==0) ptp_ts = 1;
    s->ptp_ts = ptp_ts;
    s->local_ts = 0;
    s->marker = new_marker;
  }
}

void audio_output_fifo_pull
(
    unsigned int*   samples,
    buffer_handle_t s0,
    unsigned        index,
    unsigned int    timestamp
) {
    ofifo_t *unsafe s = (ofifo_t *unsafe)((struct output_finfo *unsafe)s0)->p_buffer[index];

    if( s->dptr != s->wrptr )
    {
        if (s->zero_flag)
            for( int ii = 0; ii < AVB_CHANNEL_COALESCENCE_FACTOR; ++ii ) samples[ii] = 0;
        else
            for( int ii = 0; ii < AVB_CHANNEL_COALESCENCE_FACTOR; ++ii ) samples[ii] = (*s->dptr)[ii];

        if( s->dptr == s->marker && s->local_ts == 0 ) {
            if( timestamp == 0 ) timestamp=1;
            s->local_ts = timestamp;
        }
        if( ++s->dptr == END_OF_FIFO(s)) s->dptr = START_OF_FIFO(s);
    }
    else {} // Underflow
}

// 1722 thread
void
audio_output_fifo_maintain(buffer_handle_t s0,
                           unsigned index,
                           chanend buf_ctl,
                           int *notified_buf_ctl)
{
  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];
  unsigned time_since_last_notification;

  if (s->pending_init_notification && !(*notified_buf_ctl)) {
    notify_buf_ctl_of_new_stream(buf_ctl, (int)s); // TODO: This can pass the index
    *notified_buf_ctl = 1;
    s->pending_init_notification = 0;
  }

  switch (s->state)
    {
    case DISABLED:
      break;
    case ZEROING:
      if (**s->zero_marker == 0) {
        // we have zero-ed the entire fifo
        // set the wrptr so that the fifo size is 1/2 of the buffer size
        int buf_len = (END_OF_FIFO(s) - START_OF_FIFO(s));
        unsigned int** new_wrptr;

        new_wrptr = s->dptr + ((buf_len>>1));
        while (new_wrptr >= END_OF_FIFO(s))
          new_wrptr -= buf_len;

        s->wrptr = new_wrptr;
        s->state = LOCKING;
        s->local_ts = 0;
        s->ptp_ts = 0;
        s->marker = 0;
#if (OUTPUT_DURING_LOCK == 0)
        s->zero_flag = 1;
#endif
      }
      break;
    case LOCKING:
    case LOCKED:
      time_since_last_notification =
        (signed) s->sample_count - (signed) s->last_notification_time;
      if (s->ptp_ts != 0 &&
          s->local_ts != 0 &&
          !(*notified_buf_ctl)
          &&
          (s->last_notification_time == 0 ||
           time_since_last_notification > NOTIFICATION_PERIOD)
          )
        {
          notify_buf_ctl_of_info(buf_ctl, (int)s); // TODO: FIXME
          *notified_buf_ctl = 1;
          s->last_notification_time = s->sample_count;
        }
      break;
    }
}

// 1722 thread
void audio_output_fifo_push
(
    buffer_handle_t s0,
    unsigned        index,
    unsigned int*   sample_ptr,
    int             channel_count,
    int             cycle_count
) {
    ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];

    #ifdef AVB_AUDIO_OUTPUT_FIFO_VOLUME_CONTROL
    int volume = (s->state == ZEROING) ? 0 : s->volume;
    #else
    int volume = (s->state == ZEROING) ? 0 : 1;
    #endif

    for( int ii = 0; ii < cycle_count; ++ii )
    {
        unsigned int** new_wrptr = s->wrptr + 1;
        if( new_wrptr == END_OF_FIFO(s) ) new_wrptr = START_OF_FIFO(s);
        if( new_wrptr != s->dptr )
        {
            for( int jj = 0; jj < AVB_CHANNEL_COALESCENCE_FACTOR; ++jj )
            {
                unsigned int sample = __builtin_bswap32( sample_ptr[jj] );
                #ifndef AVBa_1722_FORMAT_SAF
                sample = sample << 8;
                #endif
                #ifdef AUDIO_OUTPUT_FIFO_VOLUME_CONTROL
                {
                    int ah, al;
                    asm("maccs %0,%1,%2,%3":"=r"(ah),"=r"(al):"r"(xx),"r"(volume),"0"(0),"1"(0));
                    sample = (ah >> 6) & 0xffffff;
                }
                #else
                sample *= volume;
                #endif
                (*s->wrptr)[jj] = sample;
            }
            s->wrptr = new_wrptr;
        }
        else {} // Overflow
        sample_ptr += channel_count;
    }
    s->sample_count += cycle_count;
}

// 1722 thread
void
audio_output_fifo_handle_buf_ctl(chanend buf_ctl,
                                 buffer_handle_t s0,
                                 unsigned index,
                                 int *buf_ctl_notified,
                                 timer tmr)
{
  int cmd;
  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];
  cmd = get_buf_ctl_cmd(buf_ctl);
  switch (cmd)
    {
    case BUF_CTL_REQUEST_INFO: {
      send_buf_ctl_info(buf_ctl,
                        s->state == LOCKED,
                        s->ptp_ts,
                        s->local_ts,
                        s->dptr - START_OF_FIFO(s),
                        s->wrptr - START_OF_FIFO(s),
                        tmr);
      s->ptp_ts = 0;
      s->local_ts = 0;
      s->marker = (unsigned int**) 0;
      break;
    }
    case BUF_CTL_REQUEST_NEW_STREAM_INFO: {
      send_buf_ctl_new_stream_info(buf_ctl,
                                   s->media_clock);
      buf_ctl_ack(buf_ctl);
      *buf_ctl_notified = 0;
      break;
    }
    case BUF_CTL_ADJUST_FILL:
      {
        int adjust;
        unsigned int** new_wrptr;
        adjust = get_buf_ctl_adjust(buf_ctl);

        new_wrptr = s->wrptr - adjust;
        while (new_wrptr < START_OF_FIFO(s))
          new_wrptr += (END_OF_FIFO(s) - START_OF_FIFO(s));

        while (new_wrptr >= END_OF_FIFO(s))
          new_wrptr -= (END_OF_FIFO(s) - START_OF_FIFO(s));

        s->wrptr = new_wrptr;
      }
      s->state = LOCKED;
      s->zero_flag = 0;
      s->ptp_ts = 0;
      s->local_ts = 0;
      s->marker = (unsigned int**) 0;
      buf_ctl_ack(buf_ctl);
      *buf_ctl_notified = 0;
      break;
    case BUF_CTL_RESET:
      s->state = ZEROING;
      if (s->wrptr == START_OF_FIFO(s))
        s->zero_marker = END_OF_FIFO(s) - 1;
      else
        s->zero_marker = s->wrptr - 1;
      s->zero_flag = 1;
      **s->zero_marker = 1;
      buf_ctl_ack(buf_ctl);
      *buf_ctl_notified = 0;
      break;
    case BUF_CTL_ACK:
      buf_ctl_ack(buf_ctl);
      *buf_ctl_notified = 0;
      break;
    default:
      break;
    }
}

void
audio_output_fifo_set_volume(buffer_handle_t s0,
                             unsigned index,
                             unsigned int volume)
{
	  ofifo_t *s = (ofifo_t *)((struct output_finfo *)s0)->p_buffer[index];
	  s->volume = volume;
}
