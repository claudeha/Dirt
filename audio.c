#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include <assert.h>
#include <dirent.h>
#include <lo/lo.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "thpool.h"

#ifdef JACK
#include "jack.h"
#elif PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/gccmacro.h>
#else

#include "portaudio.h"

#ifdef __linux
#include <pa_linux_alsa.h>
#endif

PaStream *stream;

#define PA_FRAMES_PER_BUFFER 1024

#endif

#include "audio.h"
#include "server.h"
#include "pitch.h"

#define HALF_PI 1.5707963267948966f

t_line* delays;
float line_feedback_delay;

pthread_mutex_t queue_loading_lock;
pthread_mutex_t queue_waiting_lock;
pthread_mutex_t mutex_sounds;

t_sound *loading = NULL;
t_sound *waiting = NULL;
t_sound *playing = NULL;

t_sound sounds[MAX_SOUNDS];
int playing_n = 0;

double epochOffset = 0;
float starttime = 0;

#ifdef JACK
jack_client_t *jack_client = NULL;
#endif
float compression_speed = -1;

float delay_time = 0.1;
float delay_feedback = 0.7;

bool use_dirty_compressor = false;
bool use_late_trigger = false;
bool use_shape_gain_comp = false;

thpool_t* read_file_pool;

const char* sampleroot;

void queue_add(t_sound **queue, t_sound *new);
void init_sound(t_sound *sound);
int queue_size(t_sound *queue);

#ifdef SEND_RMS
static t_rms rms[MAX_ORBIT*2];
#endif

static int is_sample_loading(const char* samplename) {
  int result = 0;
  t_sound *p = loading;
  while (p != NULL) {
    if (strcmp(samplename, p->samplename) == 0) {
      result = 1;
      break;
    }
    p = p->next;
  }
  return(result);
}

static void mark_as_loading(t_sound* sound) {
  if (loading) {
    sound->prev = NULL;
    sound->next = loading;
    loading->prev = sound;
  }
  else {
    sound->prev = NULL;
    sound->next = NULL;
  }
  loading = sound;
}

static void unmark_as_loading(const char* samplename, int success) {
  pthread_mutex_lock(&queue_loading_lock);
  t_sound *p = loading;
  while (p != NULL) {
    t_sound *next = p->next;
    if (strcmp(samplename, p->samplename) == 0) {
      if (p->prev == NULL) {
	loading = p->next;
	
	if (loading  != NULL) {
	  loading->prev = NULL;
	}
      }
      else {
	p->prev->next = p->next;
	if (p->next) {
	  p->next->prev = p->prev;
	}
      }
    
      p->prev = NULL;
      p->next = NULL;
      if (success) {
	pthread_mutex_lock(&queue_waiting_lock);
	queue_add(&waiting, p);
	pthread_mutex_unlock(&queue_waiting_lock);
      }
      else {
	p->active = 0;
      }
    }
    p = next;
  }
  pthread_mutex_unlock(&queue_loading_lock);
}

static void reset_sound(t_sound* s);

void *read_file_func(void* new) {
  t_sound* sound = new;
  t_sample *sample = file_get(sound->samplename, sampleroot);
  if (sample != NULL) {
    sound->sample = sample;
    init_sound(sound);
  }
  unmark_as_loading(sound->samplename, sample != NULL);
  return NULL;
}

int queue_size(t_sound *queue) {
  int result = 0;
  while (queue != NULL) {
    result++;
    queue = queue->next;
    if (result > 4096) {
      printf("whoops, big queue %d\n", result);
      break;
    }
  }
  return(result);
}

void queue_add(t_sound **queue, t_sound *new) {
  int added = 0;
  assert(new->next != new);
  assert(new->prev != new);
  if (*queue == NULL) {
    *queue = new;
    added++;
  }
  else {
    t_sound *tmp = *queue;
    assert(tmp->prev == NULL);

    int i =0;
    while (1) {
      if (tmp->startT > new->startT) {
        // insert in front of later event
        new->next = tmp;
        new->prev = tmp->prev;
        if (new->prev != NULL) {
          new->prev->next = new;
        }
        else {
          *queue = new;
        }
        tmp->prev = new;

        added++;
        break;
      }

      if (tmp->next == NULL) {
        // add to end of queue
        tmp->next = new;
        new->prev = tmp;
        added++;
        break;
      }
      ++i;
      tmp = tmp->next;
    }
  }

  assert(added == 1);
}


void queue_remove(t_sound **queue, t_sound *old) {
  // printf("played %d\n", old->played);
  if (old->prev == NULL) {
    *queue = old->next;
    if (*queue  != NULL) {
      (*queue)->prev = NULL;
    }
  }
  else {
    old->prev->next = old->next;

    if (old->next) {
      old->next->prev = old->prev;
    }
  }
  old->active = 0;
  old->is_playing = 0;
  playing_n--;
}

