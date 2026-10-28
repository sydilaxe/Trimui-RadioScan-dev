/* RadioScan synth engine — two internet radio streams, each played through a voice and opened
 * and closed by its own Euclidean rhythm.
 *
 *  stream A -> [voice sense] -> VOICE A (LIVE | LOOP | WAVE | GRAIN) (+ osc) -> RHYTHM A --\
 *                                                       FM between the voices, either way    CROSS: ring, vocode, morph
 *  stream B -> [voice sense] -> VOICE B (LIVE | LOOP | WAVE | GRAIN) (+ osc) -> RHYTHM B --/   -> FILTER -> FOLD
 *                                                                                             -> ECHO -> SPACE
 *                                                                                             -> VOLUME -> LEVELLER
 *  The tempo clock (tap tempo, Wi-Fi sync) drives both rhythms.
 *  "Voice sense" classifies each stream as speech or music so a voice can focus on either.
 */
#ifndef DSP_H
#define DSP_H

/* a rhythm: a Euclidean gate and a stutter at the same rate; voices A and B and the cross stage each have one */
#define RHYTHM_FIELDS(X)                                                                                      \
    P_##X##_GATE_ON, P_##X##_GATE_STEPS, P_##X##_GATE_PULSES, P_##X##_GATE_ROT,                                 \
    P_##X##_RATE, P_##X##_LENGTH, P_##X##_ATTACK, P_##X##_RELEASE, P_##X##_CHANCE,                              \
    P_##X##_STUT_ON, P_##X##_STUT_STEPS, P_##X##_STUT_PULSES, P_##X##_STUT_ROT,                                 \
    P_##X##_STUT_RATCHET, P_##X##_STUT_MODE, P_##X##_STUT_CHANCE

#define VOICE_FIELDS(X)                                                                                       \
    P_##X##_STATION, P_##X##_MODE, P_##X##_TUNE, P_##X##_SIZE, P_##X##_TEXTURE, P_##X##_DIR, P_##X##_FOCUS,     \
    P_##X##_LEVEL, P_##X##_MUTE, P_##X##_PAN, P_##X##_DRIVE, P_##X##_OSC, P_##X##_OSC_MIX,                     \
    P_##X##_LFO1_WAVE, P_##X##_LFO1_RATE, P_##X##_LFO1_SYNC, P_##X##_LFO1_DEPTH, P_##X##_LFO1_DEST,            \
    P_##X##_LFO2_WAVE, P_##X##_LFO2_RATE, P_##X##_LFO2_SYNC, P_##X##_LFO2_DEPTH, P_##X##_LFO2_DEST,                                    \
    RHYTHM_FIELDS(X)

enum {
    VOICE_FIELDS(A), /* VOICE B id = VOICE A id + OSC_STRIDE */
    VOICE_FIELDS(B),
    P_BPM, P_SYNC,
    P_OUTPUT, P_CLK_SIDE, P_CLK_PPQN, P_CLK_PULSE, /* stereo, or mono sound + clock pulses for other gear */
    P_VOCODE, P_VOCODE_AMT, P_BANDS, P_FM, P_FM_DIR, P_RING, P_MORPH, P_CONVERSE,
    P_ROOT,
    P_TONE, P_RESO, P_LOW, P_HIGH, P_DRIVE, P_FOLD, P_CRUSH,
    P_ECHO, P_SPACE, P_WIDTH, P_VOLUME,
    P_CROSS_LEVEL, P_SHAPE_LEVEL, P_ECHO_LEVEL, P_SPACE_LEVEL, /* MIX page: how loud each effect comes back */
    P_LIMIT, P_LIM_RELEASE, P_LIM_CEILING, P_AUTO_LEVEL,      /* MIX page: master limiter and leveller */
    /* the cross rhythm: its gate brings cross-modulation (morph, vocode, ring, FM) in on its pulses, and its
     * stutter ratchets the mix after cross-modulation */
    RHYTHM_FIELDS(X),
    P_COUNT
};

#define OSC_STRIDE (P_B_STATION - P_A_STATION)
/* The rhythms on screen: each one's gate (A, B, cross) and each one's stutter. A rhythm belongs to v = 0 (A), 1 (B)
 * or SEQ_X (cross), and its settings are one RHYTHM_FIELDS block. */
