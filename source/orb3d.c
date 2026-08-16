// Renders the Orb visualizer's lit, audio-deformed sphere.
//
// This draws with the raw citro3d 3D pipeline (its own shader, its own
// lighting/culling/depth state) into a small offscreen render target, then
// hands the result back to the caller as an ordinary C2D_Image to composite
// into the 2D UI - the same "render 3D to a texture, then treat it as a 2D
// sprite" technique used for minimaps/previews in many 3DS homebrew titles.
// Doing it this way (rather than drawing 3D straight onto the shared top
// screen target) keeps the temporary GPU state changes (custom shader,
// lighting, depth test, culling) fully contained: nothing here needs to be
// undone in exact reverse order for citro2d's own subsequent 2D draws on the
// *screen* target to keep working, because the screen target's pipeline
// state was never touched in the first place.

#include "orb3d.h"

#include <citro2d.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "vshader_orb_shbin.h"

// Compile-time constant color macro (matches ui.c's - kept local since
// there's no shared header for it).
#define RGBA8(r, g, b, a) ((u32)((((a)&0xFF)<<24) | (((b)&0xFF)<<16) | (((g)&0xFF)<<8) | ((r)&0xFF)))

#define LAT_SEGS 12
#define LON_SEGS 18
#define SPHERE_VERTS (LAT_SEGS * LON_SEGS * 6)
// Wide, not square: matches the visualizer box's aspect closely enough that
// the final composite (which stretches this to fill the box exactly) only
// applies a mild residual stretch, rather than squashing a square render
// into a ~2.8:1 wide box. PICA200 textures need power-of-two dimensions on
// each axis, so this is the closest square-ish ratio to the box.
#define TEX_W 512
#define TEX_H 256
#define TAU 6.2831853f
#define PI 3.14159265f

typedef struct { float position[3]; float texcoord[2]; float normal[3]; } A3DVertex;

static bool s_inited = false;

static DVLB_s* s_vshader_dvlb;
static shaderProgram_s s_program;
static int s_uLoc_projection, s_uLoc_modelView;

static C3D_Tex s_tex;
static C3D_RenderTarget* s_target;
static Tex3DS_SubTexture s_subtex;
static C2D_Image s_image;

static C3D_LightEnv s_lightEnv;
static C3D_Light s_light;
static C3D_LightLut s_lut_phong;

// Precomputed once: each duplicated triangle-list vertex's direction on the
// unit sphere (both its rest position and its lighting normal, since the
// per-frame bulge displacement is gentle enough that the undisplaced normal
// stays a good approximation).
static float s_base_dir[SPHERE_VERTS][3];

// Precomputed once: ORB3D_BANDS points spread evenly over the sphere (a
// Fibonacci sphere distribution) that each frame's band_level[] bulges
// outward from, giving the surface an audio-reactive, organic lumpiness.
static float s_bump_dir[ORB3D_BANDS][3];

static A3DVertex* s_vbo; // linearAlloc'd once, contents rebuilt every frame

static void build_base_sphere(void) {
    int idx = 0;
    for (int i = 0; i < LAT_SEGS; i++) {
        float phi0 = (PI * i) / LAT_SEGS;
        float phi1 = (PI * (i + 1)) / LAT_SEGS;
        for (int j = 0; j < LON_SEGS; j++) {
            float th0 = (TAU * j) / LON_SEGS;
            float th1 = (TAU * (j + 1)) / LON_SEGS;

            float p00[3] = { sinf(phi0) * cosf(th0), cosf(phi0), sinf(phi0) * sinf(th0) };
            float p01[3] = { sinf(phi0) * cosf(th1), cosf(phi0), sinf(phi0) * sinf(th1) };
            float p10[3] = { sinf(phi1) * cosf(th0), cosf(phi1), sinf(phi1) * sinf(th0) };
            float p11[3] = { sinf(phi1) * cosf(th1), cosf(phi1), sinf(phi1) * sinf(th1) };

            const float* tri[6] = { p00, p10, p11, p00, p11, p01 };
            for (int k = 0; k < 6; k++) {
                s_base_dir[idx][0] = tri[k][0];
                s_base_dir[idx][1] = tri[k][1];
                s_base_dir[idx][2] = tri[k][2];
                idx++;
            }
        }
    }
}

static void build_bump_centers(void) {
    // Standard Fibonacci-sphere point distribution - deterministic, no RNG
    // needed, spreads ORB3D_BANDS points roughly evenly over the sphere.
    const float golden = PI * (3.0f - sqrtf(5.0f));
    for (int i = 0; i < ORB3D_BANDS; i++) {
        float t = ((float)i + 0.5f) / ORB3D_BANDS;
        float phi = acosf(1.0f - 2.0f * t);
        float theta = golden * (float)i;
        s_bump_dir[i][0] = sinf(phi) * cosf(theta);
        s_bump_dir[i][1] = cosf(phi);
        s_bump_dir[i][2] = sinf(phi) * sinf(theta);
    }
}

