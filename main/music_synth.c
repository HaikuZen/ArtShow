/* music_synth.c - the synthesizer and the sequencer.
 *  - voices: 2-operator FM (a sine modulating a sine), an attack and an
 *    exponential decay; the modulation index decays too (bells, plucks)
 *  - a pad: three soft sines on the chord, swelling in and out with it
 *  - an echo: a feedback delay (3/8 of a bar), mixed in
 *  - the sequencer: eighth notes; a chord a bar (a progression in the
 *    scale), the bass on the bar's first beat, the melody a random walk on
 *    the scale's degrees (a loop for Truchet's ostinato), its density and
 *    sound from the style
 * The key from the colours: the middle colour's hue picks the root. */
#include "music_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICES   10
#define LUT      1024
#define TAU      6.2831853f

typedef struct {
    const char *name;
    int8_t scale[8];
    int n;
} scale_t;

static const scale_t SCALES[] = {
    { "lydian", { 0, 2, 4, 6, 7, 9, 11 }, 7 },
    { "aeolian", { 0, 2, 3, 5, 7, 8, 10 }, 7 },
    { "hirajoshi", { 0, 2, 3, 7, 8 }, 5 },
    { "dorian", { 0, 2, 3, 5, 7, 9, 10 }, 7 },
    { "major pentatonic", { 0, 2, 4, 7, 9 }, 5 },
    { "minor pentatonic", { 0, 3, 5, 7, 10 }, 5 },
};

/* a style per generative piece */
typedef struct {
    int scale;                   /* SCALES' */
    int bpm_lo, bpm_hi;
    float density;               /* a melody note on an eighth: chance */
    float ratio, index, idx_decay_s;   /* the melody's FM: modulator/carrier, depth, its fade (s) */
    float decay_s;               /* the melody's note length (to -60 dB) */
    float pad, echo;             /* their mix */
    int bass_every;              /* eighths between bass notes */
    int loop;                    /* an ostinato of this many eighths, else 0 */
} style_t;

static const style_t STYLES[SYNTH_STYLES] = {
    /* flow field: flowing lines, soft */
    { 0, 66, 80, 0.55f, 1.0f, 1.2f, 0.8f, 1.6f, 0.55f, 0.35f, 8, 0 },
    /* attractor: sparse bells in the dark */
    { 1, 48, 58, 0.22f, 3.5f, 3.0f, 2.0f, 3.5f, 0.75f, 0.5f, 16, 0 },
    /* Hitomezashi: a plucked koto-ish line */
    { 2, 70, 84, 0.5f, 1.0f, 4.0f, 0.15f, 0.9f, 0.3f, 0.3f, 8, 0 },
    /* stained glass: chords, slow, an organ-ish bell */
    { 3, 54, 64, 0.32f, 2.0f, 1.5f, 1.2f, 2.5f, 0.9f, 0.45f, 8, 0 },
    /* Mondrian: brisk, staccato, a walking bass */
    { 4, 96, 110, 0.7f, 1.0f, 2.5f, 0.08f, 0.35f, 0.2f, 0.2f, 2, 0 },
    /* Truchet: a looping riff */
    { 5, 84, 96, 0.8f, 1.0f, 3.0f, 0.12f, 0.6f, 0.35f, 0.3f, 4, 8 },
};