enum { SEQ_A, SEQ_B, SEQ_X, SEQ_SA, SEQ_SB, SEQ_SX, SEQ_COUNT };
#define SEQ_VOICE(seq) ((seq) >= SEQ_SA ? (seq) - SEQ_SA : (seq))
#define RHYTHM_BASE(v) ((v) == SEQ_X ? P_X_GATE_ON : P_A_GATE_ON + (v) * OSC_STRIDE)
#define RHYTHM_P(v, F) (RHYTHM_BASE(v) + (P_A_##F - P_A_GATE_ON)) /* e.g. RHYTHM_P(v, RATE), RHYTHM_P(v, STUT_MODE) */
/* on (field 0), steps (1), pulses (2) or rotate (3) of a gate or stutter pattern */
#define SEQ_PARAM(seq, field) (RHYTHM_BASE(SEQ_VOICE(seq)) + ((seq) >= SEQ_SA ? P_A_STUT_ON - P_A_GATE_ON : 0) + (field))
#define SEQ_SHAPE(seq, F) RHYTHM_P(SEQ_VOICE(seq), F) /* RATE, LENGTH, ATTACK, RELEASE, CHANCE */
/* stutter modes: REPEAT the step's first slice; REVERSE every other ratchet; RANDOM count and direction;
 * PITCH each ratchet up (or down) the scale; DROPOUT some ratchets to silence; SCATTER fragments of a latched
 * sample at random places, directions and octaves (a grab re-latches) */
enum { STUT_REPEAT, STUT_REVERSE, STUT_RANDOM, STUT_PITCH, STUT_DROPOUT, STUT_SCATTER };

enum { OSC_LIVE, OSC_LOOP, OSC_WAVE, OSC_GRAIN };
enum { FOCUS_ALL, FOCUS_VOICE, FOCUS_MUSIC };
enum { DIR_FORWARD, DIR_REVERSE, DIR_BOTH }; /* BOTH: loops ping-pong, grains go either way */
enum { CAT_ANY, CAT_VOICE, CAT_MUSIC, CAT_SOUND };
enum { VS_SILENT, VS_MUSIC, VS_VOICE };
enum { PAGE_VOICES, PAGE_RHYTHM, PAGE_ENGINE, PAGE_MOD, PAGE_MIX, PAGE_COUNT };
/* two LFOs per voice: k = 0 or 1; LFO_PARAM(o, k, WAVE / RATE / SYNC / DEPTH / DEST). SYNC 0 runs free at RATE Hz;
 * otherwise a note value, locked to the clock (tap tempo and Wi-Fi sync included) */
#define LFO_PARAM(o, k, F) (P_A_LFO1_##F + (o) * OSC_STRIDE + (k) * (P_A_LFO2_WAVE - P_A_LFO1_WAVE))
enum { LFO_SINE, LFO_TRIANGLE, LFO_SAW_UP, LFO_SAW_DOWN, LFO_SQUARE, LFO_RANDOM, LFO_DRIFT, LFO_WAVES };
/* what an LFO moves: its own voice's pitch, level, pan, size, texture, drive or oscillator mix, the shared filter,
 * morph, fold, echo or space, or the voice's station (radio scan, done by the app: see synth_lfo_value) */
enum { LFO_OFF, LFO_PITCH, LFO_LEVEL, LFO_PAN, LFO_SIZE, LFO_TEXTURE, LFO_DRIVE, LFO_OSC_MIX, LFO_FILTER, LFO_MORPH,
       LFO_FOLD, LFO_ECHO, LFO_SPACE, LFO_SCAN, LFO_DESTS };
/* OUTPUT: STEREO, or MONO + CLOCK: the sound in mono on one side and clock pulses (P_CLK_PPQN per quarter note,
 * P_CLK_PULSE ms wide) on the other (P_CLK_SIDE: 0 right, 1 left), while the clock runs. Every pan and stereo spread
 * plays from the centre; the settings are kept. */
enum { OUT_STEREO, OUT_MONO_CLOCK };
int clk_ppqn_value(int i);                             /* P_CLK_PPQN index -> pulses per quarter note */
enum { VOC_OFF, VOC_A_TO_B, VOC_B_TO_A };
enum { FM_B_TO_A, FM_A_TO_B };

typedef struct {
    const char *key;      /* settings-file key */
    const char *label;    /* on-screen name */
    int page;
    float min, max, def;
    float step;           /* linear step, or multiplicative step when log=1 */
    int log;
    const char *unit;
    const char *const *choices; /* non-NULL: enumerated parameter */
} ParamInfo;

extern const ParamInfo param_info[P_COUNT];
extern const char *const page_names[PAGE_COUNT];
extern const char *const category_names[];
const char *param_label(int id, const float *params);  /* engine-aware labels */
const char *param_section(int id);                     /* group heading */
int euclid_hit(int steps, int pulses, int rotate, int step);