static void unpack_rgb(u32 c, float* r, float* g, float* b) {
    *r = (c & 0xFF) / 255.0f;
    *g = ((c >> 8) & 0xFF) / 255.0f;
    *b = ((c >> 16) & 0xFF) / 255.0f;
}

void orb3d_init(void) {
    if (s_inited) return;
    s_inited = true;

    build_base_sphere();
    build_bump_centers();
    s_vbo = (A3DVertex*)linearAlloc(sizeof(A3DVertex) * SPHERE_VERTS);

    // Shader
    s_vshader_dvlb = DVLB_ParseFile((u32*)vshader_orb_shbin, vshader_orb_shbin_size);
    shaderProgramInit(&s_program);
    shaderProgramSetVsh(&s_program, &s_vshader_dvlb->DVLE[0]);
    s_uLoc_projection = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");
    s_uLoc_modelView  = shaderInstanceGetUniformLocation(s_program.vertexShader, "modelView");

    // Offscreen render target (with a depth buffer - this is real 3D
    // geometry, not a flat 2D fan, so it needs actual depth testing).
    C3D_TexInitVRAM(&s_tex, TEX_W, TEX_H, GPU_RGBA8);
    s_target = C3D_RenderTargetCreateFromTex(&s_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);

    // Full-texture, non-rotated subtexture so it composites right-side-up
    // via citro2d (top > bottom is citro2d/tex3ds's "normal" orientation).
    s_subtex.width = TEX_W;
    s_subtex.height = TEX_H;
    s_subtex.left = 0.0f;
    s_subtex.top = 1.0f;
    s_subtex.right = 1.0f;
    s_subtex.bottom = 0.0f;
    s_image.tex = &s_tex;
    s_image.subtex = &s_subtex;

    // Fixed-function lighting: a single point light, Phong-ish specular via
    // a lookup table (see devkitPro's graphics/gpu/fragment_light example,
    // which this whole module's lighting setup mirrors).
    C3D_LightEnvInit(&s_lightEnv);
    LightLut_Phong(&s_lut_phong, 30);
    C3D_LightEnvLut(&s_lightEnv, GPU_LUT_D0, GPU_LUTINPUT_LN, false, &s_lut_phong);
    C3D_LightInit(&s_light, &s_lightEnv);
    C3D_LightColor(&s_light, 1.0f, 1.0f, 1.0f);
}