static const char *NOTE[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

typedef struct {
    float ph, inc, mph, minc;    /* carrier and modulator: phase in turns, step per sample */
    float env, att, peak, decay; /* attack to peak, then *decay each sample */
    float idx, idx_decay;
    int attacking;
} voice_t;

static struct {
    int ready;
    float lut[LUT + 1];
    voice_t v[VOICES];
    int nextv;
    float pad_f[3], pad_ph[3], pad_env, pad_target;
    float *echo;
    int echo_len, echo_pos;
    float lp;                    /* a one-pole low-pass: warmth */
    uint32_t rng;
    const style_t *st;
    const scale_t *sc;
    int root;                    /* MIDI */
    int step_len, step_pos, step;
    int degree, chord;
    int prog[4];
    int loop_notes[16];
    float gain;                  /* the master, ramped up from 0: no click */
    int bpm;
} S;

static uint32_t rnd(void) {
    S.rng ^= S.rng << 13;
    S.rng ^= S.rng >> 17;
    S.rng ^= S.rng << 5;
    return S.rng;
}
static float frnd(void) { return (rnd() & 0xFFFF) / 65536.0f; }

static inline float lsin(float turns) {
    float t = (turns - floorf(turns)) * LUT;
    int i = (int)t;
    return S.lut[i] + (S.lut[i + 1] - S.lut[i]) * (t - i);
}

static float hz(int midi) { return 440.0f * powf(2.0f, (midi - 69) / 12.0f); }

/* a scale degree (any, negative too) as MIDI, from the root */
static int degree_note(int d) {
    int n = S.sc->n, oct = d >= 0 ? d / n : -((-d + n - 1) / n);
    return S.root + 12 * oct + S.sc->scale[d - oct * n];
}

static void play(int midi, float peak, float ratio, float index, float idx_decay_s, float decay_s, float attack_s, int rate) {
    voice_t *v = &S.v[S.nextv];
    S.nextv = (S.nextv + 1) % VOICES;   /* the oldest gives way */
    float f = hz(midi);
    v->ph = v->mph = 0;
    v->inc = f / rate;
    v->minc = f * ratio / rate;
    v->env = 0;
    v->peak = peak;
    v->att = peak / (attack_s * rate + 1);
    v->attacking = 1;
    v->decay = expf(logf(0.001f) / (decay_s * rate));   /* -60 dB in decay_s */
    v->idx = index;
    v->idx_decay = expf(logf(0.05f) / (idx_decay_s * rate));
}

static void new_chord(int rate) {
    int d = S.prog[(S.step / 8) % 4];
    S.chord = d;
    int tones[3] = { degree_note(d - S.sc->n), degree_note(d - S.sc->n + 2), degree_note(d - S.sc->n + 4) };
    for (int k = 0; k < 3; k++) S.pad_f[k] = hz(tones[k]) / rate;
    S.pad_env *= 0.6f;   /* a breath between chords */
}

static void tick(int rate) {
    const style_t *st = S.st;
    if (S.step % 8 == 0) new_chord(rate);
    if (S.step % st->bass_every == 0) {
        int d = S.chord - 2 * S.sc->n;
        if (st->bass_every <= 2) d += (int)(rnd() % 3) - 1;   /* walking */
        play(degree_note(d), 0.5f, 1.0f, 0.6f, 0.4f, st->bass_every <= 2 ? 0.5f : 2.0f, 0.01f, rate);
    }
    int note;
    if (st->loop) {   /* the riff, each bar a little changed */
        note = S.loop_notes[S.step % st->loop];
        if (S.step % (st->loop * 4) == 0) S.loop_notes[rnd() % st->loop] = S.chord + (int)(rnd() % 5);
        if (note < 0) goto out;
    } else {
        if (frnd() > st->density) goto out;
        /* a random walk on the degrees, drawn back to the chord now and then */
        static const int moves[] = { -2, -1, -1, 0, 1, 1, 2, 3 };
        S.degree += moves[rnd() % 8];
        if (frnd() < 0.15f) S.degree = S.chord + (int)(rnd() % 3) * 2;
        if (S.degree < 0) S.degree = 1;
        if (S.degree > 2 * S.sc->n) S.degree = 2 * S.sc->n - 2;
        note = S.degree;
    }
    play(degree_note(note), 0.32f + 0.12f * frnd(), st->ratio, st->index, st->idx_decay_s, st->decay_s * (0.7f + 0.6f * frnd()), 0.004f, rate);
out:
    S.step++;
}


/* the hue (0..360) of an 0xRRGGBB */
static float hue(uint32_t c) {
    float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = (c & 255) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn, h;
    if (d < 1e-4f) return 0;
    if (mx == r) h = fmodf((g - b) / d, 6);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    return h < 0 ? h + 360 : h;
}

/* the key and the tempo, from the colours and the seed (the seed's first draw) */
static void key_tempo(uint32_t seed, int style, const uint32_t *pal, const style_t **st, int *root, int *bpm) {
    *st = &STYLES[(style % SYNTH_STYLES + SYNTH_STYLES) % SYNTH_STYLES];
    int r = (int)(hue(pal[2]) / 30) % 12;   /* the colour's hue, round the circle of keys */
    *root = 57 + (r + 3) % 12 - 3;          /* near A3: 54..65 */
    uint32_t x = seed * 2654435761u | 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *bpm = (*st)->bpm_lo + x % ((*st)->bpm_hi - (*st)->bpm_lo + 1);
}

void synth_describe(uint32_t seed, int style, const uint32_t *pal, char *desc, size_t n) {
    const style_t *st;
    int root, bpm;
    key_tempo(seed, style, pal, &st, &root, &bpm);
    snprintf(desc, n, "%s %s, %d bpm", NOTE[root % 12], SCALES[st->scale].name, bpm);
}

void synth_set(uint32_t seed, int style, const uint32_t *pal, char *desc, size_t n) {
    if (!S.ready) {
        for (int i = 0; i <= LUT; i++) S.lut[i] = sinf(i * TAU / LUT);
        S.ready = 1;
    }
    int bpm;
    key_tempo(seed, style, pal, &S.st, &S.root, &bpm);
    S.sc = &SCALES[S.st->scale];
    S.rng = seed * 2654435761u | 1;
    rnd();   /* the draw key_tempo took for the tempo */
    S.step_len = 0;                            /* worked out at the next render (the rate) */
    S.step_pos = 0;
    S.step = 0;
    S.degree = S.sc->n;
    static const int progs[][4] = { { 0, 3, 4, 0 }, { 0, 5, 3, 4 }, { 0, 2, 3, 1 }, { 0, 4, 5, 3 } };
    memcpy(S.prog, progs[rnd() % 4], sizeof S.prog);
    if (S.sc->n == 5)   /* pentatonic: fewer degrees */
        for (int k = 0; k < 4; k++) S.prog[k] %= 5;
    for (int k = 0; k < 16; k++)   /* the riff: chord tones, a few rests */
        S.loop_notes[k] = rnd() % 4 == 0 ? -1 : S.sc->n + (int)(rnd() % 5);
    S.pad_target = S.st->pad;
    S.gain = 0;   /* fade in */
    S.bpm = bpm;
    if (desc) synth_describe(seed, style, pal, desc, n);
}

void synth_render(int16_t *out, int n, int rate) {
    if (!S.st) {
        memset(out, 0, n * sizeof *out);
        return;
    }
    if (!S.step_len) {   /* a new piece: its eighth in samples, the echo 3 eighths long */
        S.step_len = rate * 60 / S.bpm / 2;
        int len = S.step_len * 3;
        if (len != S.echo_len) {
            free(S.echo);
            S.echo = calloc(len, sizeof *S.echo);
            S.echo_len = S.echo ? len : 0;
        }
        if (S.echo) memset(S.echo, 0, S.echo_len * sizeof *S.echo);
        S.echo_pos = 0;
        S.pad_env = 0;
    }
    const float swell = 1.0f / (rate * 1.5f), fade = 1.0f / (rate * 0.5f);
    for (int i = 0; i < n; i++) {
        if (--S.step_pos <= 0) {
            tick(rate);
            S.step_pos = S.step_len;
        }
        float x = 0;
        for (int k = 0; k < VOICES; k++) {
            voice_t *v = &S.v[k];
            if (v->attacking) {
                v->env += v->att;
                if (v->env >= v->peak) {
                    v->env = v->peak;
                    v->attacking = 0;
                }
            } else {
                v->env *= v->decay;
                if (v->env < 1e-5f) continue;
            }
            float m = lsin(v->mph) * v->idx * (1.0f / TAU);   /* phase modulation, in turns */
            x += lsin(v->ph + m) * v->env;
            v->idx *= v->idx_decay;
            v->ph += v->inc;
            v->mph += v->minc;
            if (v->ph >= 1) v->ph -= 1;
            if (v->mph >= 1) v->mph -= 1;
        }
        S.pad_env += (S.pad_target - S.pad_env) * swell;
        float p = 0;
        for (int k = 0; k < 3; k++) {
            p += lsin(S.pad_ph[k]);
            S.pad_ph[k] += S.pad_f[k];
            if (S.pad_ph[k] >= 1) S.pad_ph[k] -= 1;
        }
        x += p * S.pad_env * 0.12f;
        if (S.echo_len) {   /* the echo: what's in the line comes back, and goes round again a little softer */
            float e = S.echo[S.echo_pos];
            S.echo[S.echo_pos] = x + e * 0.45f;
            S.echo_pos = (S.echo_pos + 1) % S.echo_len;
            x += e * S.st->echo;
        }
        S.lp += (x - S.lp) * 0.35f;   /* warmer: the small speaker's harshness off */
        S.gain += (1.0f - S.gain) * fade;
        x = S.lp * S.gain * 1.2f;
        x = x / (1.0f + fabsf(x));    /* a soft clip, never a hard one */
        out[i] = (int16_t)(x * 22000);
    }
}