const double coeff[5][11]= {
  { 3.11044e-06,
    8.943665402,    -36.83889529,    92.01697887,    -154.337906,    181.6233289,
    -151.8651235,   89.09614114,    -35.10298511,    8.388101016,    -0.923313471
  },
  {4.36215e-06,
   8.90438318,    -36.55179099,    91.05750846,    -152.422234,    179.1170248,
   -149.6496211,87.78352223,    -34.60687431,    8.282228154,    -0.914150747
  },
  { 3.33819e-06,
    8.893102966,    -36.49532826,    90.96543286,    -152.4545478,    179.4835618,
    -150.315433,    88.43409371,    -34.98612086,    8.407803364,    -0.932568035
},
  {1.13572e-06,
   8.994734087,    -37.2084849,    93.22900521,    -156.6929844,    184.596544,
   -154.3755513,    90.49663749,    -35.58964535,    8.478996281,    -0.929252233
  },
  {4.09431e-07,
   8.997322763,    -37.20218544,    93.11385476,    -156.2530937,    183.7080141,
   -153.2631681,    89.59539726,    -35.12454591,    8.338655623,    -0.910251753
  }
};

float formant_filter(float in, t_sound *sound, int channel) {
#if 1
  // this looks like a bug, but it was how it was before..
  // doing independent channels changes the sound
  channel = 0;
#endif
  const double *c = coeff[sound->formant_vowelnum];
  double *h = sound->per_channel[channel].formant_history;
  float res =
    (float) ( c[0] * in +
              c[1] * h[0] +
              c[2] * h[1] +
              c[3] * h[2] +
              c[4] * h[3] +
              c[5] * h[4] +
              c[6] * h[5] +
              c[7] * h[6] +
              c[8] * h[7] +
              c[9] * h[8] +
              c[10] * h[9]
             );

  h[9] = h[8];
  h[8] = h[7];
  h[7] = h[6];
  h[6] = h[5];
  h[5] = h[4];
  h[4] = h[3];
  h[3] = h[2];
  h[2] = h[1];
  h[1] = h[0];
  h[0] = (float) res;
  return res;
}

void init_vcf (t_sound *sound) {
  for (int channel = 0; channel < g_num_channels; ++channel) {
    t_vcf *vcf = &(sound->per_channel[channel].vcf);
    vcf->f     = 2 * sound->cutoff;
    vcf->k     = 3.6f * vcf->f - 1.6f * vcf->f * vcf->f -1;
    vcf->p     = (vcf->k+1) * 0.5f;
    vcf->scale = exp((1-vcf->p)*1.386249f);
    vcf->r     = sound->resonance * vcf->scale;
  }
}

void init_hpf (t_sound *sound) {
  for (int channel = 0; channel < g_num_channels; ++channel) {
    t_vcf *vcf = &(sound->per_channel[channel].hpf);
    vcf->f     = 2 * sound->hcutoff;
    vcf->k     = 3.6f * vcf->f - 1.6f * vcf->f * vcf->f -1;
    vcf->p     = (vcf->k+1) * 0.5f;
    vcf->scale = exp((1-vcf->p)*1.386249f);
    vcf->r     = sound->hresonance * vcf->scale;
  }
}

void init_bpf (t_sound *sound) {
  // I've changed the meaning of some of these a bit
  for (int channel = 0; channel < g_num_channels; ++channel) {
    t_vcf *vcf = &(sound->per_channel[channel].bpf);
    vcf->f     = fabsf(sound->bandf);
    vcf->r     = sound->bandq;
    vcf->k     = vcf->f / vcf->r;
    vcf->p     = 2.0f - vcf->f * vcf->f;
    vcf->scale = 1.0f / (1.0f + vcf->k);
  }
}

void init_per_channel(t_sound *sound) {
  memset(sound->per_channel, 0, g_num_channels * sizeof(t_sound_per_channel));
  init_vcf(sound);
  init_hpf(sound);
  init_bpf(sound);
}

float effect_coarse_pos(float in, t_sound *sound, int channel) {
  t_crs *crs = &(sound->per_channel[channel].coarsef);
  (crs->index)++;
  if (crs->index == sound->coarse) {
    crs->index = 0;
    crs->last = in;
  }
  return crs->last;
}

float effect_coarse_neg(float in, t_sound *sound, int channel) {
  t_crs *crs = &(sound->per_channel[channel].coarsef);
  (crs->index)++;
  crs->sum += in / (float) -(sound->coarse);
  if (crs->index == -(sound->coarse)) {
    crs->last = crs->sum;
    crs->index = 0;
    crs->sum = 0;
  }
  return crs->last;
}

#ifdef FASTPOW
float fastPow(float a, float b) {
  union {
    float d;
    int x[2];
  } u = { a };
  u.x[1] = (int)(b * (u.x[1] - 1072632447) + 1072632447);
  u.x[0] = 0;
  return u.d;
}
#endif

#ifdef FASTPOW
#define myPow (float) fastPow
#else
#define myPow (float) powf
#endif

float effect_vcf(float in, t_sound *sound, int channel) {
  t_vcf *vcf = &(sound->per_channel[channel].vcf);
  vcf->x  = in - vcf->r * vcf->y4;

  float xp = vcf->x * vcf->p;
  float y1p = vcf->y1 * vcf->p;
  float y2p = vcf->y2 * vcf->p;
  float y3p = vcf->y3 * vcf->p;

  vcf->y1 = xp  + vcf->oldx  - vcf->k * vcf->y1;
  vcf->y2 = y1p + vcf->oldy1 - vcf->k * vcf->y2;
  vcf->y3 = y2p + vcf->oldy2 - vcf->k * vcf->y3;
  vcf->y4 = y3p + vcf->oldy3 - vcf->k * vcf->y4;

  vcf->y4 = vcf->y4 - (vcf->y4 * vcf->y4 * vcf->y4) / 6;

  vcf->oldx  = xp;
  vcf->oldy1 = y1p;
  vcf->oldy2 = y2p;
  vcf->oldy3 = y3p;

  return vcf->y4;
}