void orb3d_draw(C3D_RenderTarget* screen_target, float x, float y, float w, float h,
                 float energy, const float band_level[ORB3D_BANDS],
                 float rot_phase, float bump_amount, float light_angle,
                 u32 core_color, u32 rim_color) {
    orb3d_init();

    // --- Rebuild the deformed sphere's vertex buffer on the CPU ---
    // base_radius and the multiplier cap below are sized together against
    // the camera setup further down (38 deg... see Mtx_Persp call) so the
    // sphere can never bulge past the offscreen texture's own edges, no
    // matter how bump_amount/bulge/wobble combine - that used to clip.
    float base_radius = 0.45f + 0.03f * energy; // slow "breathing" with loudness
    for (int i = 0; i < SPHERE_VERTS; i++) {
        const float* dir = s_base_dir[i];

        float bulge = 0.0f;
        for (int b = 0; b < ORB3D_BANDS; b++) {
            float d = dir[0] * s_bump_dir[b][0] + dir[1] * s_bump_dir[b][1] + dir[2] * s_bump_dir[b][2];
            if (d > 0.3f) {
                float f = (d - 0.3f) / 0.7f;
                bulge += band_level[b] * (f * f);
            }
        }
        if (bulge > 1.0f) bulge = 1.0f; // cap the (rare) case of several bands overlapping at one point

        // A cheap, always-on ripple (two overlapping oscillators over the
        // surface) so the surface reads as organic rather than a perfectly
        // smooth ball even during quiet passages - the audio-driven bulge
        // above then layers on top of this. Driven by light_angle (which
        // advances at a fixed rate) rather than rot_phase, so it stays
        // lively even during a slow-rotation phase instead of animating at
        // whatever speed the sphere happens to be spinning this phase.
        float wobble = 0.07f * sinf(dir[0] * 3.5f + dir[1] * 2.2f + light_angle * 2.6f)
                             * cosf(dir[2] * 3.0f - light_angle * 1.7f);

        float mult = 1.0f + bump_amount * bulge * 1.4f + wobble;
        if (mult > 1.6f) mult = 1.6f; // hard cap - keeps the sphere inside the camera frustum always
        float r = base_radius * mult;

        A3DVertex* v = &s_vbo[i];
        v->position[0] = dir[0] * r;
        v->position[1] = dir[1] * r;
        v->position[2] = dir[2] * r;
        v->texcoord[0] = 0.0f;
        v->texcoord[1] = 0.0f;
        v->normal[0] = dir[0];
        v->normal[1] = dir[1];
        v->normal[2] = dir[2];
    }
    GSPGPU_FlushDataCache(s_vbo, sizeof(A3DVertex) * SPHERE_VERTS);

    // --- Render the sphere into the offscreen target ---
    // Flush first: citro2d batches its draw calls and only actually submits
    // them on its own C2D_Flush()/C2D_SceneBegin() - redirecting the active
    // draw target with C3D_FrameDrawOn() directly, without flushing first,
    // would leave anything the caller already drew this frame (e.g. this
    // box's background rect) stranded mid-batch.
    C2D_Flush();
    C3D_FrameDrawOn(s_target);
    C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, RGBA8(0x0C, 0x0C, 0x18, 0xFF), 0);

    C3D_BindProgram(&s_program);

    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0 = position
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v1 = texcoord (unused)
    AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 3); // v2 = normal

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, s_vbo, sizeof(A3DVertex), 3, 0x210);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_FRAGMENT_PRIMARY_COLOR, GPU_FRAGMENT_SECONDARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_ADD);

    float rim_r, rim_g, rim_b, core_r, core_g, core_b;
    unpack_rgb(rim_color, &rim_r, &rim_g, &rim_b);
    unpack_rgb(core_color, &core_r, &core_g, &core_b);
    C3D_Material material = {
        { rim_r * 0.25f, rim_g * 0.25f, rim_b * 0.25f },   // ambient
        { rim_r * 0.85f, rim_g * 0.85f, rim_b * 0.85f },   // diffuse
        { core_r, core_g, core_b },                         // specular0
        { 0.0f, 0.0f, 0.0f },                                // specular1
        { 0.0f, 0.0f, 0.0f },                                // emission
    };
    C3D_LightEnvBind(&s_lightEnv);
    C3D_LightEnvMaterial(&s_lightEnv, &material);

    C3D_FVec lightVec = FVec4_New(cosf(light_angle) * 3.0f, 1.4f, sinf(light_angle) * 3.0f, 1.0f);
    C3D_LightPosition(&s_light, &lightVec);

    C3D_CullFace(GPU_CULL_NONE); // depth test alone resolves visibility - see header comment
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);

    C3D_Mtx projection, modelView;
    Mtx_Persp(&projection, C3D_AngleFromDegrees(40.0f), (float)TEX_W / TEX_H, 0.1f, 10.0f, false);
    Mtx_Identity(&modelView);
    Mtx_Translate(&modelView, 0.0f, 0.0f, -2.4f, true);
    Mtx_RotateY(&modelView, rot_phase, true);
    Mtx_RotateX(&modelView, 0.35f, true); // slight tilt so the "pole" isn't dead-center

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLoc_projection, &projection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLoc_modelView, &modelView);

    C3D_DrawArrays(GPU_TRIANGLES, 0, SPHERE_VERTS);

    // Leave the GPU in the state citro2d's own 2D draws expect - see this
    // file's top comment.
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_GREATER, GPU_WRITE_ALL);
    C3D_LightEnvBind(NULL);

    // --- Hand drawing back to the real screen target, then composite ---
    // C2D_Prepare() rebinds citro2d's own shader - its header is explicit
    // that this is only a one-time setup call "if citro2d is the sole user
    // of the GPU". It isn't here: C3D_BindProgram() above swapped the GPU's
    // shader unit to ours, and nothing else in citro2d rebinds its shader
    // per-draw, so every citro2d draw for the rest of the program (both
    // screens, every frame after) would otherwise silently keep using our
    // sphere shader instead of citro2d's, producing garbage/blank output.
    C2D_Prepare();
    C2D_SceneBegin(screen_target);

    // Fill the whole visualizer box, same as the other styles' background -
    // the sphere itself can't touch the box edges regardless (its bulge is
    // capped well inside the render's own frustum - see the mult clamp
    // above), so there's no need to additionally inset the destination rect.
    float scale_x = w / (float)TEX_W;
    float scale_y = h / (float)TEX_H;
    C2D_DrawImageAt(s_image, x, y, 0.5f, NULL, scale_x, scale_y);
}
