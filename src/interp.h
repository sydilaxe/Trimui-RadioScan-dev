/* 4-point cubic (Hermite) interpolation, shared by the stream resampler and the synth's pitched playback: much
 * cleaner than a straight line between two samples, which dulls the highs and adds grit. */
#ifndef INTERP_H
#define INTERP_H

/* the value at t (0..1) between x0 and x1, with their neighbours xm1 and x2 */
static inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
    float c = 0.5f * (x1 - xm1), v = x0 - x1, w = c + v, a = w + v + 0.5f * (x2 - x0), b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

#endif