float effect_hpf(float in, t_sound *sound, int channel) {
  t_vcf *vcf = &(sound->per_channel[channel].hpf);
  vcf->x  = in - vcf->r * vcf->y4;

  vcf->y1 = vcf->x  * vcf->p + vcf->oldx  * vcf->p - vcf->k * vcf->y1;
  vcf->y2 = vcf->y1 * vcf->p + vcf->oldy1 * vcf->p - vcf->k * vcf->y2;
  vcf->y3 = vcf->y2 * vcf->p + vcf->oldy2 * vcf->p - vcf->k * vcf->y3;
  vcf->y4 = vcf->y3 * vcf->p + vcf->oldy3 * vcf->p - vcf->k * vcf->y4;

  vcf->y4 = vcf->y4 - (vcf->y4 * vcf->y4 * vcf->y4) / 6;

  vcf->oldx  = vcf->x;
  vcf->oldy1 = vcf->y1;
  vcf->oldy2 = vcf->y2;
  vcf->oldy3 = vcf->y3;

  return (in - vcf->y4);
}

float effect_bpf(float in, t_sound *sound, int channel) {
  t_vcf *vcf = &(sound->per_channel[channel].bpf);
  vcf->x  = in;

  vcf->y3 = vcf->p * vcf->y2 - vcf->y1 + vcf->k * (vcf->x - vcf->oldx +
        vcf->y2);
  vcf->y3 = vcf->scale * vcf->y3;

  vcf->oldx  = vcf->x;
  vcf->y1 = vcf->y2;
  vcf->y2 = vcf->y3;
  return (vcf->y3);
}

float effect_bpf2(float value, t_sound *p, int channel) {
  value = value - effect_bpf(value, p, channel);
  return (value);
}

float effect_shape(float value, t_sound *p, int channel) {
  value = (1+p->shape_k)*value/(1+p->shape_k*(float) fabs(value));
  // gain compensation, fine-tuned by ear
  if (use_shape_gain_comp) {
    float gcomp = 1.0f - (0.15f * p->shape_k / (p->shape_k + 2.0f));
    value *= gcomp * gcomp;
  }
  return (value);
}

void init_crush(t_sound *p)
{
  p->crush_range = (float) myPow(2,p->crush_bits-1);
}

float effect_crush_pos(float value, t_sound *p, int channel) {
  //value = (1.0 + log(fabs(value)) / 16.63553) * (value / fabs(value));
  float tmp = p->crush_range;
  value = (float) truncf(tmp * value) / tmp;
  //value = exp( (fabs(value) - 1.0) * 16.63553 ) * (value / fabs(value));
  return (value);
}

float effect_crush_neg(float value, t_sound *p, int channel) {
  float isgn = (value >= 0) ? 1 : -1;
  value = isgn * value;
  value = myPow(value, 0.125);
/*
  value = sqrtf(value);
  value = sqrtf(value);
  value = sqrtf(value);
*/
  value = effect_crush_pos(value, p, channel);
  value *= value;
  value *= value;
  value *= value;
/*
  value = myPow(value, 8.0);
*/
  value = isgn * value;
  return (value);
}

float effect_gain(float value, t_sound *p, int channel) {
  value *= p->gain;
  return (value);
}

float effect_env(float value, t_sound *p, int channel) {
  float env = 1.0;
  if (p->playtime < p->attack) {
    env = 1.0523957 - 1.0523958*exp(-3.0 * p->playtime/p->attack);
  } else if (p->playtime > (p->attack + p->hold + p->release)) {
    env = 0.0;
  } else if (p->playtime > (p->attack + p->hold)) {
    env = 1.0523957 *
      exp(-3.0 * (p->playtime - p->attack - p->hold) / p->release) 
      - 0.0523957;
  }
  value *= env;
  return (value);
}

float effect_roundoff(float value, t_sound *p, int channel) {
  float roundoff = 1;
  if ((p->end - p->position) < ROUNDOFF) {
    // TODO what if end < ROUNDOFF?)
    //printf("roundoff: %f\n", (p->end - pos) / (float) ROUNDOFF);
    roundoff = (p->end - p->position) * (1.0f / (float) ROUNDOFF);
    //printf("end roundoff: %f (%f)\n", roundoff, p->end - p->position);
  }
  else {
    if ((p->position - p->start) < ROUNDOFF) {
      roundoff = (p->position - p->start) * (1.0f / (float) ROUNDOFF);
      //printf("start roundoff: %f (%f / %d)\n", roundoff, p->position - p->start, ROUNDOFF);
    }
  }
  value *= roundoff;
  return (value);
}

