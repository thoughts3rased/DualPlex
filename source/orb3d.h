#ifndef ORB3D_H
#define ORB3D_H

#include <3ds.h>
#include <citro3d.h>

// Number of audio-spectrum buckets orb3d_draw() expects in band_level[].
#define ORB3D_BANDS 16

// One-time setup: loads the vertex shader, precomputes the sphere mesh and
// creates the offscreen render target it's drawn into. Safe to call more
// than once (a no-op after the first). Must be called after C3D_Init().
void orb3d_init(void);

// Renders one frame of a lit, audio-deformed sphere into its own offscreen
// texture, then composites that texture into the given box on screen_target
// - call this between C2D_SceneBegin(screen_target) and any further
// C2D_Draw*() calls for that target. It redirects drawing to its own
// offscreen target for the 3D pass and calls C2D_SceneBegin(screen_target)
// itself before returning, so screen_target stays the active citro2d target
// (and citro2d's own 2D GPU state is freshly re-established) by the time
// this returns.
//
//   screen_target - the citro2d target currently being drawn to (e.g. what
//                  main.c passed to ui_render_top())
//   x, y, w, h   - destination box, in the caller's current 2D coordinates
//   energy       - overall loudness this frame, 0..1 (drives a slow "breathe")
//   band_level   - ORB3D_BANDS values, 0..1, coarse spectrum split; each
//                  bucket bulges a different patch of the sphere
//   rot_phase    - accumulated rotation, radians (spins the sphere)
//   bump_amount  - 0..1, how strongly band_level bulges the surface
//   light_angle  - radians; where the key light currently sits
//   core_color, rim_color - packed RGBA8 (see RGBA8() in ui.c); alpha is
//                  ignored. rim_color is the material's diffuse/ambient
//                  color, core_color tints the specular highlight.
void orb3d_draw(C3D_RenderTarget* screen_target, float x, float y, float w, float h,
                 float energy, const float band_level[ORB3D_BANDS],
                 float rot_phase, float bump_amount, float light_angle,
                 u32 core_color, u32 rim_color);

#endif // ORB3D_H
