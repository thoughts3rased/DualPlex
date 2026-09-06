#ifndef FFT_H
#define FFT_H

// Minimal iterative radix-2 Cooley-Tukey FFT, just enough for the audio
// visualizers (the Spectrum style in ui.c). No allocation, no external
// deps - the twiddle factors are generated on the fly with an incremental
// rotation per stage, which drifts a little over a 1024-point transform
// but is far below what a 130-pixel-tall bar plot could ever show.

// Largest transform fft_forward() will be asked to do. Sized for the
// visualizer snapshot (see VIS_SNAPSHOT_SAMPLES in audio_player.c).
#define FFT_MAX_N 1024

// In-place forward FFT of n complex samples (n must be a power of two,
// 2 <= n <= FFT_MAX_N). re[] / im[] hold the input on entry and the
// spectrum on return; for real input, pass im[] pre-zeroed. Only bins
// 0 .. n/2 are meaningful for real input (the rest mirror them).
void fft_forward(float* re, float* im, int n);

#endif // FFT_H