void init_pan(t_sound *p)
{
  for (int channel = 0; channel < p->channels; ++channel)
  {
    float c = (float) channel + p->pan;
    float d = c - (float) floor(c);
    int channel_a =  ((int) c) % g_num_channels;
    int channel_b =  ((int) c + 1) % g_num_channels;
    if (channel_a < 0) {
      channel_a += g_num_channels;
    }
    if (channel_b < 0) {
      channel_b += g_num_channels;
    }
    // equal power panning
    // PERF - 8.4% of time?
    float tmpa, tmpb;
    // optimisations for middle, hard left + hard right
    if (d == 0.5f) {
      tmpa = tmpb = 0.7071067811f;
    }
    else if (d == 0) {
      tmpa = 1;
      tmpb = 0;
    }
    else if (d == 1) {
      tmpa = 0;
      tmpb = 1;
    }
    else {
      tmpa = (float) cos(HALF_PI * d);
      tmpb = (float) sin(HALF_PI * d);
    }
    t_pan out = {{{channel_a, tmpa}, {channel_b, tmpb}}};
    p->per_channel[channel].pan = out;
  }
}

t_pan effect_pan(float value, t_sound *p, int channel) {
  t_pan out = p->per_channel[channel].pan;
  out.out[0].value *= value;
  out.out[1].value *= value;
  return out;
}

/**/

/**/

void add_delay(t_line *line, float sample, float delay, float feedback) {
  int point = (line->point + (int) ( delay * MAXLINE )) % MAXLINE;

  //printf("'feedback': %f\n", feedback);
  line->samples[point] += (sample * feedback);
}

/**/

float shift_delay(t_line *line) {
  float result = line->samples[line->point];
  line->samples[line->point] = 0;
  line->point = (line->point + 1) % MAXLINE;
  return(result);
}

/**/

extern int audio_play(t_sound* sound) {
  t_sample *sample = NULL;

  sample = file_get_from_cache(sound->samplename);

  if (sample != NULL) {
    sound->sample = sample;

    init_sound(sound);
    sound->prev = NULL;
    sound->next = NULL;
    pthread_mutex_lock(&queue_waiting_lock);
    queue_add(&waiting, sound);
    pthread_mutex_unlock(&queue_waiting_lock);
  }
  else {
    pthread_mutex_lock(&queue_loading_lock);
    if (!is_sample_loading(sound->samplename)) {
      if (!thpool_add_job(read_file_pool, read_file_func, (void*) sound)) {
	fprintf(stderr, "audio_play: Could not add file reading job for '%s'\n", sound->samplename);
      }
    }
    mark_as_loading(sound);
    pthread_mutex_unlock(&queue_loading_lock);
  }

  return(1);

}

void init_effects(t_sound *p) {
  if (p->formant_vowelnum >= 0) {
    p->effects[p->num_effects++] = formant_filter;
  }
  // why 44000 (or 44100)? init_vcf divides by samplerate..
  if (p->resonance > 0 && p->resonance < 1
      && p->cutoff > 0 && p->cutoff < 1) {
    p->effects[p->num_effects++] = effect_vcf;
  }
  if (p->hresonance > 0 && p->hresonance < 1
      && p->hcutoff > 0 && p->hcutoff < 1) {
    p->effects[p->num_effects++] = effect_hpf;
  }
  if (p->bandf > 0 && p->bandf < 1 && p->bandq > 0) {
    p->effects[p->num_effects++] = effect_bpf;
  } else if (p->bandf < 0 && p->bandf > -1 && p->bandq > 0) {
    p->effects[p->num_effects++] = effect_bpf2;
  }
  if (p->coarse > 0) {
    p->effects[p->num_effects++] = effect_coarse_pos;
  }
  else if (p->coarse < 0) {
    p->effects[p->num_effects++] = effect_coarse_neg;
  }
  if (p->shape) {
    p->effects[p->num_effects++] = effect_shape;
  }
  if (p->crush > 0) {
    init_crush(p);
    p->effects[p->num_effects++] = effect_crush_pos;
  } else if (p->crush < 0) {
    init_crush(p);
    p->effects[p->num_effects++] = effect_crush_neg;
  }
  if (p->gain != 1) {
    p->effects[p->num_effects++] = effect_gain;
  }
  if (p->attack >= 0 && p->release >= 0) {
    p->effects[p->num_effects++] = effect_env;
  }
  p->effects[p->num_effects++] = effect_roundoff;
}