typedef struct Synth Synth;

Synth *synth_create(int sample_rate);
void synth_destroy(Synth *s);
void synth_set_param(Synth *s, int id, float v);
float synth_get_param(const Synth *s, int id);
const float *synth_params(const Synth *s);

/* pitch for LOOP / WAVE / GRAIN voices (LIVE plays the radio as is); note 60 plays slices at their own speed */
void synth_note_on(Synth *s, int midi_note);
void synth_note_off(Synth *s, int midi_note);
void synth_grab(Synth *s, int osc);                    /* fresh slice from the stream; osc 0, 1 or -1 */
/* clean slate: both voices plain LIVE feeds at unity, rhythms off (patterns kept), every effect neutral and its
 * tail cleared; stations, volume and tempo stay. Faded out and back in by the audio thread, so it doesn't click. */
void synth_clean(Synth *s);

/* clock: tap tempo and Wi-Fi sync (main thread; hold the audio lock around position calls) */
void synth_tap_reset(Synth *s);
float synth_beat_phase(const Synth *s);
double synth_position(const Synth *s);                 /* 16th notes since the downbeat */
void synth_set_position(Synth *s, double steps);
void synth_set_clock_rate(Synth *s, double rate);      /* 0.9 .. 1.1 speed trim */
double synth_steps_per_frame(const Synth *s);          /* how far the position moves each frame */
void synth_set_running(Synth *s, int on);
int synth_running(const Synth *s);

/* dice: randomise parameters within musical ranges (amount 1 = anything goes, small = gentle) */
void synth_randomize(Synth *s, const int *ids, int n, float amount);
enum { DICE_SOUND, DICE_RHYTHM, DICE_MUTATE };
void synth_dice(Synth *s, int which);

/* inA / inB: interleaved stereo stream audio; out: interleaved stereo */
void synth_process(Synth *s, const float *inA, const float *inB, float *out, int frames);

/* display */
int synth_seq_step(const Synth *s, int seq);           /* current step of SEQ_A, SEQ_B or SEQ_X */
float synth_seq_flash(const Synth *s, int seq);
float synth_cross_gate(const Synth *s);                /* how open the cross rhythm is now, 0..1 */
float synth_lfo_value(const Synth *s, int o, int k);   /* an LFO's output now, -1..1 */
float synth_lfo_hz(const Synth *s, int o, int k);      /* its speed now, free or from the tempo */
float synth_gate_value(const Synth *s, int osc);       /* how open the voice is now, 0..1 */
float synth_speechness(const Synth *s, int osc);       /* 0 music .. 1 speech */
int synth_voice_state(const Synth *s, int osc);        /* VS_* */
float synth_osc_level(const Synth *s, int osc);
int synth_floor(const Synth *s);                       /* converse: which voice holds the floor (0, 1), -1 off */
/* copy the latest n samples (n <= 2048) of a tap, oldest first */
void synth_tap_read(const Synth *s, int tap, float *dst, int n);

enum { TAP_STREAM, TAP_VOICES, TAP_CROSS, TAP_FILTER, TAP_FOLD, TAP_ECHO, TAP_OUT, TAP_A, TAP_B, TAP_RAW_A, TAP_RAW_B, TAP_COUNT };
/* level (RMS), brightness (0 dark .. ~1.4 noise) and crest factor (peak / RMS) over the last ~20 ms */
void synth_tap_features(const Synth *s, int tap, float *rms, float *bright, float *crest);
float synth_fold_preview(float x, float fold);         /* the folder's shape at a fold amount */
float synth_cross_amount(const Synth *s);              /* 0..1: how much cross-modulation is on */
#define LIMIT_RANGE_DB 18.0f                           /* the limiter can push the mix up to +18 dB into its ceiling */
/* MIX page meters: peak level (0..~1) of each voice after its drive and fader, each effect's return and the
 * output; MIX_LIMIT is the limiter's gain reduction (0 none .. 1 full) */
enum { MIX_A, MIX_B, MIX_CROSS, MIX_SHAPE, MIX_ECHO, MIX_SPACE, MIX_LIMIT, MIX_OUT, MIX_COUNT };
float synth_mix_meter(const Synth *s, int ch);

float param_clamp(int id, float v);
float param_adjust(int id, float v, int dir, int fine);
void param_format_ctx(int id, const float *params, char *buf, int n);

#endif