void init_sound(t_sound *sound) {
  
  float start_pc = sound->start;
  float end_pc = sound->end;
  t_sample *sample = sound->sample;

  // switch to frames not percent..
  sound->start = 0;
  sound->end = sample->info->frames;
  sound->items = sample->items;
  sound->channels = sample->info->channels;

  sound->active = 1;

  if (sound->delay > 1) {
    sound->delay = 1;
  }

  if (sound->delaytime > 1) {
    sound->delaytime = 1;
  }

  if (sound->delayfeedback >= 1) {
    sound->delayfeedback = 0.9999;
  }

#ifdef JACK
  sound->startT = (sound->when - epochOffset) * 1000000;
# else
  sound->startT = sound->when - epochOffset;
#endif
  

  if (sound->unit == 's') { // unit = "sec"
    sound->accelerate = sound->accelerate / sound->speed; // change rate by 1 per specified duration
    sound->speed = sound->sample->info->frames / sound->speed / g_samplerate;
  }
  else if (sound->unit == 'c') { // unit = "cps"
    sound->accelerate = sound->accelerate * sound->speed * sound->cps; // change rate by 1 per cycle
    sound->speed = sound->sample->info->frames * sound->speed * sound->cps / g_samplerate;
  }
  // otherwise, unit is rate/ratio,
  // i.e. 2 = twice as fast, -1 = normal but backwards

  sound->next = NULL;
  sound->prev = NULL;
  sound->reverse  = sound->speed < 0;
  sound->speed    = fabsf(sound->speed);

  if (sound->channels == 2 && g_num_channels == 2 && sound->pan == 0.5f) {
    sound->pan = 0;
  }
  else {
    sound->mono = 1;
  }
#ifdef FAKECHANNELS
  sound->pan *= (float) g_num_channels / FAKECHANNELS;
#endif
#ifdef SCALEPAN
  if (g_num_channels > 2) {
    sound->pan *= (float) g_num_channels;
  }
#endif
  init_per_channel(sound);

//  if (sound->shape != 0) {
//    float tmp = sound->shape;
//    tmp = fabs(tmp);
//    if (tmp > 0.99) {
//      tmp = 0.99;
//    }
//    sound->shape = 1;
//    sound->shape_k = (2.0f * tmp) / (1.0f - tmp);
//  }
  
  if (sound->crush != 0) {
    float tmp = sound->crush;
    sound->crush = (tmp > 0) ? 1 : -1;
    sound->crush_bits = fabsf(tmp);
  }
  
  if (start_pc < 0) {
    start_pc = 0;
    sound->cut_continue = 1;
  }

  if (sound->delaytime >= 0) {
    delay_time = sound->delaytime;
  }
  if (sound->delayfeedback >= 0) {
    delay_feedback = sound->delayfeedback;
  }

  if (sound->reverse) {
    float tmp = start_pc;
    start_pc = 1 - end_pc;
    end_pc = 1 - tmp;
  }

  //printf("frames: %f\n", new->end);
  if (start_pc > 0 && start_pc <= 1) {
    sound->start = start_pc * sound->end;
  }

  if (end_pc > 0 && end_pc < 1) {
    sound->end *= end_pc;
  }
  sound->position = sound->start;
  sound->playtime = 0.0;

  init_pan(sound);
  init_effects(sound);
}


t_sound *queue_next(t_sound **queue, sampletime_t now) {
  t_sound *result = NULL;
  // printf("queue_next - waiting sz %d / %d\n", queue_size(*queue), queue_size(waiting));
  //printf("%f vs %f\n", *queue == NULL ? 0 : (*queue)->startT, now);
  if (*queue != NULL && (*queue)->startT <= now) {
    result = *queue;
    *queue = (*queue)->next;
    if ((*queue) != NULL) {
      (*queue)->prev = NULL;
    }
  }
  return(result);
}

void cut(t_sound *s) {
  t_sound *p = NULL;
  p = playing;

  int group = s->cutgroup;

  if (group != 0) {
    while (p != NULL) {
      // If group is less than 0, only cut playback of the same sample
      if (p->cutgroup == group && (group > 0 || p->sample == s->sample)) {
        // schedule this sound to end in ROUNDOFF samples time, so we
        // don't get a click
        float newend = p->position + ROUNDOFF;
        // unless it's dying soon anyway..
        if (newend < p->end) {
          p->end = newend;
          // cut_continue means start the next where the prev is leaving off
          if (s->cut_continue > 0 && p->position < s->end) {
            s->start = p->position;
            s->position = p->position;
            s->cut_continue = 0;
          }
        }
        // cut should also kill any looping
        p->sample_loop = 0;
      }
      p = p->next;
    }
  }
}

void dequeue(sampletime_t now) {
  t_sound *p;
  pthread_mutex_lock(&queue_waiting_lock);
  assert(waiting == NULL || waiting->next != waiting);

  while ((p = queue_next(&waiting, now)) != NULL) {
    int s = queue_size(playing);
    cut(p);
    p->prev = NULL;
    p->next = playing;

    p->is_playing = 1;
    playing_n++;
    
    if (playing != NULL) {
      playing->prev = p;
    }
    playing = p;
#ifdef DEBUG
    assert(s == (queue_size(playing) - 1));
#endif

    //printf("done.\n");
  }
  pthread_mutex_unlock(&queue_waiting_lock);
}

float compress(float in) {
  static float env = 0;
  env += (float) 50 / g_samplerate;
  if (fabs(in * env) > 1) {
    env = env / (float) fabs(in * env);
  }
  return(env);
}

float compressdave(float in) {
  static float threshold = 0.5;
  static float env = 0;
  float result = in;
  // square input (to abs and make logarithmic)
  float t=in*in;

  // blend to create simple envelope follower
  env = env*(1-compression_speed) + t*compression_speed;

  // if we are over the threshold
  if (env > threshold) {
    // calculate the gain related to amount over thresh
    result *= 1.0f / (1.0f+(env - threshold));
  }
  return(result);
}

/**/

float playback_source(t_sound *p, int channel)
{
  float value = 0;
  int ix = (p->channels * (p->reverse ? (p->sample->info->frames - (int) p->position) : (int) p->position)) + channel;
  if (0 <= ix && ix < p->end)
  {
    value = p->items[ix];
    int next_ix = p->reverse ? ix - p->channels : ix + p->channels;
    if (0 <= next_ix && next_ix < p->end)
    {
      float next_value = p->items[next_ix];
      float tween_amount = (p->position - (int) p->position);
      /* linear interpolation */
      value += (next_value - value) * tween_amount;
    }
  }
  return value;
}

t_pan playback_effects(float value, t_sound *p, int channel) {
  for (int e = 0; e < p->num_effects; ++e) {
    value = p->effects[e](value, p, channel);
  }
  return effect_pan(value, p, channel);
}

void playback_out(float **buffers, int frame, t_pan out, t_sound *p)
{
  buffers[out.out[0].channel][frame] += out.out[0].value;
  buffers[out.out[1].channel][frame] += out.out[1].value;

#ifdef SEND_RMS
  rms[p->orbit*2 + out.out[0].channel].sum += out.out[0].value;
  rms[p->orbit*2 + out.out[1].channel].sum += out.out[1].value;
#endif

  if (p->delay > 0) {
    add_delay(&delays[out.out[0].channel], out.out[0].value, delay_time, p->delay);
    add_delay(&delays[out.out[1].channel], out.out[1].value, delay_time, p->delay);
  }
}

void playback(float **buffers, int frame, sampletime_t now) {
  int channel;
  t_sound *p = playing;

#ifdef SEND_RMS
  for (int i = 0; i < (MAX_ORBIT*2); ++i) {
    rms[i].sum = 0;
    rms[i].n = (rms[i].n + 1) % RMS_SZ;
  }
#endif
  
  for (channel = 0; channel < g_num_channels; ++channel) {
    buffers[channel][frame] = 0;
  }

  while (p != NULL) {
    t_sound *tmp;

    if (p->startT > now) {
      p->checks++;
      p = p->next;
      continue;
    }
    if ((!p->started) && p->checks == 0 && p->startT < now) {
      /*      printf("started late by %f frames (%d checks)\n",
	     now - p->startT, p->checks
	     );*/
      p->started = 1;
    }
    //printf("playing %s\n", p->samplename);
    //printf("channels: %d\n", p->channels);

    for (channel = 0; channel < p->channels; ++channel) {
      float value = playback_source(p, channel);
      t_pan out = playback_effects(value, p, channel);
      playback_out(buffers, frame, out, p);
      if (p->mono) {
        break;
      }
    }

    if (p->accelerate != 0) {
      // ->startFrame ->end ->position
      p->speed += p->accelerate/g_samplerate;
    }
    p->position += p->speed;
    p->playtime += 1.0 / g_samplerate;

    p->played++;
    //printf("position: %d of %d\n", p->position, playing->end);
    /* remove dead sounds */
    tmp = p;
    p = p->next;
    if (tmp->position >= tmp->end || tmp->position < tmp->start) {
      if (--(tmp->sample_loop) > 0) {
        tmp->position = tmp->start;
      } else {
        queue_remove(&playing, tmp);
      }
    }
  }

  for (channel = 0; channel < g_num_channels; ++channel) {
    float tmp = shift_delay(&delays[channel]);
    if (delay_feedback > 0 && tmp != 0) {
      add_delay(&delays[channel], tmp, delay_time, delay_feedback);
    }
    buffers[channel][frame] += tmp;
  }

  if (use_dirty_compressor) {
    float max = 0;

    for (channel = 0; channel < g_num_channels; ++channel) {
      if (fabsf(buffers[channel][frame]) > max) {
        max = buffers[channel][frame];
      }
    }
    float factor = compress(max);
    for (channel = 0; channel < g_num_channels; ++channel) {
      buffers[channel][frame] *= factor * g_gain/5.0f;
    }
  } else {
    for (channel = 0; channel < g_num_channels; ++channel) {
      buffers[channel][frame] *= g_gain;
    }
  }
  #ifdef SEND_RMS
  for (int i = 0; i < MAX_ORBIT*2; ++i) {
    rms[i].sum_of_squares -= rms[i].squares[rms[i].n];

    // this happens sometimes. could be a floating point error?
    if (rms[i].sum_of_squares < 0) {
      rms[i].sum_of_squares = 0;
    }
    
    if (rms[i].sum == 0) {
      rms[i].squares[rms[i].n] = 0;
    }
    else {
      float sqrd = rms[i].sum * rms[i].sum;
      rms[i].squares[rms[i].n] = sqrd;
      rms[i].sum_of_squares += sqrd;
    }
  }
  #endif
}


#ifdef JACK
extern int jack_callback(int frames, float *input, float **outputs) {
    sampletime_t now;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    epochOffset = ((double) tv.tv_sec + ((double) tv.tv_usec / 1000000.0))
      - ((double) jack_get_time() / 1000000.0);
    //printf("jack time: %d tv_sec %d epochOffset: %f\n", jack_get_time(), tv.tv_sec, epochOffset);

  now = jack_last_frame_time(jack_client);

  for (int i=0; i < frames; ++i) {
    jack_time_t nowt = jack_frames_to_time(jack_client, now + i);
    playback(outputs, i, nowt);

    dequeue(nowt);
  }
  return(0);
}
#elif PULSE

void run_pulse() {
  #define FRAMES 64
  struct timeval tv;
  double samplelength = (((double) 1)/((double) g_samplerate));

  float *buf[g_num_channels];
  for (int i = 0 ; i < g_num_channels; ++i) {
    buf[i] = (float*) malloc(sizeof(float)*FRAMES);
  }
  float interlaced[g_num_channels*FRAMES];

  pa_sample_spec ss;
  ss.format = PA_SAMPLE_FLOAT32LE;
  ss.rate = g_samplerate;
  ss.channels = g_num_channels;

  pa_simple *s = NULL;
  //  int ret = 1;
  int error;
  if (!(s = pa_simple_new(NULL, "dirt", PA_STREAM_PLAYBACK, NULL,
    "playback", &ss, NULL, NULL, &error))) {
    fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n",
    pa_strerror(error));
    goto finish;
  }

  for (;;) {

    pa_usec_t latency;
    if ((latency = pa_simple_get_latency(s, &error)) == (pa_usec_t) -1) {
      fprintf(stderr, __FILE__": pa_simple_get_latency() failed: %s\n",
	      pa_strerror(error));
      goto finish;
    }
    //fprintf(stderr, "%f sec    \n", ((float)latency)/1000000.0f);

    gettimeofday(&tv, NULL);
    double now = ((double) tv.tv_sec + ((double) tv.tv_usec / 1000000.0));

    for (int i=0; i < FRAMES; ++i) {
      double framenow = now + (samplelength * (double) i);
      playback(buf, i, framenow);
      for (int j=0; j < g_num_channels; ++j) {
	interlaced[g_num_channels*i+j] = buf[j][i];
      }
      dequeue(framenow);
    }

    if (pa_simple_write(s, interlaced, sizeof(interlaced), &error) < 0) {
      fprintf(stderr, __FILE__": pa_simple_write() failed: %s\n", pa_strerror(error));
      goto finish;
    }
  }
  /* Make sure that every single sample was played */
  if (pa_simple_drain(s, &error) < 0) {
    fprintf(stderr, __FILE__": pa_simple_drain() failed: %s\n", pa_strerror(error));
    goto finish;
  }
  //    ret = 0;
 finish:
  if (s)
    pa_simple_free(s);
  //    return ret;
}

#else


static int pa_callback(const void *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       const PaStreamCallbackTimeInfo* timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData) {

  struct timeval tv;

  if (epochOffset == 0) {
    gettimeofday(&tv, NULL);
    #ifdef HACK
    epochOffset = 0;
    #else
    epochOffset = ((double) tv.tv_sec + ((double) tv.tv_usec / 1000000.0))
      - timeInfo->outputBufferDacTime;
    #endif
    /* printf("set offset (%f - %f) to %f\n", ((float) tv.tv_sec + ((float) tv.tv_usec / 1000000.0f))
       , timeInfo->outputBufferDacTime, epochOffset); */
  }
  #ifdef HACK
  double now = ((double) tv.tv_sec + ((double) tv.tv_usec / 1000000.0));
  #else
  double now = timeInfo->outputBufferDacTime;
  #endif
  // printf("%f %f %f\n", timeInfo->outputBufferDacTime, timeInfo->currentTime,   Pa_GetStreamTime(stream));
  float **buffers = (float **) outputBuffer;
  for (int i=0; i < framesPerBuffer; ++i) {
    double framenow = now + (((double) i)/((double) g_samplerate));
    playback(buffers, i, framenow);
    dequeue(framenow);
  }
  return paContinue;
}
#endif



#ifdef JACK
void jack_init(bool autoconnect) {
  jack_client = jack_start(jack_callback, autoconnect);
  g_samplerate = jack_get_sample_rate(jack_client);
}
#elif PULSE
void pulse_init() {
  //  pulse = pa_threaded_mainloop_new();
  //  pa_threaded_mainloop_set_name(pulse, "dirt");
}
#else

static void StreamFinished( void* userData ) {
  printf( "Stream Completed\n");
}

void pa_init(void) {
  PaStreamParameters outputParameters;

  PaError err;

  printf("init pa\n");

  err = Pa_Initialize();
  if( err != paNoError ) {
    goto error;
  }

  int num = Pa_GetDeviceCount();
  const PaDeviceInfo *d;
  if (num <0) {
    err = num;
    goto error;
  }

  printf("Devices = #%d\n", num);
  for (int i =0; i < num; i++) {
     d = Pa_GetDeviceInfo(i);
     printf("%d = %s: %fHz\n", i, d->name, d->defaultSampleRate);
  }

  outputParameters.device = Pa_GetDefaultOutputDevice();
  if (outputParameters.device == paNoDevice) {
    fprintf(stderr,"Error: No default output device.\n");
    goto error;
  }
  printf("default device: %s\n", Pa_GetDeviceInfo(outputParameters.device)->name);
  outputParameters.channelCount = g_num_channels;
  outputParameters.sampleFormat = paFloat32 | paNonInterleaved;
  outputParameters.suggestedLatency = 0.050;
  // Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
  outputParameters.hostApiSpecificStreamInfo = NULL;

  char foo[] = "hello";
  err = Pa_OpenStream(
            &stream,
            NULL, /* no input */
            &outputParameters,
            g_samplerate,
            PA_FRAMES_PER_BUFFER,
            paNoFlag,
            pa_callback,
            (void *) foo );
    if( err != paNoError ) {
      printf("failed to open stream.\n");
      goto error;
    }

    err = Pa_SetStreamFinishedCallback( stream, &StreamFinished );
    if( err != paNoError ) {
      goto error;
    }

#ifdef __linux__
    printf("setting realtime priority\n");
    PaAlsa_EnableRealtimeScheduling(stream, 1);
#endif
    
    err = Pa_StartStream(stream);
    if( err != paNoError ) {
      goto error;
    }

  return;
error:
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    if( err == paUnanticipatedHostError) {
	const PaHostErrorInfo *hostErrorInfo = Pa_GetLastHostErrorInfo();
	fprintf( stderr, "Host API error = #%ld, hostApiType = %d\n", hostErrorInfo->errorCode, hostErrorInfo->hostApiType );
	fprintf( stderr, "Host API error = %s\n", hostErrorInfo->errorText );
    }
    Pa_Terminate();
    exit(-1);
}
#endif

#ifdef SEND_RMS
void thread_send_rms() {
  lo_address a = lo_address_new(NULL, "6010");
  lo_message m;
  
  while(1) {
    m = lo_message_new();
    for (int i = 0; i < (MAX_ORBIT*2); ++i) {
      if (rms[i].sum_of_squares == 0) {
	lo_message_add_float(m, 0);
      }
      else {
	float result = sqrt(rms[i].sum_of_squares / RMS_SZ);
	lo_message_add_float(m, result);
      }
    }
    lo_send_message(a, "/rmsall", m);
    lo_message_free(m);
    usleep(50000);
  }
}
#endif

extern void audio_init(bool dirty_compressor, bool autoconnect, bool late_trigger, unsigned int num_workers, char *sroot, bool shape_gain_comp, bool preload_flag) {
  struct timeval tv;

  atexit(audio_close);

  gettimeofday(&tv, NULL);
  sampleroot = sroot;
  starttime = (float) tv.tv_sec + ((float) tv.tv_usec / 1000000.0);

  delays = calloc(g_num_channels, sizeof(t_line));
  if (!delays) {
    fprintf(stderr, "no memory to allocate `delays' array\n");
    exit(1);
  }
  
  pthread_mutex_init(&queue_waiting_lock, NULL);
  pthread_mutex_init(&queue_loading_lock, NULL);
  pthread_mutex_init(&mutex_sounds, NULL);

  if (preload_flag) {
    file_preload_samples(sampleroot);
  }
  
  read_file_pool = thpool_init(num_workers);
  if (!read_file_pool) {
    fprintf(stderr, "could not initialize `read_file_pool'\n");
    exit(1);
  }

#ifdef SEND_RMS
  memset(rms, 0, sizeof(t_rms) * MAX_ORBIT * 2);
  pthread_t rms_t;
  pthread_create(&rms_t, NULL, (void*) thread_send_rms, NULL);
#endif

#ifdef JACK
  jack_init(autoconnect);
#elif PULSE
  pthread_t t;
  pthread_create(&t, NULL, (void *(*)(void *)) run_pulse, NULL);
  //sleep(1);
#else
  pa_init();
#endif
  compression_speed = 1000 / g_samplerate;
  use_dirty_compressor = dirty_compressor;
  use_late_trigger = late_trigger;
  use_shape_gain_comp = shape_gain_comp;
}

extern void audio_close(void) {
  if (delays) free(delays);
  if (read_file_pool) thpool_destroy(read_file_pool);
}

// Reset sound structure for reutilization
static void reset_sound(t_sound* s) {
  memset(s, 0, sizeof(t_sound));
}

/**/

t_sound *new_sound() {
  t_sound *result = NULL;
  t_sound *oldest = NULL;
  int dying = 0;
  int cull = playing_n >= MAX_PLAYING;

  pthread_mutex_lock(&mutex_sounds);
  
  for (int i = 0; i < MAX_SOUNDS; ++i) {
    if (result == NULL && sounds[i].active == 0) {
	result = &sounds[i];
    }

    if (cull && sounds[i].is_playing == 1) {
      if ((sounds[i].end - sounds[i].position) > ROUNDOFF) {
        if (oldest == NULL || oldest->startT > sounds[i].startT) {
	  oldest = &sounds[i];
	}
      }
      else {
	dying++;
      }
    }
  }

  // printf("playing: %d dying: %d \n", playing_n, dying);
  
  // Treat MAX_PLAYING as a soft limit - those about to finish
  // aren't counted.
  if ((playing_n - dying) >= MAX_PLAYING) {
    // printf("hit soft buffer, playing_n %d, dying %d, MAX_PLAYING %d(-%d)\n", playing_n, dying, MAX_PLAYING, MAX_PLAYING_SOFT_BUFFER);
    if (oldest != NULL) {
      // printf("culling sound with end %f, position %f, ROUNDOFF %d\n", oldest->end, oldest->position, ROUNDOFF);

      // Rather than stop immediately, set it to finish in ROUNDOFF
      // samples, so the envelope is applied thereby
      // avoiding audio clicks.
      oldest->end = oldest->position + ROUNDOFF;
    }
  }

  if (result != NULL) {
    reset_sound(result);
    result->active = 1;
  }

  pthread_mutex_unlock(&mutex_sounds);
  // printf("qs: playing %d waiting %d loading %d\n", queue_size(playing), queue_size(waiting), queue_size(loading));
  return(result);
}
