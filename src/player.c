/*
 * DSVP — Dead Simple Video Player
 * player.c — Demux, video decode, display, seeking, media info
 *
 * Threading model:
 *   - Demux thread: reads packets from the container, pushes to queues
 *   - Main thread:  pops video packets, decodes, scales, renders
 *   - SDL audio thread: calls audio_callback() which decodes audio
 *
 * A/V sync strategy:
 *   Audio is the master clock. Video frame display timing is adjusted
 *   to match the audio clock. This is the standard approach (same as
 *   ffplay) because audio glitches are far more perceptible than
 *   dropped/delayed video frames.
 *
 * Rendering (v0.1.4 — SDL_GPU):
 *   Video frames are uploaded to GPU textures (R8_UNORM per plane for
 *   8-bit, R16_UNORM/R16G16_UNORM for 10-bit passthrough) and converted
 *   YUV→RGB by custom HLSL fragment shaders compiled at runtime via
 *   SDL3_shadercross. This replaces SDL_Renderer and unlocks HDR10
 *   and further GPU-side processing.
 */

#include "dsvp.h"

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>


/* ═══════════════════════════════════════════════════════════════════
 * Blue Noise Dither Texture (64×64, void-and-cluster algorithm)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Generated via the void-and-cluster algorithm (Ulichney 1993).
 * All spectral energy is concentrated in high frequencies — the human
 * visual system is least sensitive to high-frequency noise, making
 * blue noise dither perceptually invisible even at ±0.5 LSB amplitude.
 *
 * Uploaded once at startup as R8_UNORM (0-255 → 0.0-1.0 in shader).
 * Fragment shader tiles with frac(screen_pos / 64.0) for seamless wrap.
 */
static const uint8_t blue_noise_64[4096] = {
     53, 245,  22, 216, 180,  41, 124, 223, 111,  64, 205, 102, 177, 251,  54, 193, 123,  86, 154,  50,  78,  23,  94, 128,  70, 243,  86, 215,  26, 204,  64, 120,  47,  20, 236,  41, 192, 216, 132,  10,  76, 140,  18,  69, 225,  96, 242,  62, 179,  47, 163,  72, 200, 178,  79, 205,  50,  98, 138, 176,  35,  77, 215, 172,
     84, 185, 137,  58, 146, 198,  75, 173,   8, 185,  42, 227,   2, 153,  92, 218,  35, 233,  12, 210, 195, 169, 249, 188,  40, 165,   3, 109, 159,  96,  18, 187,  85, 201, 142, 173,  87, 107,  51, 222, 161, 112, 207, 176, 117,  25, 171, 131, 216,  84, 246, 138, 103,   2, 130, 157,  30, 245,  65, 223, 151, 193,  27, 108,
    221,   8, 112, 239,  87,  15, 236,  48, 156, 233, 120, 141,  79, 211,  21, 137,  71, 171, 102, 132,  63, 116,   7, 149, 220, 121, 192, 228,  52, 253, 129, 231, 160, 108,  63,   3, 230, 155,  30, 181,  93, 252,  37,  55, 139, 210,  78,  36, 109,   7, 187,  33, 231, 212,  25, 236, 117, 166,  15, 104,  52, 128, 233, 147,
     39, 160, 210,  31, 167, 106, 207, 130,  94,  69,  22, 171,  46, 182, 105, 240, 189,  47, 253, 162,  37, 229,  82,  51, 100,  28,  65, 141,  35, 174,  72,   7, 212,  33, 251, 122, 205,  71, 244, 119,  61,   2, 187,  87, 238,  10, 151, 251, 202, 159,  95, 127,  64, 152,  87,  48, 192,  89, 211, 182, 247,   2,  93,  62,
    253,  99,  73, 129, 189,  54, 148,  32, 243, 188, 213,  97, 247, 127,  60, 156,   5, 126,  89,  17, 213, 180, 137, 238, 205, 155, 245,  85, 116, 197, 149, 103,  53, 138, 177,  95,  45, 137,  15, 193, 149, 230, 130, 163, 104, 196,  48, 120,  65,  24, 237,  49, 196, 110, 180, 217,  71, 143,  39, 122,  80, 163, 205, 179,
    132, 200,  47, 227,   2, 251,  72, 175,  10, 117,  50, 147,   9, 192,  33, 228,  80, 219, 201, 150,  66,  97,  23, 168,  74,   8, 178, 211,  13, 223,  29, 244, 193,  80, 224,  24, 195, 167, 104, 217,  34,  76, 203,  20,  66, 176, 231,  86, 185, 134, 210, 170,  17, 253,  31, 135,   5, 241, 161,  19, 219,  45, 112,  21,
     76,  13, 175, 145, 113,  90, 210, 125, 223,  85, 164, 232,  75, 207,  95, 118, 179,  56,  31, 110, 246, 129, 198,  41, 115, 133,  54, 102, 146,  60,  90, 169, 115,  11, 156, 126,  76, 235,  53,  88, 175, 113,  48, 245, 126,  31, 143,   3, 224,  42,  80, 103, 144,  76, 164,  99, 224,  62, 103, 195, 136,  67, 239, 152,
    224, 105, 240,  67, 201,  21, 163,  38,  64, 194,  18, 110,  39, 136, 160,  21, 242, 140, 167, 188,  48,   2, 221,  89, 251, 188, 230,  22, 166, 236, 132,  44, 221,  66, 242,  40, 210,   4, 145, 251,  16, 141, 220, 158,  80, 212,  98, 166, 111, 155, 246,   6, 203, 228,  53, 127, 201, 171,  43, 250,  88, 173,  32, 190,
     54, 128,  28, 165,  44, 135, 234, 105, 151, 246, 133, 215, 179, 251,  52, 212,  73,  10,  95, 228,  79, 144, 175,  62, 157,  29,  81, 199, 118,  74, 189,  23, 144, 187,  94, 176, 107, 163, 124, 197,  66, 184,  93,   8, 191,  44, 252,  59, 206,  29, 130,  66, 117,  35, 180,  11,  85,  23, 119, 148,   7, 229, 119,  92,
    208, 150, 217,  97, 248, 186,  76,   5, 181,  54,  32,  98,  68,   2,  90, 173, 114, 203, 128,  27, 206, 240, 100,  17, 216, 111, 148,  43, 216,   2, 249, 102, 210,  53, 131,  26, 232,  47,  82,  29, 228, 121,  39, 241, 109, 131, 182,  15,  78, 172, 231, 187, 156,  90, 244, 209, 151, 237, 184,  75, 212,  57, 160,  11,
     38, 180,  79,   7, 118,  57, 221, 127, 209,  84, 231, 169, 202, 130, 235, 150,  40, 249,  53, 160,  67, 120,  37, 137, 185,  58, 238, 175,  98,  57, 153, 171,  82,   7, 253, 150,  71, 183, 209, 104, 164,  58, 213, 150,  27,  69, 145, 224, 122,  95,  45,  20, 221,  58, 136, 106,  67,  46, 220,  29, 107, 192, 134, 249,
    109,  59, 231, 142, 173,  32, 155,  99,  21, 147, 117,  17, 156,  45, 108,  24, 190,  86, 142, 182,  16, 196, 166, 232,  77,   9, 126,  25, 138, 230, 122,  38, 224, 116, 198,  90, 221,  12, 141, 248,   7, 134,  79, 199, 170, 237,  93,  37, 199, 243, 149, 107, 197,  13, 164,  28, 179, 128,  91, 141, 170,  41,  78, 201,
    164, 126,  27, 206,  71, 197, 238,  49, 192, 252,  61, 213,  78, 226, 182,  65, 229,   5, 216, 105, 244,  87,  51, 106, 198, 253,  92, 212, 189,  80,  16, 194,  65, 141,  21, 168,  60, 125,  42,  89, 180, 235,  34, 102,   2,  51, 210, 156,  11,  60, 185,  74, 125, 253,  80, 213, 239,  15, 197, 252,   2, 230, 100,  20,
    239, 185,  90, 246, 107,  15, 133,  81, 172,  39, 103, 184, 126,   8, 144,  98, 165, 121,  72,  37, 135, 209,   3, 157,  33, 144, 167,  67,  35, 159, 245,  93, 179, 238,  45, 111, 206, 242, 158, 196,  52, 113, 155, 226, 138, 186, 114,  75, 179, 132,  29, 229,  42, 176, 101,  53, 117, 154,  70,  49, 123, 204, 149,  66,
    138,   4,  52, 149,  39, 167, 217, 113,   3, 225, 139,  30, 242,  57, 197, 253,  42, 207, 154, 234,  62, 170, 121, 223,  59, 117,  18, 223, 112,  55, 204, 132,  10, 158,  77, 188,  30, 100,  16,  73, 214,  19, 175,  59,  84, 253,  19, 218, 100, 247,  86, 208, 147,   2, 137, 191,  36, 224, 106, 184,  85, 166,  48, 219,
     81, 195, 115, 222, 190,  93,  55, 240, 156,  73, 206,  90, 166, 116,  83,  18, 134,  93,  12, 186,  24,  99, 249,  75, 182, 206,  85, 178, 238, 146,  27, 109,  60, 212, 124, 229, 146, 178, 225, 120, 143, 245,  95, 201,  37, 123, 161,  44, 144,   7, 167, 110,  64, 202, 233,  74, 163,  11, 208, 145,  25, 235,  12, 111,
     36, 253, 170,  68,   9, 137, 202,  20, 127,  50, 178,  12, 219,  37, 231, 180,  56, 171, 217,  82, 144, 194,  36, 136,  11, 240,  41, 129,   2,  89, 218, 177, 252,  34,  91,   2,  68,  48,  87, 167,  38,  63, 132,  10, 223, 182,  68, 230, 196,  57, 222,  21, 161,  95,  27, 127, 240,  91,  41, 247,  61, 119, 180, 207,
    159,  98,  22, 124, 243,  78, 162, 102, 188, 249, 112, 151,  63, 131, 155, 206, 108, 244,  36, 120, 239,  55, 215,  90, 161, 104, 148,  70, 199, 165,  46,  74, 142, 196, 160, 241, 134, 193, 253,   6, 208, 184, 230, 109, 146,  89,  21, 106, 129,  81, 182, 121, 249,  48, 215, 185,  55, 115, 172, 130, 193,  94, 144,  69,
    214,  52, 227, 154,  45, 208,  27, 227,  40,  86,  24, 234, 189,  97,   2,  72,  22, 141,  67, 165,   4, 105, 173,  23, 230,  54, 183, 251,  33, 118, 237, 102,  13, 114,  52, 208,  98,  29, 117, 152, 103,  83,  30, 170,  52, 197, 245, 173,  27, 238,  41, 144,  71, 171, 106,   8, 153, 203,  75,   3, 226,  33, 238,  16,
    106, 134, 194,  89, 177, 107, 147,  67, 171, 215, 138,  75,  46, 211, 250, 124, 195, 223,  95, 191, 227, 149,  68, 193, 116, 207,  10,  97, 217, 145,  23, 189, 226, 173,  80,  17, 170, 218,  71, 235,  49, 137, 248,  73, 212,   3, 135,  49, 153, 205,  96, 192,  18, 234, 135,  84, 250,  29, 219, 103, 162,  54,  85, 186,
    244,   2,  74,  31, 216,  12, 252, 131,  97,   4, 196, 118, 176,  26, 160,  84,  34, 153,  51,  17, 127,  43, 253, 138,  38,  79, 133, 173,  52,  83, 162,  63, 131,  36, 235, 122, 147,  44, 189,  13, 161, 200,  22, 120, 158, 101,  76, 216, 110,  64,   4, 225, 115,  39, 197,  62, 177, 141,  46, 184, 134, 206, 121, 152,
     43, 168, 231, 142, 119,  63, 189,  47, 231, 158,  37, 246, 143, 105,  55, 233, 178, 107, 247, 208,  76, 185,  92,  18, 236, 156, 225,  25, 124, 202, 248,   4, 216,  97, 197,  61, 249,  86, 132, 221, 113,  62, 180, 218,  38, 233, 186,  15, 247, 177, 130, 155,  83, 164, 213,  22, 120,  93, 239,  68,  14, 250,  25,  66,
    126, 204, 101,  54, 240, 153,  91,  21, 201, 110,  59,  89,  11, 224, 193, 131,  10,  64, 136, 168,  33, 112, 218, 171,  61, 105, 193,  72, 232,  37, 105, 140,  74, 157,  14, 181, 106,  20, 175,  39,  94, 241,   9,  85, 143,  59, 163, 125,  85,  45, 202,  31, 253,  59, 104, 149, 233,   6, 199, 115, 157,  93, 174, 224,
     81,  19, 157, 195,   7, 179, 218, 125,  70, 177, 237, 208, 154,  70,  38,  97, 200, 218,  25,  89, 237, 151,  10, 126, 204,  44,   6, 146, 180,  88, 166, 194,  49, 240, 125,  39, 206, 152, 228,  74, 208, 165, 131, 107, 254,  22, 204,  35, 147, 234,  70, 109, 186,   9, 226,  42, 190,  80, 165,  34, 230,  50, 110, 190,
     38, 254,  88,  42, 114,  76,  34, 247, 151,  14, 133,  29, 186, 117, 171, 252,  78, 161, 119, 191,  52, 201,  71, 243,  88, 167, 251, 116,  57,  15, 243,  29, 112, 214, 169,  83, 234,  58, 123,   2, 143,  30,  49, 197, 172,  80, 115, 228,  96, 169,  19, 221, 148, 123,  70, 173, 129,  55, 213, 142,  77, 202,   5, 142,
    217, 172, 134, 212, 234, 139, 169, 101,  52, 225,  78, 100,  51, 235,   3, 134,  33,  56, 244,   5, 139, 100,  38, 156,  27, 132, 211,  96, 198, 219, 125, 152,  85,   9,  64, 143,  26, 100, 198, 245,  97, 182, 237,  69,  14, 219, 159,  65,   6, 196, 136,  82,  39, 208,  95, 246,  13, 109, 240,  18, 125, 170, 243,  64,
    115,  11,  59, 184,  17,  62, 208,   1, 199, 115, 170, 204, 145, 215,  83, 157, 225, 108, 176,  82, 227, 168, 213, 113, 189,  75,  49,  19, 158,  78,  46, 228, 174, 196, 254, 110, 185, 154,  43, 168,  56, 118, 215, 149, 101, 133,  47, 187, 250, 105,  52, 239, 182, 158,  26, 140, 203, 163,  92,  62, 226,  40,  94, 158,
     77, 232, 125,  95, 155, 109, 241,  84, 157,  24, 254,  41,  17, 108,  60, 187,  19, 205, 147,  37, 123,  18,  59, 235,   1, 220, 173, 236, 136, 187,   4, 102,  57, 128,  41, 210,  11, 238,  86,  19, 207,  78,   6,  42, 176, 244,  28, 121, 150,  25, 213, 120,   4,  62, 218,  84,  51,  33, 195, 147, 186, 112,  23, 207,
     49, 191,  31, 248,  45, 192,  29, 126, 185,  55, 137,  75, 192, 163, 243, 121,  48,  95,  65, 236, 199,  77, 177, 134,  90, 147, 110,  31,  62, 113, 203, 247, 148,  24,  91, 165,  72, 126, 193, 140, 252, 161, 129, 233,  87, 195,  67, 223,  86, 177,  68, 165,  98, 249, 114, 181, 229, 122, 244,   7,  82, 215, 136, 178,
    106, 146, 214, 167,  82, 222, 145,  68, 235,  97, 215, 118, 229,  88,  11, 211, 140, 250, 181,  25, 154, 101, 254,  41, 202,  54, 248,  86, 213, 165,  35,  81, 177, 215, 235, 139,  50, 226, 106,  64,  29, 102, 190,  59,  17, 112, 144,   8, 205,  43, 231, 135, 195,  42, 153,  13,  70, 159, 103,  57, 168,  44, 250,   3,
    234,  87,  18,  57, 116,   8, 176,  38, 207,   6, 174,  22,  56, 150,  40, 172,  75,   1, 111, 130,  49, 212,  10, 117, 161,  19, 188, 130,  12, 240, 142, 119,  18,  61, 110, 190,  30, 178,   9, 218, 170,  45, 227, 152, 214, 169, 240,  97, 163, 116,  12,  83,  27, 211,  90, 134, 194,  28, 209, 139, 228, 125,  73, 159,
     60, 129, 188, 140, 205, 243, 101, 121, 158,  79, 142, 247, 188, 129, 202, 105, 230, 160, 221, 194,  72, 169, 136, 229,  84, 221,  68, 153,  98,  50,  71, 228, 198, 159,   1,  82, 249,  97, 148, 119, 200,  91,  23, 121,  73,  33,  50, 133,  62, 254, 183, 144, 241,  61, 174, 234,  47, 254,  92,  12, 191,  30, 100, 201,
    224,  38, 254,  75, 162,  25,  61, 196, 239,  49, 111,  31,  96,  70, 242,  28,  57,  91,  42,  18, 232,  94,  34,  60, 195, 114,  29, 233, 168, 208, 183,  28,  90, 243, 135, 204, 162,  58,  36, 241,  67, 138, 178, 250,  95, 207, 177, 225,  28, 199, 105,  48, 161, 119,   1, 108,  77, 149, 179,  64, 114, 242, 176,  16,
    116, 171,   9, 109,  44, 224,  92, 135,  17, 183, 213, 162, 222,  13, 154, 121, 183, 208, 152, 108, 183, 143, 246, 172,  13, 145, 181,  44, 123,   4, 109, 153, 122,  41,  70,  26, 124, 219, 185,  86,   3, 231,  54,  16, 148, 125,   1,  88, 155,  75,  16, 226,  92, 193, 221,  34, 214, 127,  20, 225, 160,  81,  50, 148,
     66,  97, 217, 196, 127, 181, 155,  36, 219,  87,  66, 123,  45, 193,  85, 235,   8, 134,  68, 243,  53,   5,  77, 110, 209,  93, 252,  75, 217,  87, 248,  58, 191, 222, 172, 237, 104,  13, 155, 128, 204, 158, 108, 219, 198,  64, 246, 190, 118, 211, 129, 175,  23,  71, 136, 159, 184,  54, 101, 202,  37, 138, 219, 198,
    237, 143,  54,  83, 241,   1,  70, 251, 107, 150,   5, 178, 249, 139,  59, 171, 102, 220,  28, 164, 122, 197, 218, 155,  32,  56, 132,  17, 149, 198,  35, 163,  19,  98, 143,  50, 189,  73, 254,  47,  99,  30, 185,  80,  40, 166, 104,  52,  33, 242,  60, 146, 232,  44, 249,  88,   9, 238, 152,  75, 250,   1,  92,  28,
    187,  13, 164,  30, 148,  99, 206, 172,  54, 201, 229,  94,  22, 109, 216,  31,  50, 190,  79, 227,  96,  43, 133,  66, 238, 188, 222, 105, 179,  63, 127, 232,  72, 202,  11,  86, 216, 119,  21, 174, 223,  66, 242, 118, 139,  11, 233, 151, 182,  85,   6, 201,  95, 124, 203,  59, 118, 196,  24, 125, 183, 112, 168, 124,
     74, 248, 113, 191, 230,  41, 120,  16, 139,  32, 125,  62, 161, 200,  76, 130, 254, 111, 149,  20, 185, 249,  23, 175, 101,   1, 162,  45, 243,  20, 101, 211, 139, 112, 246, 169,  35, 150, 202,  83, 131, 151,  19, 173, 225,  73, 204,  24, 135, 225, 107, 160,  30, 179,  14, 147, 227,  69, 174,  43, 213,  61, 230,  42,
    138,  93, 220,  63, 132,  77, 186, 237,  82, 167, 241, 189,  42, 226,   1, 157, 179,  12, 208,  57, 142,  85, 115, 225, 148,  81, 123, 204,  90, 144, 168,   5,  43, 183,  61, 123, 233, 100,  59, 244,   8, 214,  56,  96,  34, 187,  91, 116,  67, 168,  44, 252,  70, 217, 103, 166,  34,  96, 240, 146,  84,  22, 154, 206,
    173,  49,  24, 178,  10, 216, 157,  58, 211, 101,   9,  72, 113, 141,  88, 236,  64,  92, 124, 237, 166,   6, 210,  51,  33, 197,  61,  26, 220,  68, 194, 254,  84, 227,  28, 160,   1, 191,  39, 122, 166, 105, 194, 251, 156, 127,  47, 247, 209,  14, 190, 119, 143,  48, 242,  74, 212, 133,   5, 110, 192, 247, 103,   7,
    119, 202, 158, 108, 242,  94,  26, 116,  40, 136, 203, 154, 250,  29, 187, 119,  43, 220, 194,  38, 106,  65, 189, 137, 252, 166, 229, 132, 174,  35, 118,  56, 130, 150,  96, 200,  79, 141, 226, 183,  68,  26, 139,  78,   1, 214, 166,  20, 149,  97, 222,  81,   1, 199, 128,  17, 189,  55, 171, 220,  51, 136,  69, 227,
     34, 252,  76, 139,  53, 196, 143, 254, 181, 224,  22,  91, 170,  57, 207,  17, 162, 133,  25,  77, 175, 244, 122,  96,  18,  85, 110,   8, 242,  98, 160, 210,  24, 178, 219,  45, 241, 108,  23,  91, 211, 237,  42, 111, 226,  65, 102, 192,  53, 131,  35, 157, 234, 175,  88, 156, 106, 254,  80,  30, 160,  14, 186,  89,
     59, 181,   5, 212,  32, 169,  72,   1,  88,  61, 127,  44, 220, 130,  77, 228, 101, 251, 184, 152, 217,  15,  40, 161, 207,  55, 184, 152,  72, 199,  14, 239, 108,  71,  12, 124,  63, 177, 152,  51, 127, 162, 197, 146, 176,  33, 136, 239,  76, 231, 184,  64, 104,  32,  58, 229,  24, 139, 205, 123, 234, 107, 212, 146,
    123, 154,  98, 236, 130, 112, 209, 232, 153, 174, 245, 194, 107,   4, 181, 143,  50,  70,   5, 116,  90, 139, 228,  70, 239, 131,  31, 216,  46, 141,  87,  41, 185, 143, 250, 168, 213,  15, 200, 254,   5,  83,  61,  15, 248,  88, 204,   9, 169, 117,  21, 215, 137, 249, 123, 202, 164,  41,  66,  92, 175,  55,  27, 238,
     19, 222,  46,  80, 186,  58,  18, 100,  37, 117,  13,  74, 161, 233,  93,  29, 170, 205, 235,  43, 197,  58, 172, 113,   8, 192,  99, 250, 114, 170, 227, 133,  55, 201,  81,  37,  96, 137,  78, 111, 186, 231, 102, 209, 118,  52, 155, 106,  40, 205,  91, 163,  13, 191,  83,   4,  99, 182, 241,  10, 200, 140,  83, 192,
     72, 109, 199, 163,  25, 245, 135, 178, 201,  83, 213, 142,  37,  62, 208, 245, 108, 129,  86, 146, 222,  22,  93, 203, 147,  79, 163,  63,   1, 209,  25, 104, 232,   4, 114, 160, 234,  49, 221,  26, 145,  36, 131, 164,  27, 191, 228,  69, 252, 147,  53, 232,  73,  43, 151, 210,  53, 222, 127, 156,  36, 251, 116, 164,
     41, 247,  10, 121, 218, 154,  73, 229,  50, 129,  30, 240, 185, 126, 150,  48,   9, 192,  27,  65, 176, 123, 253,  35,  52, 234,  26, 198, 127,  81, 187,  68, 150, 174, 209,  26, 128, 189, 157,  63, 176, 218,  56, 244,  76, 140,  17, 175, 123,   6, 198, 110, 131, 177, 227, 112, 140,  27,  77, 104, 218,  65,   3, 211,
    129, 177, 144,  65,  92,  38, 111,  12, 150, 221, 169, 104,  85,  14, 196,  77, 166, 230, 153, 244, 101,   1, 154, 215, 133, 176, 104, 152, 241,  49, 161, 248,  36,  94,  60, 243,  75,   6,  99, 247, 114,  86,  10, 183,  99, 217,  50,  94, 210,  79, 180,  24, 245,  89,  15,  63, 251, 174, 197,  48, 170, 134, 186,  99,
    220,  82,  34, 241, 206, 165, 193, 254,  94,  67,   3, 201,  55, 252, 115, 221,  98,  58, 117,  40, 195,  78, 182,  63,  88,  13, 220,  35,  91, 218, 133,  20, 121, 221, 138, 181, 109, 229,  40, 199,  20, 135, 211, 154,  40, 128, 238, 145,  31, 241, 138,  65, 157,  38, 199, 162,  94,   8, 118, 239,  16,  89, 234,  54,
     14, 198, 159, 103,   0,  56, 126,  24, 182, 119, 156, 233, 138, 175,  42,  19, 137, 189,  14, 213, 128, 229,  26, 115, 242, 189, 124,  71, 172,  11, 102, 203,  78, 188,  11,  44, 164, 206, 142,  79, 164, 236,  66, 107, 200,   0, 187,  66, 167, 103,  47, 217, 188, 108, 230, 128,  40, 211, 142,  67, 214, 160,  31, 149,
    119,  61, 226, 132, 180, 235,  74, 211,  46, 226,  34,  76,  21,  96, 153, 204, 246,  86, 173,  69, 159,  49, 140, 205,  43, 150,  54, 238, 141, 184,  45, 237, 165,  56, 253,  91, 128,  23,  58, 187, 122,  44, 179,  23, 254,  83, 113, 220,  17, 200, 120,   3,  81, 144,  18,  72, 237, 183,  84,  22, 187, 110,  75, 250,
     94, 188,  22,  78,  42, 152, 102, 137, 167,  91, 193, 131, 214, 186,  67, 121,  51,  31, 234,  96,   8, 248,  81, 167,   5, 101, 200,  24, 108, 214,  67, 145,  16, 114, 151, 212,  68, 225, 104, 250,   7,  95, 222, 140,  57, 152, 174,  43,  90, 148, 227, 169, 255,  50, 218, 170, 107,  52, 157, 243, 130,  47, 204, 174,
     42, 151, 243, 114, 223, 202,  29, 248,   7,  63, 238, 106,  45, 242,   6, 224, 163, 109, 142, 219, 122, 193, 108, 222,  74, 255, 159,  84, 228,   7, 123, 196,  97, 223,  35, 190,   0, 171, 147,  37, 211, 155,  77, 118, 205,  30, 231, 133, 247,  60,  26,  69, 131, 100, 195,  32, 138,   0, 199,  95,  28, 223, 140,   4,
    124, 209,  56, 172,   9,  90,  60, 187, 116, 149, 177,  16, 161,  79, 139,  94, 198,  18, 181,  57,  36, 172,  21,  56, 126, 184,  36, 136,  55, 164,  81, 246,  28, 175,  77, 135,  99, 238,  81, 130, 195,  54, 244,  10, 183, 100,  70,   6, 190, 162, 114, 210, 179,   9, 153,  87, 248, 121, 221,  65, 170, 104,  69, 238,
     84,  19, 100, 142, 195, 125, 159, 214,  82,  32, 221,  57, 125, 209, 174,  35,  63, 253,  80, 209, 151,  92, 239, 143, 215,  14, 113, 235, 179, 207,  39, 153,  60, 121, 243,  47, 205,  26,  57, 181,  24, 111, 168,  41, 236, 158, 124, 208,  98,  46, 236,  88,  39, 242,  64, 209, 184,  76,  36, 146, 252,  20, 199, 156,
    220, 181, 255,  34,  71, 239,  15,  46, 244, 132, 197,  91, 248,  21, 112, 232, 155, 116, 135,  14, 230,  66, 196,  39, 165,  93, 203,  73,  22,  94, 133, 219, 190,  19, 149, 180, 112, 157, 216, 120, 232,  71, 214, 128,  87,  51, 224,  28, 180, 139,  12, 125, 201, 145, 113,  46,  16, 168, 103, 194,  55, 119, 178,  49,
    112, 134,  65, 160, 212,  96, 141, 174, 106,  69,   0, 144, 183,  71,  48, 203,   4, 192,  46, 170, 105, 130,   3, 111,  68, 245,  48, 157, 119, 251,   3, 107,  73, 228,  92,  10,  67, 255,  84,   5, 153,  98,  19, 144, 200,  12, 172,  85, 251,  72, 217, 168,  76,  23, 232, 162, 129, 239, 215,   6, 136, 230,  79,   9,
    236,  39, 200,   0, 120,  51, 232,  21, 194, 162, 236,  39, 118, 226, 148,  99,  74, 242,  89, 217,  33, 246, 158, 225, 181, 134,   8, 190, 224,  60, 186, 166,  47, 200, 131, 235, 191,  32, 137, 200,  51, 240, 192,  59, 246, 111, 148,  58, 117, 154,  33,  55, 188,  96, 217,  69,  92,  32,  63, 159,  89,  34, 206, 150,
    169,  82, 103, 226, 184,  77, 204, 127,  88,  51, 210, 102, 166,  13, 215, 176, 138,  27, 183, 145,  73, 190,  47,  83,  28, 210,  99, 143,  82,  34, 137, 241,  89,  26, 162,  43, 122, 171, 102, 224, 179,  36, 117, 173,  78,  34, 207, 233,   0, 194, 225, 109, 248, 140,   3, 175, 202, 147, 115, 191, 246, 172, 102,  62,
     21, 247, 145,  32, 167,  17, 155,  35, 223, 146,  25,  65, 255,  85,  36,  58, 113, 223,  53, 120,  15, 100, 206, 151, 115,  56, 237,  23, 169, 214, 115,  15, 146, 212, 107,  79, 219,  59,  20,  73, 129,  87, 156,  14, 227, 136,  92, 184,  47,  98, 135,  14, 159,  43, 122,  53, 255,  14, 219,  45,  71,  11, 128, 214,
    116, 185,  60, 126, 239,  93, 114, 245,  73, 178, 122, 204, 141, 187, 128, 236, 191,  17, 156, 250, 167, 231, 132,   7, 255, 171, 124,  70, 194,  97,  51, 227,  68, 173, 250,   0, 142, 198, 236, 165,   8, 248, 203, 105,  53, 177,  20, 124, 162, 245,  80, 180,  68, 213, 194, 100, 133,  80, 167, 105, 140, 229, 193,  51,
    222,  13,  87, 198,  48, 214,  60, 186,   4, 106, 234,  16,  50,  99,   6, 163,  82, 104, 204,  66,  90,  32,  61, 188,  79,  30, 203, 151,  11, 249, 158, 191, 129,  31,  53, 182,  93,  37, 115, 145, 195,  64,  33, 216, 146, 255,  69, 220,  31,  58, 209,  25, 237,  88,  30, 228, 180,  40, 241,  23, 183,  91,  32, 151,
     99, 165, 232, 152,   7, 164, 134, 207,  46, 165,  83, 195, 158, 229, 206,  67, 245,  44, 136,   0, 178, 216, 111, 159, 226, 103,  45, 233, 113,  40,  81,   6,  95, 205, 111, 222, 154, 246,  82,  28, 101, 174, 126,  84,   4, 114, 199,  89, 140, 191, 101, 150, 120, 168, 141,  12,  69, 153, 120, 203,  56, 161, 252,  69,
    201,  38, 122,  74, 103, 255,  25,  87, 148, 247,  27, 132,  72,  32, 114, 148,  20, 182, 222, 113, 240, 145,  46, 207,  16, 141, 181,  61, 135, 176, 223, 147, 244, 164,  74, 126,  16,  62, 169, 203, 231,  46, 240, 158, 190,  44, 153,  12, 233, 117,  15, 225,  38,  56, 249, 110, 188, 220,   5,  86, 235, 118,   9, 135
};


/* ═══════════════════════════════════════════════════════════════════
 * HLSL Shader Sources (compiled at runtime via shadercross)
 * ═══════════════════════════════════════════════════════════════════
 *
 * SDL_GPU binding convention (CRITICAL — wrong spaces = black screen):
 *   Fragment textures/samplers: space2 (SPIR-V set 2)
 *   Fragment uniform buffers:   space3 (SPIR-V set 3)
 *   Vertex textures/samplers:   space0 (SPIR-V set 0)
 *   Vertex uniform buffers:     space1 (SPIR-V set 1)
 *
 * CRITICAL: One SamplerState per Texture2D, always. SDL_GPU / SPIRV-Cross
 * counts "samplers" as texture+sampler pairs. Sharing one SamplerState
 * across multiple textures causes unpaired textures to be misclassified
 * as storage textures and bound to wrong slots.
 */

/* Fullscreen quad — generates 4 vertices from SV_VertexID, no vertex buffer.
 * Draw with SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0) as triangle-strip. */
static const char hlsl_fullscreen_vert[] =
    "struct VSOutput {\n"
    "    float4 pos : SV_Position;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VSOutput main(uint id : SV_VertexID) {\n"
    "    VSOutput o;\n"
    "    o.uv  = float2((id & 1), (id >> 1));\n"
    "    o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv.y = 1.0 - o.uv.y;\n"  /* flip Y: video is top-left origin */
    "    return o;\n"
    "}\n";

/* Planar YUV420P fragment shader — Lanczos-2 luma, Catmull-Rom chroma, blue noise dither.
 *
 * Luma (Y): Lanczos-2 windowed sinc, 4×4 texel kernel (16 taps).
 *           Preserves sharp detail during downscaling.
 *
 * Chroma (U, V): Catmull-Rom bicubic, 4×4 texel kernel (16 taps).
 *           Smoother than Lanczos without ringing at chroma block
 *           boundaries. Standard for chroma in quality video players.
 *
 * Output: Blue noise dither (±0.5 LSB) from 64×64 void-and-cluster
 *         texture before 8-bit quantization. All spectral energy in
 *         high frequencies — perceptually invisible, superior to IGN.
 *
 * SampleLevel(s, uv, 0) forces mip level 0. */
static const char hlsl_yuv_planar_frag[] =
    "Texture2D<float> texY : register(t0, space2);\n"
    "Texture2D<float> texU : register(t1, space2);\n"
    "Texture2D<float> texV : register(t2, space2);\n"
    "Texture2D<float> texNoise : register(t3, space2);\n"
    "SamplerState sampY : register(s0, space2);\n"
    "SamplerState sampU : register(s1, space2);\n"
    "SamplerState sampV : register(s2, space2);\n"
    "SamplerState sampNoise : register(s3, space2);\n"
    "\n"
    "cbuffer Params : register(b0, space3) {\n"
    "    row_major float4x4 colorMatrix;\n"
    "    float2 rangeY;\n"
    "    float2 rangeUV;\n"
    "    float2 texSizeY;\n"
    "    float2 texSizeUV;\n"
    "    float2 chromaOffset;\n"
    "    float frameCount;\n"
    "    float is_hdr;\n"
    "    float hdr_peak_nits;\n"
    "    float hdr_gamut;\n"
    "    float hdr_debug;\n"
    "    float hdr_target_nits;\n"
    "    float hdr_midtone_gain;\n"
    "    float is_dovi;\n"
    "    float out_gamma;\n"
    "    float is_hlg;\n"
    "    float4 dovi_num_pieces;\n"
    "    float4 dovi_pivots[9];\n"
    "    float4 dovi_c0[8];\n"
    "    float4 dovi_c1[8];\n"
    "    float4 dovi_c2[8];\n"
    "    float4 dovi_ycc_r0;\n"
    "    float4 dovi_ycc_r1;\n"
    "    float4 dovi_ycc_r2;\n"
    "    float4 dovi_out_r0;\n"
    "    float4 dovi_out_r1;\n"
    "    float4 dovi_out_r2;\n"
    "    float4 dovi_mmr_meta;\n"
    "    float4 dovi_mmr_ct[6];\n"
    "    float4 dovi_mmr_cp[6];\n"
    "    float hdr_pass;\n"
    "    float out_black_nits;\n"
    "    float2 _pad_hdrp;\n"
    "};\n"
    "\n"
    "#define PI 3.14159265358979\n"
    "\n"
    "float lanczos2(float x) {\n"
    "    x = abs(x);\n"
    "    if (x < 1e-6) return 1.0;\n"
    "    if (x >= 2.0) return 0.0;\n"
    "    float pix = PI * x;\n"
    "    return (sin(pix) * sin(pix * 0.5)) / (pix * pix * 0.5);\n"
    "}\n"
    "\n"
    "float sample_lanczos(Texture2D<float> tex, SamplerState samp,\n"
    "                     float2 uv, float2 tex_size) {\n"
    "    /* Downscale dilation: when one output pixel spans df > 1 source\n"
    "     * texels, a fixed 4-tap kernel undersamples high frequencies and\n"
    "     * aliases (moire on 4K content in a small window). Stretch the\n"
    "     * kernel by the pixel footprint, measured from screen-space\n"
    "     * derivatives — exact under letterboxing and aspect scaling.\n"
    "     * At df == 1 (1:1 and upscale) this reproduces the original\n"
    "     * 4x4 path: same taps, same weights. */\n"
    "    float2 df = float2(max(1.0, abs(ddx(uv.x)) * tex_size.x),\n"
    "                       max(1.0, abs(ddy(uv.y)) * tex_size.y));\n"
    "    df = min(df, 4.0);  /* 16 taps/axis cap — total work stays\n"
    "                           bounded because output pixels shrink as\n"
    "                           fast as taps grow */\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 fr   = pos - base;\n"
    "\n"
    "    int2 jmin = int2(floor(fr - 2.0 * df)) + 1;\n"
    "    int2 jmax = int2(floor(fr + 2.0 * df));\n"
    "\n"
    "    float result  = 0.0;\n"
    "    float wsum    = 0.0;\n"
    "    float tap_min = 1e9;\n"
    "    float tap_max = -1e9;\n"
    "\n"
    "    [loop] for (int j = jmin.y; j <= jmax.y; j++) {\n"
    "        float wy = lanczos2((float(j) - fr.y) / df.y);\n"
    "        [loop] for (int i = jmin.x; i <= jmax.x; i++) {\n"
    "            float w  = lanczos2((float(i) - fr.x) / df.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float s  = tex.SampleLevel(samp, tc, 0).r;\n"
    "            tap_min  = min(tap_min, s);\n"
    "            tap_max  = max(tap_max, s);\n"
    "            result  += s * w;\n"
    "            wsum    += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    float filtered = (wsum > 0.0) ? result / wsum : 0.0;\n"
    "    /* Anti-ringing: clamp to local tap range. Strength 0.8 per\n"
    "     * Artoriuz's scaler benchmarks (mpv community). */\n"
    "    float clamped  = clamp(filtered, tap_min, tap_max);\n"
    "    return lerp(filtered, clamped, 0.8);\n"
    "}\n"
    "\n"
    "/* Catmull-Rom (bicubic) 4x4 tap filter for chroma planes.\n"
    " * Smoother than bilinear without the ringing of Lanczos.\n"
    " * Standard for chroma upscaling in quality video players (mpv). */\n"
    "float catmull_w(float t) {\n"
    "    t = abs(t);\n"
    "    return (t <= 1.0)\n"
    "        ? (1.5 * t * t * t - 2.5 * t * t + 1.0)\n"
    "        : (-0.5 * t * t * t + 2.5 * t * t - 4.0 * t + 2.0);\n"
    "}\n"
    "\n"
    "float sample_catmull(Texture2D<float> tex, SamplerState samp,\n"
    "                     float2 uv, float2 tex_size) {\n"
    "    /* Same footprint dilation as sample_lanczos — chroma aliases on\n"
    "     * downscale just like luma. df == 1 reproduces the original. */\n"
    "    float2 df = float2(max(1.0, abs(ddx(uv.x)) * tex_size.x),\n"
    "                       max(1.0, abs(ddy(uv.y)) * tex_size.y));\n"
    "    df = min(df, 4.0);\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 fr   = pos - base;\n"
    "\n"
    "    int2 jmin = int2(floor(fr - 2.0 * df)) + 1;\n"
    "    int2 jmax = int2(floor(fr + 2.0 * df));\n"
    "\n"
    "    float result = 0.0;\n"
    "    float wsum   = 0.0;\n"
    "\n"
    "    [loop] for (int j = jmin.y; j <= jmax.y; j++) {\n"
    "        float wy = catmull_w((float(j) - fr.y) / df.y);\n"
    "        [loop] for (int i = jmin.x; i <= jmax.x; i++) {\n"
    "            float w = catmull_w((float(i) - fr.x) / df.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            result += tex.SampleLevel(samp, tc, 0).r * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    return (wsum > 0.0) ? result / wsum : 0.0;\n"
    "}\n"
    "\n"
    "/* PQ EOTF (SMPTE ST 2084 inverse): PQ code values [0,1] → linear\n"
    " * light [0, 10000] nits. Constants from ITU-R BT.2100. */\n"
    "float3 pq_eotf(float3 pq) {\n"
    "    float m1 = 0.1593017578125;\n"      /* 2610/16384 */
    "    float m2 = 78.84375;\n"              /* 2523/32 * 128 */
    "    float c1 = 0.8359375;\n"             /* 3424/4096 */
    "    float c2 = 18.8515625;\n"            /* 2413/128 */
    "    float c3 = 18.6875;\n"               /* 2392/128 */
    "    float3 Np = pow(max(pq, 0.0), 1.0 / m2);\n"
    "    float3 num = max(Np - c1, 0.0);\n"
    "    float3 den = c2 - c3 * Np;\n"
    "    return 10000.0 * pow(max(num / den, 0.0), 1.0 / m1);\n"
    "}\n"
    "\n"
    "/* PQ OETF (forward): normalized linear light [0,1] (1.0 = 10000\n"
    " * nits) → PQ code values [0,1]. Inverse of pq_eotf; used by the\n"
    " * HDR passthrough path to re-encode display-linear results into\n"
    " * the HDR10 container. */\n"
    "float3 pq_oetf(float3 lin) {\n"
    "    float m1 = 0.1593017578125;\n"
    "    float m2 = 78.84375;\n"
    "    float c1 = 0.8359375;\n"
    "    float c2 = 18.8515625;\n"
    "    float c3 = 18.6875;\n"
    "    float3 Np = pow(max(lin, 0.0), m1);\n"
    "    return pow((c1 + c2 * Np) / (1.0 + c3 * Np), m2);\n"
    "}\n"
    "\n"
    "/* Scalar PQ pair for the tone-map path. BT.2390's EETF is defined\n"
    " * on PQ-ENCODED luminance, so the tone map round-trips one scalar\n"
    " * through the OETF and back per pixel. Same constants as the\n"
    " * float3 pair above; pq_oetf1 takes normalized linear (nits/10000),\n"
    " * pq_eotf1 returns nits. */\n"
    "float pq_oetf1(float lin) {\n"
    "    float m1 = 0.1593017578125;\n"
    "    float m2 = 78.84375;\n"
    "    float c1 = 0.8359375;\n"
    "    float c2 = 18.8515625;\n"
    "    float c3 = 18.6875;\n"
    "    float Np = pow(max(lin, 0.0), m1);\n"
    "    return pow((c1 + c2 * Np) / (1.0 + c3 * Np), m2);\n"
    "}\n"
    "float pq_eotf1(float pq) {\n"
    "    float m1 = 0.1593017578125;\n"
    "    float m2 = 78.84375;\n"
    "    float c1 = 0.8359375;\n"
    "    float c2 = 18.8515625;\n"
    "    float c3 = 18.6875;\n"
    "    float Np = pow(max(pq, 0.0), 1.0 / m2);\n"
    "    float num = max(Np - c1, 0.0);\n"
    "    float den = c2 - c3 * Np;\n"
    "    return 10000.0 * pow(max(num / den, 0.0), 1.0 / m1);\n"
    "}\n"
    "\n"
    "/* BT.2390 EETF: Hermite spline shoulder rolloff for tone mapping.\n"
    " * Maps normalized luminance [0,1] through a soft knee at ks,\n"
    " * compressing highlights to maxLum. Below ks is linear passthrough.\n"
    " *\n"
    " * DOMAIN (fixed 2026-08-26, convicted in tools/eetf-audit.c): the\n"
    " * callers feed PQ-ENCODED luminance normalized by PQ(peak), per\n"
    " * spec. maxLum = PQ(target)/PQ(peak) lands ~0.58-0.9 for real\n"
    " * peak/target pairs, so ks = 1.5*maxLum - 0.5 is ~0.37-0.85 and\n"
    " * mids below the knee pass through untouched. (The old linear-\n"
    " * domain form had maxLum = target/peak = 0.02-0.34, ks clamped to\n"
    " * ~0, the whole range splined: mids -15..-19% dark at peak 600,\n"
    " * and at high peaks everything above ~1/16 of peak clipped flat.)\n"
    " *\n"
    " * The output clamp to [0, maxLum] stays as the bounded-math belt:\n"
    " * in PQ domain maxLum >= 0.5 for all real pairs so the Hermite\n"
    " * never overshoots and the clamp is a no-op, but it keeps every\n"
    " * downstream stage (midtone gain, gamut matrix) bounded for any\n"
    " * metadata the sanitizer lets through. */\n"
    "float bt2390_eetf(float e, float ks, float maxLum) {\n"
    "    if (e <= ks) return e;\n"
    "    float t = (e - ks) / (1.0 - ks);\n"
    "    float t2 = t * t;\n"
    "    float t3 = t2 * t;\n"
    "    float y = (2.0*t3 - 3.0*t2 + 1.0) * ks\n"
    "            + (t3 - 2.0*t2 + t) * (1.0 - ks)\n"
    "            + (-2.0*t3 + 3.0*t2) * maxLum;\n"
    "    return clamp(y, 0.0, maxLum);\n"
    "}\n"
    "\n"
    "/* BT.2390 black-level lift: E3 = E2 + minLum*(1-E2)^4, minLum =\n"
    " * PQ(display black)/PQ(peak) — same normalized-PQ domain as the\n"
    " * spline (source black taken as 0). Keeps near-black detail above\n"
    " * the display's real black instead of clipping into it (deck\n"
    " * 4614882 port; Holden: \"reference means lift, we lift\").\n"
    " * out_black_nits: <0 off, 0 auto (target/2000 — a 2000:1\n"
    " * contrast assumption; halved from 1000:1 after the lift made\n"
    " * WEB-DL baked-bar codec noise visible, Holden field 2026-08-27),\n"
    " * >0 fixed nits. (1-e2)^4 by squaring —\n"
    " * no pow (transcendental budget). */\n"
    "float bt2390_lift(float e2, float target, float maxE) {\n"
    "    if (out_black_nits < 0.0) return e2;\n"
    "    float b = (out_black_nits > 0.0) ? out_black_nits\n"
    "                                     : target / 2000.0;\n"
    "    float minL = pq_oetf1(b / 10000.0) / maxE;\n"
    "    float u = 1.0 - e2;\n"
    "    u = u * u; u = u * u;\n"
    "    return e2 + minL * u;\n"
    "}\n"
    "\n"
    "/* Encode tone-mapped display-linear output for the display's EOTF.\n"
    " * out_gamma == 0 selects the sRGB piecewise curve; otherwise a pure\n"
    " * power law (2.2 default, 2.4 = BT.1886 dark-room). The sRGB linear\n"
    " * toe lifts shadows slightly on a display decoding ~2.2, so power is\n"
    " * the reference-faithful default (DSVP_OUTPUT_GAMMA overrides). */\n"
    "float3 encode_output(float3 lin) {\n"
    "    lin = max(lin, 0.0);\n"
    "    if (out_gamma < 0.5) {\n"
    "        return float3(\n"
    "            lin.r <= 0.0031308 ? 12.92*lin.r : 1.055*pow(lin.r, 1.0/2.4) - 0.055,\n"
    "            lin.g <= 0.0031308 ? 12.92*lin.g : 1.055*pow(lin.g, 1.0/2.4) - 0.055,\n"
    "            lin.b <= 0.0031308 ? 12.92*lin.b : 1.055*pow(lin.b, 1.0/2.4) - 0.055);\n"
    "    }\n"
    "    return pow(lin, 1.0 / out_gamma);\n"
    "}\n"
    "\n"
    "/* HLG (ARIB STD-B67): inverse OETF to scene-linear, then the\n"
    " * BT.2100 OOTF at nominal Lw=1000 (system gamma 1.2) to display\n"
    " * light in nits — from there the shared BT.2390 path takes over. */\n"
    "float3 hlg_oetf_inv(float3 e) {\n"
    "    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;\n"
    "    float3 lo = (e * e) / 3.0;\n"
    "    float3 hi = (exp((e - c) / a) + b) / 12.0;\n"
    "    return float3(e.r <= 0.5 ? lo.r : hi.r,\n"
    "                  e.g <= 0.5 ? lo.g : hi.g,\n"
    "                  e.b <= 0.5 ? lo.b : hi.b);\n"
    "}\n"
    "\n"
    "float3 hlg_to_nits(float3 sig) {\n"
    "    float3 s = hlg_oetf_inv(saturate(sig));\n"
    "    float ys = dot(s, float3(0.2627, 0.6780, 0.0593));\n"
    "    return 1000.0 * pow(max(ys, 1e-6), 0.2) * s;\n"
    "}\n"
    "\n"
    "/* Select component from float4: 0=x(I), 1=y(Ct), 2=z(Cp) */\n"
    "float sel3(float4 v, int c) {\n"
    "    if (c == 0) return v.x;\n"
    "    if (c == 1) return v.y;\n"
    "    return v.z;\n"
    "}\n"
    "\n"
    "/* DV MMR (Multivariate Multiple Regression) reshape for one chroma\n"
    " * component: a cross-channel polynomial over the coded (I,Ct,Cp)\n"
    " * triple. Terms per order o (1..3): I, Ct, Cp, I*Ct, I*Cp, Ct*Cp,\n"
    " * I*Ct*Cp — each raised to the o-th power, 7 coefficients per\n"
    " * order plus one constant. comp: 1 = Ct, 2 = Cp. */\n"
    "float dovi_mmr_eval(float3 sig, int comp) {\n"
    "    int   order = (int)((comp == 1) ? dovi_mmr_meta.x : dovi_mmr_meta.y);\n"
    "    float s     =        (comp == 1) ? dovi_mmr_meta.z : dovi_mmr_meta.w;\n"
    "    float t[7];\n"
    "    t[0] = sig.x; t[1] = sig.y; t[2] = sig.z;\n"
    "    t[3] = sig.x * sig.y;\n"
    "    t[4] = sig.x * sig.z;\n"
    "    t[5] = sig.y * sig.z;\n"
    "    t[6] = t[3] * sig.z;\n"
    "    float p[7];\n"
    "    [unroll] for (int k = 0; k < 7; k++) p[k] = t[k];\n"
    "    [loop] for (int o = 0; o < 3; o++) {\n"
    "        if (o >= order) break;\n"
    "        [unroll] for (int i = 0; i < 7; i++) {\n"
    "            int idx = o * 7 + i;\n"
    "            float4 bank = (comp == 1) ? dovi_mmr_ct[idx >> 2]\n"
    "                                      : dovi_mmr_cp[idx >> 2];\n"
    "            s += bank[idx & 3] * p[i];\n"
    "        }\n"
    "        [unroll] for (int k2 = 0; k2 < 7; k2++) p[k2] *= t[k2];\n"
    "    }\n"
    "    return s;\n"
    "}\n"
    "\n"
    "/* Piecewise polynomial reshape for one DV component.\n"
    " * Searches pivot array to find the active piece, then evaluates\n"
    " * c0 + c1*x + c2*x*x. Falls back to identity if no pieces. */\n"
    "float dovi_reshape(float x, int comp) {\n"
    "    int n = (int)sel3(dovi_num_pieces, comp);\n"
    "    if (n <= 0) return x;\n"
    "    /* Clamp to the curve's pivot span BEFORE piece selection and\n"
    "     * evaluation (deck 9e5f245 port, libplacebo parity): kernel\n"
    "     * overshoot otherwise extrapolates the last piece above the\n"
    "     * top pivot. pivots has n+1 entries, [n] = top of span. */\n"
    "    x = clamp(x, sel3(dovi_pivots[0], comp),\n"
    "                 sel3(dovi_pivots[n], comp));\n"
    "    for (int p = 0; p < 8; p++) {\n"
    "        if (p >= n) break;\n"
    "        float hi = sel3(dovi_pivots[p + 1], comp);\n"
    "        if (x < hi || p == n - 1) {\n"
    "            float c0v = sel3(dovi_c0[p], comp);\n"
    "            float c1v = sel3(dovi_c1[p], comp);\n"
    "            float c2v = sel3(dovi_c2[p], comp);\n"
    "            return c0v + c1v * x + c2v * x * x;\n"
    "        }\n"
    "    }\n"
    "    return x;\n"
    "}\n"
    "\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    /* Chroma siting: shift UV to actual sample position */\n"
    "    float2 uv_chroma = uv + chromaOffset / texSizeUV;\n"
    "\n"
    "    /* Lanczos-2 for luma, Catmull-Rom for chroma */\n"
    "    float y  = sample_lanczos(texY, sampY, uv, texSizeY);\n"
    "    float cb = sample_catmull(texU, sampU, uv_chroma, texSizeUV);\n"
    "    float cr = sample_catmull(texV, sampV, uv_chroma, texSizeUV);\n"
    "\n"
    "    y  = (y  - rangeY.x)  * rangeY.y;\n"
    "    cb = (cb - rangeUV.x) * rangeUV.y;\n"
    "    cr = (cr - rangeUV.x) * rangeUV.y;\n"
    "\n"
    "    float3 rgb;\n"
    "\n"
    "    if (is_dovi > 0.5) {\n"
    "        /* ── Dolby Vision decode chain ──\n"
    "         * Planes contain I/Ct/Cp (IPTPQc2), not standard YCbCr.\n"
    "         * 1. Reshape: piecewise polynomial (from RPU pivot/coef arrays)\n"
    "         * 2. ycc_to_rgb matrix: IPT → PQ-encoded signal\n"
    "         * 3. PQ EOTF → linear light (nits)\n"
    "         * 4. Output matrix → BT.2020 linear RGB\n"
    "         * 5. BT.2390 tone mapping (shared with HDR10 path) */\n"
    "        float3 sig = float3(y, cb, cr);\n"
    "        /* Reshape input is defined on [0,1] code values; kernel\n"
    "         * ringing goes outside it and the MMR's cross terms\n"
    "         * amplify what the per-component pivot clamp alone cannot\n"
    "         * catch (deck 9e5f245 port, review PQUALITY P4). */\n"
    "        sig = saturate(sig);\n"
    "        float3 ipt;\n"
    "        ipt.x = dovi_reshape(sig.x, 0);\n"
    "        /* Chroma: MMR when the RPU says so — but only for pixels\n"
    "         * in piece 0's pivot range: the CPU packs MMR coefficients\n"
    "         * for piece 0 alone, and dispatching before the pivot\n"
    "         * search routed EVERY pixel of a multi-piece curve through\n"
    "         * piece 0's MMR. Single-piece curves (nearly all real P5\n"
    "         * content) are unchanged by construction; outside piece 0,\n"
    "         * dovi_reshape's own search lands the right poly piece. */\n"
    "        ipt.y = (dovi_mmr_meta.x > 0.5 &&\n"
    "                 (sel3(dovi_num_pieces, 1) <= 1.5 ||\n"
    "                  sig.y < sel3(dovi_pivots[1], 1)))\n"
    "                    ? dovi_mmr_eval(sig, 1) : dovi_reshape(sig.y, 1);\n"
    "        ipt.z = (dovi_mmr_meta.y > 0.5 &&\n"
    "                 (sel3(dovi_num_pieces, 2) <= 1.5 ||\n"
    "                  sig.z < sel3(dovi_pivots[1], 2)))\n"
    "                    ? dovi_mmr_eval(sig, 2) : dovi_reshape(sig.z, 2);\n"
    "        ipt = saturate(ipt);\n"
    "\n"
    "        float3 centered = ipt - float3(dovi_ycc_r0.w, dovi_ycc_r1.w, dovi_ycc_r2.w);\n"
    "        float3 pq_sig;\n"
    "        pq_sig.r = dot(dovi_ycc_r0.xyz, centered);\n"
    "        pq_sig.g = dot(dovi_ycc_r1.xyz, centered);\n"
    "        pq_sig.b = dot(dovi_ycc_r2.xyz, centered);\n"
    "        pq_sig = saturate(pq_sig);\n"
    "\n"
    "        float3 lin = pq_eotf(pq_sig);\n"
    "        float3 bt2020;\n"
    "        bt2020.r = dot(dovi_out_r0.xyz, lin);\n"
    "        bt2020.g = dot(dovi_out_r1.xyz, lin);\n"
    "        bt2020.b = dot(dovi_out_r2.xyz, lin);\n"
    "        bt2020 = max(bt2020, 0.0);\n"
    "\n"
    "        if (hdr_pass > 0.5) {\n"
    "            /* DV-as-HDR10 (deck lineage): the per-frame RPU reshape\n"
    "             * above IS Dolby Vision's dynamic metadata, applied by\n"
    "             * us — the same processing an LLDV player does\n"
    "             * internally. Re-encode the display-linear BT.2020\n"
    "             * result (nits, from pq_eotf) into the PQ container and\n"
    "             * let the display tone-map. */\n"
    "            rgb = pq_oetf(bt2020 / 10000.0);\n"
    "        } else {\n"
    "        /* BT.2390 tone mapping (PQ domain, per spec) — always\n"
    "         * BT.2020 gamut for DV. bt2020 is display-linear NITS here.\n"
    "         * SHARED-SOURCE: this block and the HDR10/HLG block below\n"
    "         * are the same transform — change BOTH together. */\n"
    "        float target = (hdr_debug > 0.5 && hdr_debug < 1.5)\n"
    "            ? hdr_target_nits + 100.0 : hdr_target_nits;\n"
    "        float maxE   = pq_oetf1(hdr_peak_nits / 10000.0);\n"
    "        float maxLum = min(pq_oetf1(target / 10000.0) / maxE, 1.0);\n"
    "        float ks = clamp(1.5 * maxLum - 0.5, 0.0, 0.999);\n"
    "        float3 lc = float3(0.2627, 0.6780, 0.0593);\n"
    "        float Y_l = dot(bt2020, lc);   /* nits */\n"
    "        /* Super-peak speculars (the percentile peak excludes the\n"
    "         * top 0.125%) clamp to the spline endpoint → maxLum → SDR\n"
    "         * white after the target divide; chromaticity rides the\n"
    "         * original Y_l in the ratio below. */\n"
    "        float e = min(pq_oetf1(Y_l / 10000.0) / maxE, 1.0);\n"
    "        float e2 = bt2390_lift(bt2390_eetf(e, ks, maxLum),\n"
    "                               target, maxE);\n"
    "        float Yt_nits = pq_eotf1(e2 * maxE);\n"
    "        float3 rgb_tm = (Y_l > 0.0)\n"
    "            ? (bt2020 / target) * (Yt_nits / Y_l) : float3(0,0,0);\n"
    "\n"
    "        /* BT.2020→BT.709 gamut matrix */\n"
    "        float3 r2 = rgb_tm;\n"
    "        rgb_tm = float3(\n"
    "             1.6605*r2.r - 0.5877*r2.g - 0.0728*r2.b,\n"
    "            -0.1246*r2.r + 1.1330*r2.g - 0.0084*r2.b,\n"
    "            -0.0182*r2.r - 0.1006*r2.g + 1.1187*r2.b);\n"
    "        rgb_tm = max(rgb_tm, 0.0);\n"
    "\n"
    "        if (hdr_midtone_gain > 1.001) {\n"
    "            float inv = 1.0 / hdr_midtone_gain;\n"
    "            rgb_tm = float3(pow(rgb_tm.r, inv), pow(rgb_tm.g, inv), pow(rgb_tm.b, inv));\n"
    "        }\n"
    "        rgb = encode_output(rgb_tm);\n"
    "        } /* end DV SDR tone map */\n"
    "\n"
    "    } else {\n"
    "        /* Standard path (SDR + HDR10) */\n"
    "        float4 yuv = float4(y, cb - 0.5, cr - 0.5, 1.0);\n"
    "        rgb = mul(colorMatrix, yuv).rgb;\n"
    "\n"
    "    /* ── HDR→SDR Tone Mapping (BT.2390 EETF) ──\n"
    "     * Debug modes (H key): 0=normal, 1=target 300, 2=PQ bypass, 3=luma viz */\n"
    "    if (is_hdr > 0.5) {\n"
    "\n"
    "        /* HDR passthrough: the swapchain is HDR10/ST2084 — rgb\n"
    "         * already holds range-expanded PQ code values in BT.2020,\n"
    "         * which is exactly the payload the display wants. No tone\n"
    "         * map, no gamut squeeze, no output encode: the most\n"
    "         * correct path is the code we skip. The debug modes and\n"
    "         * T/G controls are SDR-render tools. */\n"
    "        if (hdr_pass > 0.5) {\n"
    "            /* PQ content ships as-is. HLG rides the same HDR10\n"
    "             * container: inverse OETF + BT.2100 OOTF to display\n"
    "             * light at the 1000-nit nominal, then PQ-encode — the\n"
    "             * display tone-maps from there. */\n"
    "            if (is_hlg > 0.5)\n"
    "                rgb = pq_oetf(hlg_to_nits(rgb) / 10000.0);\n"
    "        }\n"
    "        /* Mode 2: PQ bypass — raw PQ code values straight to display.\n"
    "         * Shows what the stream actually contains. If this looks\n"
    "         * reasonably bright, PQ values are valid and the issue\n"
    "         * is in tone mapping. If dark, values themselves are wrong. */\n"
    "        else if (hdr_debug > 1.5 && hdr_debug < 2.5) {\n"
    "            /* rgb already holds PQ code values [0,1] — skip everything */\n"
    "        }\n"
    "        /* Mode 3: luminance visualization — grayscale of the\n"
    "         * frame's nit distribution. Routes through encode_output\n"
    "         * so it follows the E-key transfer pref instead of a\n"
    "         * hardwired sRGB (deck 9d108d8 port, debug-honesty). */\n"
    "        else if (hdr_debug > 2.5) {\n"
    "            float3 lin = (is_hlg > 0.5) ? hlg_to_nits(rgb) : pq_eotf(rgb);\n"
    "            float lum = lin.r * 0.2627 + lin.g * 0.6780 + lin.b * 0.0593;\n"
    "            float v = saturate(lum / hdr_peak_nits);\n"
    "            rgb = encode_output(float3(v, v, v));\n"
    "        }\n"
    "        else {\n"
    "            float3 lin = (is_hlg > 0.5) ? hlg_to_nits(rgb) : pq_eotf(rgb);\n"
    "\n"
    "            /* BT.2390 tone mapping (PQ domain, per spec). lin is\n"
    "             * display-linear NITS. SHARED-SOURCE: same transform as\n"
    "             * the DV block above — change BOTH together.\n"
    "             * Target comes from T-key toggle (203/300/400 nits).\n"
    "             * Debug mode 1: override to target+100 for comparison. */\n"
    "            float target = (hdr_debug > 0.5 && hdr_debug < 1.5)\n"
    "                ? hdr_target_nits + 100.0 : hdr_target_nits;\n"
    "            float maxE   = pq_oetf1(hdr_peak_nits / 10000.0);\n"
    "            float maxLum = min(pq_oetf1(target / 10000.0) / maxE, 1.0);\n"
    "            float ks = clamp(1.5 * maxLum - 0.5, 0.0, 0.999);\n"
    "\n"
    "            float3 lc = (hdr_gamut > 0.5)\n"
    "                ? float3(0.2627, 0.6780, 0.0593)\n"
    "                : float3(0.2126, 0.7152, 0.0722);\n"
    "            float Y = dot(lin, lc);   /* nits */\n"
    "            /* See DV path comment: super-peak clamps to the spline\n"
    "             * endpoint (SDR white), chromaticity via the Y ratio. */\n"
    "            float e = min(pq_oetf1(Y / 10000.0) / maxE, 1.0);\n"
    "            float e2 = bt2390_lift(bt2390_eetf(e, ks, maxLum),\n"
    "                                   target, maxE);\n"
    "            float Yt_nits = pq_eotf1(e2 * maxE);\n"
    "            float3 rgb_tm = (Y > 0.0)\n"
    "                ? (lin / target) * (Yt_nits / Y) : float3(0,0,0);\n"
    "\n"
    "            if (hdr_gamut > 0.5) {\n"
    "                float3 r2 = rgb_tm;\n"
    "                rgb_tm = float3(\n"
    "                     1.6605*r2.r - 0.5877*r2.g - 0.0728*r2.b,\n"
    "                    -0.1246*r2.r + 1.1330*r2.g - 0.0084*r2.b,\n"
    "                    -0.0182*r2.r - 0.1006*r2.g + 1.1187*r2.b);\n"
    "                rgb_tm = max(rgb_tm, 0.0);\n"
    "            }\n"
    "\n"
    "\n"
    "            rgb_tm = max(rgb_tm, 0.0);\n"
    "            if (hdr_midtone_gain > 1.001) {\n"
    "                float inv = 1.0 / hdr_midtone_gain;\n"
    "                rgb_tm = float3(pow(rgb_tm.r, inv), pow(rgb_tm.g, inv), pow(rgb_tm.b, inv));\n"
    "            }\n"
    "            rgb = encode_output(rgb_tm);\n"
    "        }\n"
    "    }\n"
    "    else if (hdr_gamut > 0.5) {\n"
    "        /* SDR tagged BT.2020 (rare but legal): the 2020 YCbCr matrix\n"
    "         * above produced BT.2020 RGB, but the swapchain is 709/sRGB —\n"
    "         * displaying it unconverted is visibly desaturated. Convert\n"
    "         * primaries in LINEAR light (BT.1886-ish 2.4 for SDR video),\n"
    "         * then re-encode with the same curve (transfer unchanged —\n"
    "         * this is a gamut conversion, not a tone map). */\n"
    "        float3 lin = pow(max(rgb, 0.0), 2.4);\n"
    "        float3 l7 = float3(\n"
    "             1.6605*lin.r - 0.5877*lin.g - 0.0728*lin.b,\n"
    "            -0.1246*lin.r + 1.1330*lin.g - 0.0084*lin.b,\n"
    "            -0.0182*lin.r - 0.1006*lin.g + 1.1187*lin.b);\n"
    "        rgb = pow(max(l7, 0.0), 1.0/2.4);\n"
    "    }\n"
    "    } /* end else (standard path) */\n"
    "\n"
    "    /* Blue noise dither: ±0.5 LSB in 8-bit (±1/510 in [0,1]).\n"
    "     * 64x64 void-and-cluster texture, tiled via frac(). Temporal\n"
    "     * offset shifts the pattern each frame so quantization error\n"
    "     * averages out over ~4 frames — perceived bit depth increases. */\n"
    "    uint fc = (uint)frameCount;\n"
    "    float2 ditherCoord = pos.xy + float2(fc % 4u, (fc / 4u) % 4u);\n"
    "    /* LSB matches the output surface: 10-bit in HDR passthrough. */\n"
    "    float dith_lsb = (hdr_pass > 0.5) ? 1023.0 : 255.0;\n"
    "    float d = (texNoise.SampleLevel(sampNoise, frac(ditherCoord / 64.0), 0).r - 0.5) / dith_lsb;\n"
    "    rgb += float3(d, d, d);\n"
    "\n"
    "    return float4(saturate(rgb), 1.0);\n"
    "}\n";

/* RGBA overlay fragment shader — simple passthrough with alpha.
 * Used for compositing debug overlays, seek bar, subtitles, etc.
 * over the video frame. One texture, one sampler, no uniforms. */
static const char hlsl_overlay_frag[] =
    "Texture2D<float4> texOverlay : register(t0, space2);\n"
    "SamplerState sampOverlay : register(s0, space2);\n"
    "\n"
    /* When the swapchain is HDR10/PQ, the overlay's SDR-authored
     * pixels must be re-encoded to PQ at reference white (203 nits,
     * BT.2408) — drawn raw, "white" would mean 10,000 nits and every
     * subtitle would be a flashbang. (Deck lineage.) */
    "cbuffer OvParams : register(b0, space3) {\n"
    "    float ov_pq;\n"
    "    float ov_white;\n"      /* encode white, nits (203 nominal; compensated for the KWin hold) */
    "    float2 _pad;\n"
    "};\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    float4 c = texOverlay.Sample(sampOverlay, uv);\n"
    "    if (ov_pq > 0.5) {\n"
    "        /* SDR (2.2) → linear → ov_white nits → PQ OETF */\n"
    "        float3 lin = pow(max(c.rgb, 0.0), 2.2) * (ov_white / 10000.0);\n"
    "        float m1 = 0.1593017578125, m2 = 78.84375;\n"
    "        float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
    "        float3 Np = pow(lin, m1);\n"
    "        c.rgb = pow((c1 + c2 * Np) / (1.0 + c3 * Np), m2);\n"
    "    }\n"
    "    return c;\n"
    "}\n";

/* Pure-fetch blit fragment shader — cache→swapchain copy. MUST stay
 * uniform-free: the blit draw pushes no fragment uniforms, and the
 * overlay shader it used to share gained OvParams in the HDR cycle —
 * sharing it made every reblit tick read stale slot-0 data as ov_pq
 * and randomly PQ-encode the whole frame (the 2026-08-12 strobe). */
static const char hlsl_blit_frag[] =
    "Texture2D<float4> texSrc : register(t0, space2);\n"
    "SamplerState sampSrc : register(s0, space2);\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    return texSrc.Sample(sampSrc, uv);\n"
    "}\n";


/* ═══════════════════════════════════════════════════════════════════
 * SPIRV Compile Cache
 * ═══════════════════════════════════════════════════════════════════
 *
 * Caches the SPIRV bytecode output of HLSL→SPIRV compilation — the
 * expensive step in the shader pipeline (~200ms for the 48KB fragment
 * shader at cold start). SPIRV is platform-independent bytecode, so
 * the cache key is just (cache version + stage + entrypoint + source).
 * The SPIRV→native compile still runs every launch (driver-specific,
 * fast).
 *
 * Cache layout: <exe_dir>/shadercaches/<stage>_<16-hex-key>.spv
 * Files are opaque SPIRV blobs. Safe for the user to delete; the next
 * launch will repopulate. Cache version bumps whenever shadercross
 * version changes or anything that could alter SPIRV output for the
 * same source, invalidating all entries.
 */

#ifdef _WIN32
  #include <direct.h>
  static int dsvp_mkdir(const char *p) { return _mkdir(p); }
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  static int dsvp_mkdir(const char *p) { return mkdir(p, 0755); }
#endif

#define DSVP_SHADERCACHE_VERSION "1"  /* bump to invalidate all cache files */
#define DSVP_SHADERCACHE_MAX_SIZE (16 * 1024 * 1024)  /* sanity ceiling */

/* FNV-1a 64-bit hash. Not cryptographic; collision-resistant enough
 * for keying a handful of shaders per program. Public domain. */
static uint64_t dsvp_fnv1a64(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Build hash key from (version, stage, entrypoint, source). */
static uint64_t shadercache_key(const char *stage, const char *ep,
                                const char *src) {
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr), "%s|%s|%s|",
                        DSVP_SHADERCACHE_VERSION, stage, ep);
    if (hlen < 0 || (size_t)hlen >= sizeof(hdr)) return 0;
    /* Hash header then source, in a stream to avoid concatenation. */
    uint64_t h = dsvp_fnv1a64(hdr, (size_t)hlen);
    /* Continue the hash with the source bytes. FNV is rolling, but
     * we don't expose a continuation API; re-implement inline. */
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Compose <exe_dir>/shadercaches/<stage>_<key>.spv into out. */
static int shadercache_path(char *out, size_t out_size,
                            const char *stage, uint64_t key) {
    const char *base = SDL_GetBasePath();
    if (!base) return -1;
    int n = snprintf(out, out_size, "%sshadercaches/%s_%016llx.spv",
                     base, stage, (unsigned long long)key);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

/* Best-effort: create shadercaches/ next to the exe. Ignores existing-
 * directory errors. Failure here just means saves will fail silently. */
static void shadercache_ensure_dir(void) {
    const char *base = SDL_GetBasePath();
    if (!base) return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%sshadercaches", base);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;
    dsvp_mkdir(path);  /* errno=EEXIST is fine; we don't check */
}

/* Load cached SPIRV. Returns SDL_malloc'd buffer (caller SDL_free's
 * via the same path as the SDL_ShaderCross result) or NULL on miss. */
static void *shadercache_load(const char *stage, uint64_t key,
                              size_t *out_size) {
    char path[1024];
    if (shadercache_path(path, sizeof(path), stage, key) != 0) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || size > DSVP_SHADERCACHE_MAX_SIZE) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    void *buf = SDL_malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        SDL_free(buf);
        return NULL;
    }
    *out_size = (size_t)size;
    return buf;
}

/* Save SPIRV to cache atomically (write to .tmp, then rename).
 * Best-effort — failures are non-fatal and silently skipped. */
static void shadercache_save(const char *stage, uint64_t key,
                             const void *buf, size_t size) {
    shadercache_ensure_dir();
    char path[1024], tmp[1024 + 4];
    if (shadercache_path(path, sizeof(path), stage, key) != 0) return;
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return;
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    size_t wrote = fwrite(buf, 1, size, f);
    if (fclose(f) != 0 || wrote != size) {
        remove(tmp);
        return;
    }
    if (rename(tmp, path) != 0) remove(tmp);
}


/* ═══════════════════════════════════════════════════════════════════
 * Shader Compilation Helper
 * ═══════════════════════════════════════════════════════════════════
 *
 * Three-step pipeline for shadercross 3.0.0:
 *   1. HLSL → SPIRV  (CompileSPIRVFromHLSL) — cached on disk; see
 *      SPIRV Compile Cache above. Cold path is ~200ms for the 48KB
 *      fragment shader; cache hit is ~ms.
 *   2. SPIRV → metadata (ReflectGraphicsSPIRV) — resource counts
 *   3. SPIRV → native  (CompileGraphicsShaderFromSPIRV) — D3D12/Vulkan/Metal
 *
 * Note: CompileGraphicsShaderFromHLSL does NOT exist in 3.0.0.
 */

/* True when the content signals the BT.2020/HDR family by ANY of its
 * container hints — 2020 colorspace/primaries tags, PQ/HLG transfer,
 * or a Dolby Vision configuration record. Real-world HDR re-encodes
 * strip these inconsistently (DV P5 routinely tags everything
 * UNSPECIFIED), so any one signal suffices. Consumers: the chroma
 * siting defaults in gpu_setup_uniforms and the sws dst_chr_pos
 * pinning — keep both on this single predicate. */
static int content_is_2020_family(const AVCodecParameters *par)
{
    if (par->color_space     == AVCOL_SPC_BT2020_NCL ||
        par->color_space     == AVCOL_SPC_BT2020_CL  ||
        par->color_primaries == AVCOL_PRI_BT2020     ||
        par->color_trc       == AVCOL_TRC_SMPTE2084  ||
        par->color_trc       == AVCOL_TRC_ARIB_STD_B67)
        return 1;
    if (av_packet_side_data_get(par->coded_side_data,
                                par->nb_coded_side_data,
                                AV_PKT_DATA_DOVI_CONF))
        return 1;
    return 0;
}


static SDL_GPUShader *compile_shader(
    SDL_GPUDevice *device,
    const char *source,
    const char *entrypoint,
    SDL_ShaderCross_ShaderStage stage)
{
    const char *stage_name =
        (stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX) ? "vert" : "frag";

    /* Step 1: HLSL → SPIRV, with on-disk cache. */
    SDL_ShaderCross_HLSL_Info hlsl_info;
    SDL_zero(hlsl_info);
    hlsl_info.source       = source;
    hlsl_info.entrypoint   = entrypoint;
    hlsl_info.include_dir  = NULL;
    hlsl_info.defines      = NULL;
    hlsl_info.shader_stage = stage;
    hlsl_info.props        = 0;

    size_t spirv_size = 0;
    uint64_t cache_key = shadercache_key(stage_name, entrypoint, source);
    void *spirv = (cache_key != 0)
        ? shadercache_load(stage_name, cache_key, &spirv_size)
        : NULL;

    if (spirv) {
        log_msg("Shader: SPIRV cache hit (%s, %zu bytes)",
                stage_name, spirv_size);
    } else {
        spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &spirv_size);
        if (!spirv) {
            log_msg("ERROR: HLSL->SPIRV failed (%s): %s",
                    stage_name, SDL_GetError());
            return NULL;
        }
        log_msg("Shader: HLSL->SPIRV OK (%s, %zu bytes)",
                stage_name, spirv_size);
        if (cache_key != 0) {
            shadercache_save(stage_name, cache_key, spirv, spirv_size);
        }
    }

    /* Step 2: Reflect SPIRV for resource counts.
     * Returns a malloc'd struct — must SDL_free when done. */
    SDL_ShaderCross_GraphicsShaderMetadata *metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV(spirv, spirv_size, 0);
    if (!metadata) {
        log_msg("ERROR: SPIRV reflection failed: %s", SDL_GetError());
        SDL_free(spirv);
        return NULL;
    }
    log_msg("Shader: reflect OK (samplers=%u storage_tex=%u uniforms=%u)",
            metadata->resource_info.num_samplers,
            metadata->resource_info.num_storage_textures,
            metadata->resource_info.num_uniform_buffers);

    /* Step 3: SPIRV → native GPU shader.
     * CompileGraphicsShaderFromSPIRV takes:
     *   (device, SPIRV_Info*, GraphicsShaderResourceInfo*, props) */
    SDL_ShaderCross_SPIRV_Info spirv_info;
    SDL_zero(spirv_info);
    spirv_info.bytecode     = spirv;
    spirv_info.bytecode_size = spirv_size;
    spirv_info.entrypoint   = entrypoint;
    spirv_info.shader_stage = stage;
    spirv_info.props        = 0;

    SDL_GPUShader *shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
        device, &spirv_info, &metadata->resource_info, 0);

    SDL_free(metadata);
    SDL_free(spirv);

    if (!shader) {
        log_msg("ERROR: SPIRV->native failed: %s", SDL_GetError());
        return NULL;
    }
    log_msg("Shader: native compile OK (%s)", stage_name);
    return shader;
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Pipeline Setup / Teardown
 * ═══════════════════════════════════════════════════════════════════
 *
 * Called once at startup (from main.c after creating the GPU device
 * and claiming the window). Compiles shaders and creates the
 * graphics pipelines and sampler.
 *
 * Two pipelines:
 *   - gpu_pipeline_yuv:     planar YUV420P (3 textures, 3 samplers)
 *   - gpu_pipeline_overlay: RGBA + alpha blend (1 texture, 1 sampler)
 */

int gpu_create_pipelines(PlayerState *ps) {
    if (!ps->gpu_device || !ps->window) return -1;

    log_msg("GPU: compiling shaders...");

    /* ── Compile vertex shader (shared by both pipelines) ── */
    SDL_GPUShader *vert = compile_shader(
        ps->gpu_device, hlsl_fullscreen_vert, "main",
        SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    if (!vert) return -1;

    /* ── Compile planar YUV fragment shader ── */
    SDL_GPUShader *frag_yuv = compile_shader(
        ps->gpu_device, hlsl_yuv_planar_frag, "main",
        SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!frag_yuv) {
        SDL_ReleaseGPUShader(ps->gpu_device, vert);
        return -1;
    }

    /* ── Create YUV planar pipeline ── */
    SDL_GPUColorTargetDescription color_desc;
    SDL_zero(color_desc);
    color_desc.format = SDL_GetGPUSwapchainTextureFormat(
        ps->gpu_device, ps->window);

    SDL_GPUGraphicsPipelineCreateInfo pipe_info;
    SDL_zero(pipe_info);
    pipe_info.vertex_shader   = vert;
    pipe_info.fragment_shader = frag_yuv;
    pipe_info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pipe_info.target_info.num_color_targets        = 1;
    pipe_info.target_info.color_target_descriptions = &color_desc;

    ps->gpu_pipeline_yuv = SDL_CreateGPUGraphicsPipeline(
        ps->gpu_device, &pipe_info);

    /* Shaders are baked into the pipeline — release the objects */
    SDL_ReleaseGPUShader(ps->gpu_device, frag_yuv);

    if (!ps->gpu_pipeline_yuv) {
        log_msg("ERROR: Failed to create YUV pipeline: %s", SDL_GetError());
        SDL_ReleaseGPUShader(ps->gpu_device, vert);
        return -1;
    }
    log_msg("GPU: swapchain format = %d", (int)color_desc.format);
    log_msg("GPU: YUV planar pipeline created");

    /* Vertex shader done — safe to release now */
    SDL_ReleaseGPUShader(ps->gpu_device, vert);

    /* ── Compile overlay RGBA fragment shader ── */
    SDL_GPUShader *frag_overlay = compile_shader(
        ps->gpu_device, hlsl_overlay_frag, "main",
        SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!frag_overlay) {
        log_msg("ERROR: Overlay shader compile failed");
        return -1;
    }

    /* ── Create overlay pipeline (alpha blending enabled) ──
     *
     * Standard alpha compositing: src.a * src + (1-src.a) * dst.
     * This is the "over" operator — overlay pixels with alpha < 1
     * blend with the video underneath. */
    {
        /* Need a fresh vertex shader since we released vert above */
        SDL_GPUShader *vert_overlay = compile_shader(
            ps->gpu_device, hlsl_fullscreen_vert, "main",
            SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        if (!vert_overlay) {
            SDL_ReleaseGPUShader(ps->gpu_device, frag_overlay);
            log_msg("ERROR: Overlay vertex shader compile failed");
            return -1;
        }

        SDL_GPUColorTargetDescription overlay_color_desc;
        SDL_zero(overlay_color_desc);
        overlay_color_desc.format = SDL_GetGPUSwapchainTextureFormat(
            ps->gpu_device, ps->window);
        overlay_color_desc.blend_state.enable_blend          = true;
        overlay_color_desc.blend_state.src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        overlay_color_desc.blend_state.dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        overlay_color_desc.blend_state.color_blend_op         = SDL_GPU_BLENDOP_ADD;
        overlay_color_desc.blend_state.src_alpha_blendfactor   = SDL_GPU_BLENDFACTOR_ONE;
        overlay_color_desc.blend_state.dst_alpha_blendfactor   = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        overlay_color_desc.blend_state.alpha_blend_op          = SDL_GPU_BLENDOP_ADD;
        overlay_color_desc.blend_state.color_write_mask        =
            SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
            SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

        SDL_GPUGraphicsPipelineCreateInfo overlay_pipe;
        SDL_zero(overlay_pipe);
        overlay_pipe.vertex_shader   = vert_overlay;
        overlay_pipe.fragment_shader = frag_overlay;
        overlay_pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        overlay_pipe.target_info.num_color_targets        = 1;
        overlay_pipe.target_info.color_target_descriptions = &overlay_color_desc;

        ps->gpu_pipeline_overlay = SDL_CreateGPUGraphicsPipeline(
            ps->gpu_device, &overlay_pipe);

        SDL_ReleaseGPUShader(ps->gpu_device, vert_overlay);
        SDL_ReleaseGPUShader(ps->gpu_device, frag_overlay);

        if (!ps->gpu_pipeline_overlay) {
            log_msg("ERROR: Failed to create overlay pipeline: %s", SDL_GetError());
            return -1;
        }
        log_msg("GPU: overlay pipeline created (alpha blend)");
    }

    /* ── Create blit pipeline (frame cache → swapchain, opaque copy) ──
     *
     * Reuses the overlay shader pair (textured fullscreen quad) but with
     * blending disabled — the shaded-frame cache is opaque and replaces
     * the target. Used to present the cached video render so reblit
     * ticks skip the 48-tap YUV shader entirely. */
    {
        SDL_GPUShader *vert_blit = compile_shader(
            ps->gpu_device, hlsl_fullscreen_vert, "main",
            SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *frag_blit = compile_shader(
            ps->gpu_device, hlsl_blit_frag, "main",
            SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!vert_blit || !frag_blit) {
            if (vert_blit) SDL_ReleaseGPUShader(ps->gpu_device, vert_blit);
            if (frag_blit) SDL_ReleaseGPUShader(ps->gpu_device, frag_blit);
            log_msg("ERROR: Blit shader compile failed");
            return -1;
        }

        SDL_GPUColorTargetDescription blit_color_desc;
        SDL_zero(blit_color_desc);
        blit_color_desc.format = SDL_GetGPUSwapchainTextureFormat(
            ps->gpu_device, ps->window);

        SDL_GPUGraphicsPipelineCreateInfo blit_pipe;
        SDL_zero(blit_pipe);
        blit_pipe.vertex_shader   = vert_blit;
        blit_pipe.fragment_shader = frag_blit;
        blit_pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        blit_pipe.target_info.num_color_targets        = 1;
        blit_pipe.target_info.color_target_descriptions = &blit_color_desc;

        ps->gpu_pipeline_blit = SDL_CreateGPUGraphicsPipeline(
            ps->gpu_device, &blit_pipe);

        SDL_ReleaseGPUShader(ps->gpu_device, vert_blit);
        SDL_ReleaseGPUShader(ps->gpu_device, frag_blit);

        if (!ps->gpu_pipeline_blit) {
            log_msg("ERROR: Failed to create blit pipeline: %s", SDL_GetError());
            return -1;
        }
        log_msg("GPU: blit pipeline created (shaded-frame cache)");
    }

    /* ── Samplers (deck d2f5351 port, review PQUALITY P6a) ──
     * ONE nearest sampler serves everything: the fragment shader does
     * its own Lanczos/Catmull-Rom multi-tap resampling at explicit
     * texel centers via SampleLevel(..., 0), so nearest is
     * bit-identical to the old LINEAR binding for the video planes —
     * and the no-bilinear rule stops depending on tap coordinates
     * quantizing to centers. x64 has no PQ LUTs (the one place deck
     * keeps LINEAR), so the linear sampler was deleted outright. */

    /* ── Create nearest-neighbor sampler (video planes + overlay) ──
     * Bitmap font pixels should be pixel-perfect, not bilinear-blurred. */
    SDL_GPUSamplerCreateInfo nearest_info;
    SDL_zero(nearest_info);
    nearest_info.min_filter     = SDL_GPU_FILTER_NEAREST;
    nearest_info.mag_filter     = SDL_GPU_FILTER_NEAREST;
    nearest_info.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    nearest_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    ps->gpu_sampler_nearest = SDL_CreateGPUSampler(ps->gpu_device, &nearest_info);
    if (!ps->gpu_sampler_nearest) {
        log_msg("ERROR: Failed to create nearest sampler: %s", SDL_GetError());
        return -1;
    }
    log_msg("GPU: sampler — NEAREST for video planes and overlay "
            "(kernels do all filtering; deck d2f5351 port)");

    /* ── Create and upload blue noise dither texture (64×64, R8_UNORM) ──
     * Uploaded once at startup. Lives for the entire application lifetime.
     * Nearest-neighbor sampling preserves exact noise values — bilinear
     * would low-pass the texture and destroy its blue spectral character. */
    {
        SDL_GPUTextureCreateInfo noise_tex_info;
        SDL_zero(noise_tex_info);
        noise_tex_info.type                 = SDL_GPU_TEXTURETYPE_2D;
        noise_tex_info.format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        noise_tex_info.width                = 64;
        noise_tex_info.height               = 64;
        noise_tex_info.layer_count_or_depth = 1;
        noise_tex_info.num_levels           = 1;
        noise_tex_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;

        ps->gpu_tex_noise = SDL_CreateGPUTexture(ps->gpu_device, &noise_tex_info);
        if (!ps->gpu_tex_noise) {
            log_msg("ERROR: Failed to create blue noise texture: %s", SDL_GetError());
            return -1;
        }

        /* Upload via transfer buffer */
        SDL_GPUTransferBufferCreateInfo xfer_info;
        SDL_zero(xfer_info);
        xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        xfer_info.size  = 64 * 64;  /* R8 = 1 byte per texel */

        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(
            ps->gpu_device, &xfer_info);
        if (!xfer) {
            log_msg("ERROR: Failed to create blue noise transfer buffer: %s",
                    SDL_GetError());
            return -1;
        }

        uint8_t *dst = SDL_MapGPUTransferBuffer(ps->gpu_device, xfer, false);
        if (dst) {
            memcpy(dst, blue_noise_64, 64 * 64);
            SDL_UnmapGPUTransferBuffer(ps->gpu_device, xfer);
        }

        /* Upload to GPU texture */
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
        if (cmd) {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src_info;
            SDL_GPUTextureRegion dst_region;
            SDL_zero(src_info);
            SDL_zero(dst_region);
            src_info.transfer_buffer = xfer;
            src_info.pixels_per_row  = 64;
            src_info.rows_per_layer  = 64;
            dst_region.texture = ps->gpu_tex_noise;
            dst_region.w = 64;
            dst_region.h = 64;
            dst_region.d = 1;
            SDL_UploadToGPUTexture(copy, &src_info, &dst_region, false);
            SDL_EndGPUCopyPass(copy);
            SDL_SubmitGPUCommandBuffer(cmd);
        }

        SDL_ReleaseGPUTransferBuffer(ps->gpu_device, xfer);
        log_msg("GPU: blue noise dither texture created (64x64 R8_UNORM)");
    }

    return 0;
}

void gpu_destroy_pipelines(PlayerState *ps) {
    if (!ps->gpu_device) return;

    gpu_overlay_destroy(ps);

    if (ps->gpu_sampler_nearest) {
        SDL_ReleaseGPUSampler(ps->gpu_device, ps->gpu_sampler_nearest);
        ps->gpu_sampler_nearest = NULL;
    }
    if (ps->gpu_tex_noise) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_noise);
        ps->gpu_tex_noise = NULL;
    }
    if (ps->gpu_pipeline_yuv) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv);
        ps->gpu_pipeline_yuv = NULL;
    }
    if (ps->gpu_pipeline_blit) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_blit);
        ps->gpu_pipeline_blit = NULL;
    }
    if (ps->gpu_pipeline_overlay) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_overlay);
        ps->gpu_pipeline_overlay = NULL;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Texture & Transfer Buffer Helpers (per-file lifetime)
 * ═══════════════════════════════════════════════════════════════════ */

/* Create GPU textures and transfer buffers for the current video.
 *
 * Two paths:
 *   8-bit (YUV420P):      3 × R8_UNORM planar textures (Y, U, V)
 *   10-bit (YUV420P10LE): 3 × R16_UNORM planar textures (Y, U, V)
 *
 * Both paths use the same YUV planar shader — Texture2D<float> reads
 * the .r channel from either format. The 10-bit path bypasses swscale
 * entirely; raw frame data goes straight to GPU. */
static int gpu_create_video_textures(PlayerState *ps) {
    int w = ps->vid_w;
    int h = ps->vid_h;
    /* ceil — FFmpeg allocates ceil(w/2) chroma for odd dimensions;
     * truncating dropped the last chroma column/row and skewed the
     * chroma texture geometry by half a texel across the frame. */
    int cw = (w + 1) / 2;  /* chroma width  (4:2:0) */
    int ch = (h + 1) / 2;  /* chroma height (4:2:0) */

    /* Key on the UPLOAD format, not the codec format: deep sources on
     * the swscale path now land in yuv420p10le and need R16 too. */
    int is_10bit = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
                    && !ps->sws_ctx)
                   || (ps->sws_ctx && ps->sws_out_10bit);

    SDL_GPUTextureFormat fmt = is_10bit
        ? SDL_GPU_TEXTUREFORMAT_R16_UNORM
        : SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    int bpp = is_10bit ? 2 : 1;  /* bytes per sample */

    if (!SDL_GPUTextureSupportsFormat(ps->gpu_device, fmt,
            SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        log_msg("ERROR: GPU lacks %s sampling — cannot display this file",
                is_10bit ? "R16_UNORM" : "R8_UNORM");
        return -1;
    }

    SDL_GPUTextureCreateInfo tex_info;
    SDL_zero(tex_info);
    tex_info.type                  = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format                = fmt;
    tex_info.layer_count_or_depth  = 1;
    tex_info.num_levels            = 1;
    tex_info.usage                 = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    /* Y plane (full resolution) */
    tex_info.width  = w;
    tex_info.height = h;
    ps->gpu_tex_y = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_y) {
        log_msg("ERROR: Failed to create Y texture: %s", SDL_GetError());
        return -1;
    }

    /* U plane (half resolution) */
    tex_info.width  = cw;
    tex_info.height = ch;
    ps->gpu_tex_u = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_u) {
        log_msg("ERROR: Failed to create U texture: %s", SDL_GetError());
        return -1;
    }

    /* V plane (half resolution) */
    ps->gpu_tex_v = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_v) {
        log_msg("ERROR: Failed to create V texture: %s", SDL_GetError());
        return -1;
    }

    /* Transfer buffers (CPU→GPU staging).
     *
     * Each row is padded up to 256 bytes so a stride-padded source frame
     * (FFmpeg aligns linesize to 32/64/128) fits with ONE memcpy —
     * the GPU copy then skips the padding via pixels_per_row. Falls back
     * to per-row tight packing if a frame's stride ever exceeds this. */
    SDL_GPUTransferBufferCreateInfo xfer_info;
    SDL_zero(xfer_info);
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    Uint32 row_y  = ((Uint32)(w  * bpp) + 255u) & ~255u;
    Uint32 row_uv = ((Uint32)(cw * bpp) + 255u) & ~255u;

    ps->gpu_xfer_y_cap = row_y * (Uint32)h;
    xfer_info.size = ps->gpu_xfer_y_cap;
    ps->gpu_xfer_y = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);

    ps->gpu_xfer_uv_cap = row_uv * (Uint32)ch;
    xfer_info.size = ps->gpu_xfer_uv_cap;
    ps->gpu_xfer_u = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);
    ps->gpu_xfer_v = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);

    if (!ps->gpu_xfer_y || !ps->gpu_xfer_u || !ps->gpu_xfer_v) {
        log_msg("ERROR: Failed to create transfer buffers: %s", SDL_GetError());
        return -1;
    }

    log_msg("GPU: textures created (Y=%dx%d, UV=%dx%d, %s planar)",
            w, h, cw, ch,
            is_10bit ? "R16_UNORM 10-bit" : "R8_UNORM");
    return 0;
}

/* Destroy per-file GPU resources. */
static void gpu_destroy_video_textures(PlayerState *ps) {
    if (!ps->gpu_device) return;

    if (ps->gpu_tex_y)  { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_y);  ps->gpu_tex_y  = NULL; }
    if (ps->gpu_tex_u)  { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_u);  ps->gpu_tex_u  = NULL; }
    if (ps->gpu_tex_v)  { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_v);  ps->gpu_tex_v  = NULL; }
    if (ps->gpu_xfer_y) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_y); ps->gpu_xfer_y = NULL; }
    if (ps->gpu_xfer_u) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_u); ps->gpu_xfer_u = NULL; }
    if (ps->gpu_xfer_v) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_v); ps->gpu_xfer_v = NULL; }
    ps->gpu_xfer_y_cap  = 0;
    ps->gpu_xfer_uv_cap = 0;

    /* Shaded-frame cache */
    if (ps->gpu_tex_cache) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_cache);
        ps->gpu_tex_cache = NULL;
    }
    ps->cache_w = 0;
    ps->cache_h = 0;
    ps->cache_valid = 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Uniform Setup
 * ═══════════════════════════════════════════════════════════════════
 *
 * Sets the YUV→RGB color matrix and range parameters based on the
 * video's colorspace metadata. Called once per file in player_open().
 *
 * Three modes:
 *   10-bit passthrough (yuv420p10le): range expansion in shader (R16_UNORM)
 *   8-bit passthrough  (yuv420p):     range expansion in shader (R8_UNORM)
 *   swscale fallback:                 swscale does range → identity uniforms
 */

/* CPU-side scalar PQ OETF (nits -> code [0,1]). Log lines that quote
 * the tone map's KS/maxLum must compute them in the SAME PQ domain the
 * shader uses — a linear-domain KS in a log line reads as the old
 * domain bug's fingerprint (panel and log must never disagree about
 * the same number). */
static double pq_oetf_cpu(double nits) {
    const double pm1 = 0.1593017578125, pm2 = 78.84375;
    const double pc1 = 0.8359375, pc2 = 18.8515625, pc3 = 18.6875;
    double np = pow(nits / 10000.0, pm1);
    return pow((pc1 + pc2*np)/(1.0 + pc3*np), pm2);
}

/* Pick the YCbCr matrix (601/709/2020) for a stream (deck 315ee1c
 * port, review PQUALITY P2). The tag is authoritative. For
 * untagged/unmatched streams the fallback chain is primaries/transfer
 * first — re-encodes strip the matrix flag far more often than
 * primaries or PQ/HLG — then a size heuristic keyed on width AND
 * height (scope-cropped HD keeps its 1280+ width at sub-720 heights;
 * height alone sent that whole class to 601). Shared with the sws
 * leg so an sws-encoded stream is always decoded with the matrix it
 * was encoded with. cs_reason names which rule fired. */
static int pick_ycbcr_matrix(const AVCodecParameters *par,
                             int w, int h, const char **cs_reason) {
    *cs_reason = "tag";
    if (par) {
        if (par->color_space == AVCOL_SPC_BT709)  return 709;
        if (par->color_space == AVCOL_SPC_BT470BG ||
            par->color_space == AVCOL_SPC_SMPTE170M) return 601;
        if (par->color_space == AVCOL_SPC_BT2020_NCL) return 2020;
        if (par->color_space == AVCOL_SPC_BT2020_CL) {
            /* CL math is unimplemented; NCL is the least-wrong decode
             * and keeps the primaries chain consistent. */
            *cs_reason = "tag BT.2020-CL, decoded as NCL";
            return 2020;
        }
        if (par->color_primaries == AVCOL_PRI_BT2020 ||
            par->color_trc == AVCOL_TRC_SMPTE2084 ||
            par->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            *cs_reason = "primaries/transfer fallback";
            return 2020;
        }
        if (par->color_primaries == AVCOL_PRI_BT709) {
            *cs_reason = "primaries fallback";
            return 709;
        }
        if (par->color_primaries == AVCOL_PRI_SMPTE170M ||
            par->color_primaries == AVCOL_PRI_BT470BG) {
            *cs_reason = "primaries fallback";
            return 601;
        }
    }
    *cs_reason = "size heuristic";
    return (h >= 720 || w >= 1280) ? 709 : 601;
}

static void gpu_setup_uniforms(PlayerState *ps) {
    /* Determine YCbCr matrix — see pick_ycbcr_matrix. */
    const char *cs_reason = "no stream";
    AVCodecParameters *upar = NULL;
    if (ps->fmt_ctx)
        upar = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
    int colorspace = pick_ycbcr_matrix(upar, ps->vid_w, ps->vid_h,
                                       &cs_reason);

    const char *cs_name = (colorspace == 2020) ? "BT.2020"
                        : (colorspace == 709)  ? "BT.709" : "BT.601";
    log_msg("GPU: YCbCr matrix %s (%s)", cs_name, cs_reason);

    /* BT.2390 black-lift config (deck 4614882 port; see dsvp.h
     * out_black_nits): parsed once. x64 DEFAULT = 0.01 nits FIXED —
     * Holden's eye, ladder-tuned on WIN11 IPS 2026-08-27 ("best
     * balance between lift and ink, specks practically invisible"):
     * IPS-class native black is ~0.1-0.3 nits, so the target/2000
     * auto floor rendered ~8/255 codes and lifted WEB-DL baked-bar
     * codec noise into view; 0.01 renders ~3 codes — detail above
     * the crush survives, bar noise sits at dark-room threshold.
     * PLATFORM DIVERGENCE: deck keeps target/2000 auto (OLED renders
     * those floors for real). DSVP_BLACK_NITS overrides: "0" = lift
     * off; 0<v<=10 = fixed nits. */
    {
        static float s_black_nits = -2.0f;   /* -2 = unparsed */
        static const char *s_black_src = "x64 default";
        if (s_black_nits == -2.0f) {
            s_black_nits = 0.01f;
            const char *be = SDL_getenv("DSVP_BLACK_NITS");
            if (be && be[0]) {
                double bv = SDL_atof(be);
                if (bv == 0.0) {
                    s_black_nits = -1.0f;
                    s_black_src = "DSVP_BLACK_NITS=0";
                } else if (bv > 0.0 && bv <= 10.0) {
                    s_black_nits = (float)bv;
                    s_black_src = "DSVP_BLACK_NITS";
                } else {
                    log_msg("WARN: DSVP_BLACK_NITS='%s' ignored "
                            "(want 0 to disable, or 0-10 nits) — "
                            "x64 default 0.01", be);
                }
            }
        }
        ps->gpu_uniforms.out_black_nits = s_black_nits;
        ps->black_src = s_black_src;
    }

    /* ── Range parameters ──
     *
     * Three passthrough modes, all handling range expansion in shader:
     *
     * 10-bit passthrough (yuv420p10le → R16_UNORM):
     *   GPU reads uint16 V as V/65535.
     *   Limited: Y 64-940, UV 64-960
     *   Full:    Y/UV 0-1023
     *
     * 8-bit passthrough (yuv420p → R8_UNORM):
     *   GPU reads uint8 V as V/255.
     *   Limited: Y 16-235, UV 16-240
     *   Full:    identity {0, 1}
     *
     * swscale fallback (other formats → yuv420p full-range):
     *   swscale outputs full-range → identity {0, 1}.
     */
    int is_10bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
         && !ps->sws_ctx);
    int is_8bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P
         && !ps->sws_ctx);

    /* Read color range from metadata */
    int is_full_range = 0;
    if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
        is_full_range = (par->color_range == AVCOL_RANGE_JPEG);
    }

    if (is_10bit_passthrough) {
        /* 10-bit passthrough — range correction in shader */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
            /* Full-range chroma neutral is 2^(n-1), but the shader computes
             * code/(2^n - 1) - 0.5, which puts neutral half a code low
             * (H.273: E_Cb = (code - 2^(n-1))/(2^n - 1)). Half-LSB offset
             * removes a constant colour cast. Limited range is already exact. */
            ps->gpu_uniforms.rangeUV[0] = 0.5f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 64.0f / 65535.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / (940.0f - 64.0f);
            ps->gpu_uniforms.rangeUV[0] = 64.0f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / (960.0f - 64.0f);
        }

        log_msg("GPU: uniforms set (%s, 10-bit %s range → shader)",
                cs_name, is_full_range ? "full" : "limited");

    } else if (is_8bit_passthrough) {
        /* 8-bit YUV420P passthrough — range correction in shader.
         * R8_UNORM reads uint8 V as V/255. */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 1.0f;
            /* half-LSB full-range chroma neutral — see 10-bit note above */
            ps->gpu_uniforms.rangeUV[0] = 0.5f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 1.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeY[1]  = 255.0f / (235.0f - 16.0f);
            ps->gpu_uniforms.rangeUV[0] = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 255.0f / (240.0f - 16.0f);
        }

        log_msg("GPU: uniforms set (%s, 8-bit %s range → shader)",
                cs_name, is_full_range ? "full" : "limited");

    } else if (ps->sws_out_10bit) {
        /* swscale fallback, 10-bit destination — full-range yuv420p10le
         * through the R16 path. Same math as 10-bit full-range
         * passthrough: codes 0..1023 in 16-bit words, half-LSB neutral. */
        ps->gpu_uniforms.rangeY[0]  = 0.0f;
        ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
        ps->gpu_uniforms.rangeUV[0] = 0.5f / 65535.0f;
        ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;

        log_msg("GPU: uniforms set (%s, full range 10-bit via swscale)",
                cs_name);

    } else {
        /* swscale fallback — outputs full-range YUV420P, identity range.
         * Chroma gets the half-LSB neutral offset (see 10-bit note above):
         * swscale output is JPEG-range with neutral at code 128. */
        ps->gpu_uniforms.rangeY[0]  = 0.0f;
        ps->gpu_uniforms.rangeY[1]  = 1.0f;
        ps->gpu_uniforms.rangeUV[0] = 0.5f / 255.0f;
        ps->gpu_uniforms.rangeUV[1] = 1.0f;

        log_msg("GPU: uniforms set (%s, full range via swscale)",
                cs_name);
    }

    /* Color matrix: row-major (matches HLSL row_major qualifier).
     *
     * Standard YUV→RGB for full-range input where Cb,Cr are centered:
     *   R = Y + 0     * (Cb-0.5) + Cr_coeff * (Cr-0.5)
     *   G = Y + Cb_g  * (Cb-0.5) + Cr_g    * (Cr-0.5)
     *   B = Y + Cb_b  * (Cb-0.5) + 0       * (Cr-0.5)
     */
    float *m = ps->gpu_uniforms.colorMatrix;
    memset(m, 0, 16 * sizeof(float));

    if (colorspace == 2020) {
        /* BT.2020 NCL: Kr=0.2627, Kb=0.0593 */
        m[ 0] = 1.0f;  m[ 1] =  0.0f;     m[ 2] =  1.4746f;  /* R */
        m[ 4] = 1.0f;  m[ 5] = -0.1646f;  m[ 6] = -0.5714f;  /* G */
        m[ 8] = 1.0f;  m[ 9] =  1.8814f;  m[10] =  0.0f;     /* B */
    } else if (colorspace == 709) {
        /* BT.709: Kr=0.2126, Kb=0.0722 */
        m[ 0] = 1.0f;  m[ 1] =  0.0f;     m[ 2] =  1.5748f;  /* R */
        m[ 4] = 1.0f;  m[ 5] = -0.1873f;  m[ 6] = -0.4681f;  /* G */
        m[ 8] = 1.0f;  m[ 9] =  1.8556f;  m[10] =  0.0f;     /* B */
    } else {
        /* BT.601: Kr=0.299, Kb=0.114 */
        m[ 0] = 1.0f;  m[ 1] =  0.0f;     m[ 2] =  1.402f;   /* R */
        m[ 4] = 1.0f;  m[ 5] = -0.3441f;  m[ 6] = -0.7141f;  /* G */
        m[ 8] = 1.0f;  m[ 9] =  1.772f;   m[10] =  0.0f;     /* B */
    }
    m[15] = 1.0f;  /* A passthrough */

    /* ── Texture dimensions for Lanczos resampling ──
     *
     * The fragment shader needs texel size to compute sample positions
     * for the Lanczos-2 4×4 kernel. Y plane is full resolution;
     * UV planes are half (4:2:0 chroma subsampling). */
    ps->gpu_uniforms.texSizeY[0]  = (float)ps->vid_w;
    ps->gpu_uniforms.texSizeY[1]  = (float)ps->vid_h;
    ps->gpu_uniforms.texSizeUV[0] = (float)((ps->vid_w + 1) / 2);
    ps->gpu_uniforms.texSizeUV[1] = (float)((ps->vid_h + 1) / 2);

    /* ── Chroma siting correction ──
     *
     * 4:2:0 chroma samples may be co-sited with luma at different sub-texel
     * positions depending on the codec. The Catmull-Rom kernel assumes samples
     * are at texel centers (CENTER siting). For other sitings, we offset the
     * chroma UV coordinate so the kernel reconstructs at the correct position.
     *
     * Math: in 4:2:0, each chroma texel spans 2 luma pixels. CENTER places the
     * sample at the midpoint of this span (texel center — no correction).
     * LEFT co-sites with the left luma column, which is 0.5 luma pixels = 0.25
     * chroma texels away from center. Shader applies: uv + offset / texSizeUV.
     *
     * Sign convention: these are TEXTURE-COORDINATE offsets, not swscale
     * filter phases (the two are negatives of each other). For LEFT siting
     * the chroma sample sits at (i+0.25)/C_w in chroma-normalized units while
     * the kernel assumes (i+0.5)/C_w, so the lookup must move +0.25 texels.
     * Check: output pixel 0 of a 1:1 blit samples uv=0.5/W; +0.25 lands the
     * kernel exactly on chroma sample 0 — single tap, weight 1, the
     * definition of co-sited. Verified against H.273 §8.7, mpv, libplacebo,
     * zimg, swscale during the 2026-07-31/08-02 review rounds; the previous
     * negated table displaced chroma a full luma pixel the wrong way.
     */
    enum AVChromaLocation chroma_loc = AVCHROMA_LOC_LEFT; /* safe default */
    if (ps->sws_ctx) {
        /* swscale path: the OUTPUT siting was pinned explicitly via
         * dst_chr_pos at context creation (source siting for 4:2:0
         * inputs — same-geometry conversions don't move chroma — LEFT
         * for genuinely resampled 422/444/RGB inputs). Reconstruct at
         * that siting. Replaces the old blanket zero-offset, whose
         * "sws re-sites to center" premise was false for unscaled
         * depth conversions. */
        chroma_loc = (enum AVChromaLocation)ps->sws_dst_siting;
    } else if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
        if (par->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
            chroma_loc = par->chroma_location;
        else if (content_is_2020_family(par))
            /* BT.2020-family sites 4:2:0 chroma TOP-LEFT per spec, and
             * re-encodes strip the VUI flag constantly — the LEFT
             * fallback gave most HDR files a quarter-texel vertical
             * chroma error (deck ad4ab7f lineage). Family membership by
             * ANY signal (tags, PQ/HLG, DV conf): tag-only matching
             * missed DV P5 and most stripped re-encodes in the field. */
            chroma_loc = AVCHROMA_LOC_TOPLEFT;
    }
    ps->chroma_location = (int)chroma_loc;

    switch (chroma_loc) {
        case AVCHROMA_LOC_CENTER:
            ps->gpu_uniforms.chromaOffset[0] =  0.0f;
            ps->gpu_uniforms.chromaOffset[1] =  0.0f;
            break;
        case AVCHROMA_LOC_TOPLEFT:
            ps->gpu_uniforms.chromaOffset[0] =  0.25f;
            ps->gpu_uniforms.chromaOffset[1] =  0.25f;
            break;
        case AVCHROMA_LOC_TOP:
            ps->gpu_uniforms.chromaOffset[0] =  0.0f;
            ps->gpu_uniforms.chromaOffset[1] =  0.25f;
            break;
        case AVCHROMA_LOC_BOTTOMLEFT:
            ps->gpu_uniforms.chromaOffset[0] =  0.25f;
            ps->gpu_uniforms.chromaOffset[1] = -0.25f;
            break;
        case AVCHROMA_LOC_BOTTOM:
            ps->gpu_uniforms.chromaOffset[0] =  0.0f;
            ps->gpu_uniforms.chromaOffset[1] = -0.25f;
            break;
        default: /* LEFT and fallback */
            ps->gpu_uniforms.chromaOffset[0] =  0.25f;
            ps->gpu_uniforms.chromaOffset[1] =  0.0f;
            break;
    }

    /* (No sws zero-out here anymore: the shader offset above is derived
     * from the explicitly pinned sws OUTPUT siting when sws is active —
     * see the chroma_loc selection.) */

    ps->gpu_uniforms.frameCount = 0.0f;

    /* ── HDR Detection & Metadata ──
     *
     * HDR detection priority (per industry consensus — mpv, MPC, VLC):
     *   1. color_trc == SMPTE2084 (PQ) — catches all HDR10 content
     *   2. DOVI_CONF in coded_side_data — catches DV P5 where color_trc
     *      is often UNSPECIFIED
     *   3. color_trc == ARIB_STD_B67 (HLG) — rendered via inverse OETF
     *      + BT.2100 OOTF into the shared BT.2390 tone-map path
     *
     * Primaries classification (separate from HDR detection):
     *   - color_primaries == BT2020 → true BT.2020, needs gamut mapping
     *   - DV P5: per-frame RPU processing (polynomial + MMR reshaping)
     *     drives the DV decode chain; see dovi_populate_uniforms
     *
     * Peak luminance priority:
     *   1. MaxCLL from content light level metadata
     *   2. max_luminance from mastering display metadata
     *   3. 1000 nit fallback (standard for most HDR10 content)
     */
    int is_hdr = 0;
    int is_hlg = 0;
    int is_dolby_vision = 0;
    int has_pq_transfer = 0;
    int dv_profile = -1;   /* hoisted for the P5-only gate (deck ccede14) */
    float peak_nits = 0.0f;
    int has_bt2020_primaries = 0;

    if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;

        /* Honest instrument (field 2026-09-07: a properly tagged
         * synthetic PQ clip — container AND VUI say smpte2084 to
         * ffprobe — opened with no HDR line at all, and nothing logged
         * what the player's own libavformat actually reported). Stream
         * values after the open-time VUI backfill, and the decoder's
         * own, by name and number, every open. */
        {
            const AVCodecContext *cc = ps->video_codec_ctx;
            const char *s_trc = av_color_transfer_name(par->color_trc);
            const char *s_pri = av_color_primaries_name(par->color_primaries);
            const char *s_spc = av_color_space_name(par->color_space);
            const char *s_rng = av_color_range_name(par->color_range);
            int d_trc = cc ? (int)cc->color_trc : -1;
            int d_pri = cc ? (int)cc->color_primaries : -1;
            const char *d_trc_n = cc ? av_color_transfer_name(cc->color_trc) : "-";
            const char *d_pri_n = cc ? av_color_primaries_name(cc->color_primaries) : "-";
            log_msg("Colour tags: stream trc=%s(%d) primaries=%s(%d) matrix=%s(%d) range=%s"
                    " | decoder trc=%s(%d) primaries=%s(%d)",
                    s_trc ? s_trc : "?", (int)par->color_trc,
                    s_pri ? s_pri : "?", (int)par->color_primaries,
                    s_spc ? s_spc : "?", (int)par->color_space,
                    s_rng ? s_rng : "?",
                    d_trc_n ? d_trc_n : "?", d_trc,
                    d_pri_n ? d_pri_n : "?", d_pri);
        }

        /* --- Transfer function check --- */
        if (par->color_trc == AVCOL_TRC_SMPTE2084) {
            is_hdr = 1;
            has_pq_transfer = 1;
            log_msg("HDR: detected PQ transfer (SMPTE ST 2084)");
        } else if (par->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            /* HLG: shader converts inverse-OETF + BT.2100 OOTF (Lw=1000)
             * to display light, then the shared BT.2390 path tone-maps.
             * Previously detected-but-rendered-as-SDR: washed out and
             * desaturated on every HLG file. */
            is_hdr = 1;
            is_hlg = 1;
            log_msg("HDR: detected HLG transfer (ARIB STD-B67)");
        }

        /* --- Dolby Vision fallback (DV P5 often has UNSPECIFIED trc) --- */
        const AVPacketSideData *dovi_sd = av_packet_side_data_get(
            par->coded_side_data, par->nb_coded_side_data,
            AV_PKT_DATA_DOVI_CONF);
        if (dovi_sd) {
            const AVDOVIDecoderConfigurationRecord *cfg =
                (const AVDOVIDecoderConfigurationRecord *)dovi_sd->data;
            dv_profile = cfg->dv_profile;
            is_dolby_vision = 1;
            if (!is_hdr) {
                is_hdr = 1;
                log_msg("HDR: detected Dolby Vision Profile %d (DOVI conf in stream)",
                        dv_profile);
            } else {
                log_msg("HDR: Dolby Vision Profile %d metadata also present",
                        dv_profile);
            }
        }

        /* --- Primaries classification --- */
        if (par->color_primaries == AVCOL_PRI_BT2020) {
            has_bt2020_primaries = 1;
        }

        /* --- Static metadata: peak luminance ---
         * Mastering max is read FIRST so MaxCLL can be sanitized
         * against it (deck 5d8076f port, review PQUALITY P1): the
         * tone map used to take MaxCLL raw — the field's
         * 9918-on-a-4000-nit-master disc tone-mapped against 9918
         * (knee ~30 nits instead of ~48). Trust the mastering block
         * (measured at the facility) over CLL (computed by tools). */
        float mast_max_nits = 0.0f;
        {
            const AVPacketSideData *mdm_sd = av_packet_side_data_get(
                par->coded_side_data, par->nb_coded_side_data,
                AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
            if (mdm_sd && mdm_sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
                const AVMasteringDisplayMetadata *mdm =
                    (const AVMasteringDisplayMetadata *)mdm_sd->data;
                if (mdm->has_luminance && av_q2d(mdm->max_luminance) > 0.0)
                    mast_max_nits = (float)av_q2d(mdm->max_luminance);
            }
        }

        const AVPacketSideData *cll_sd = av_packet_side_data_get(
            par->coded_side_data, par->nb_coded_side_data,
            AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
        if (cll_sd && cll_sd->size >= (int)sizeof(AVContentLightMetadata)) {
            const AVContentLightMetadata *cll =
                (const AVContentLightMetadata *)cll_sd->data;
            if (cll->MaxCLL > 0) {
                peak_nits = (float)cll->MaxCLL;
                log_msg("HDR: MaxCLL=%u nits, MaxFALL=%u nits",
                        cll->MaxCLL, cll->MaxFALL);
                if (mast_max_nits > 0.0f && peak_nits > mast_max_nits) {
                    log_msg("HDR: MaxCLL %.0f exceeds mastering peak %.0f "
                            "— tone-map peak clamped (trust the mastering "
                            "block)", peak_nits, mast_max_nits);
                    peak_nits = mast_max_nits;
                }
            }
        }

        if (peak_nits == 0.0f) {
            const AVPacketSideData *mdm_sd = av_packet_side_data_get(
                par->coded_side_data, par->nb_coded_side_data,
                AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
            if (mdm_sd && mdm_sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
                const AVMasteringDisplayMetadata *mdm =
                    (const AVMasteringDisplayMetadata *)mdm_sd->data;
                if (mdm->has_luminance) {
                    double max_lum = av_q2d(mdm->max_luminance);
                    if (max_lum > 0.0) {
                        peak_nits = (float)max_lum;
                        log_msg("HDR: mastering display max=%.0f nits, min=%.4f nits",
                                max_lum, av_q2d(mdm->min_luminance));
                    }
                }
                if (mdm->has_primaries) {
                    log_msg("HDR: mastering primaries: "
                            "R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) WP(%.4f,%.4f)",
                            av_q2d(mdm->display_primaries[0][0]),
                            av_q2d(mdm->display_primaries[0][1]),
                            av_q2d(mdm->display_primaries[1][0]),
                            av_q2d(mdm->display_primaries[1][1]),
                            av_q2d(mdm->display_primaries[2][0]),
                            av_q2d(mdm->display_primaries[2][1]),
                            av_q2d(mdm->white_point[0]),
                            av_q2d(mdm->white_point[1]));
                }
            }
        }

        /* Belt: nothing above 10000 nits exists in PQ (deck 5d8076f
         * port). A garbage declared peak with no mastering block to
         * clamp against otherwise pushes maxE past 1.0 and crushes
         * the whole image under the knee. */
        if (peak_nits > 10000.0f) {
            log_msg("HDR: declared peak %.0f exceeds the PQ ceiling "
                    "— tone-map peak clamped to 10000", peak_nits);
            peak_nits = 10000.0f;
        }

        /* Fallback: no metadata → 1000 nits (standard HDR10 assumption) */
        if (is_hdr && peak_nits == 0.0f) {
            peak_nits = 1000.0f;
            log_msg("HDR: no luminance metadata — using 1000 nit fallback");
        }

        /* DV P5 base layer is full-range by spec (IPTPQc2).
         * Range override applied after HDR detection completes. */
        if (dv_profile == 5) {
            log_msg("HDR: DV Profile 5 detected — full-range override pending");
        }
    }

    /* Gamut classification for the shader:
     * - DV P5: output after DV reshaping is BT.2020 (always)
     * - HDR10 with BT.2020 primaries: needs gamut mapping in tone map.
     * - DV P5 without explicit BT.2020 primaries: DV decode handles gamut. */
    float hdr_gamut = 0.0f; /* 0.0 = BT.709 primaries */
    int is_dovi_active = 0;
    /* The IPTPQc2 reshape pipeline is PROFILE 5, by profile — not
     * "any DV without a PQ tag" (deck ccede14 port, review PQUALITY
     * M9): DV 8.4 is HLG-base (the phone-camera class) and satisfied
     * the old gate, sending limited-range HLG through the P5
     * full-range override and IPT math. 8.4 now falls through to the
     * standard path, where is_hlg routes it through the HLG pipeline
     * (BL-compatible, as an HLG TV would play it). */
    if (is_hdr && is_dolby_vision && !has_pq_transfer && dv_profile != 5)
        log_msg("HDR: DV Profile %d with non-PQ base — BL-compatible "
                "standard path (%s pipeline), DV reshape not applicable",
                dv_profile, is_hlg ? "HLG" : "SDR/other");
    if (is_hdr && is_dolby_vision && !has_pq_transfer && dv_profile == 5) {
        /* DV-only (no PQ transfer tag, Profile 5):
         * Base layer is IPTPQc2 — needs DV reshaping pipeline.
         * The DV decode chain outputs BT.2020, so set gamut accordingly.
         * DV uniforms will be populated from first decoded frame's RPU.
         *
         * Spec-conforming P5 is always 10-bit 4:2:0; the full-range
         * override below hardwires R16 10-bit scale factors, so a
         * mistagged/nonconforming stream on any other upload path
         * would get a 64x range scale (white garbage, no diagnostic).
         * Guard: DV requires the 10-bit passthrough path. */
        if (is_10bit_passthrough) {
            is_dovi_active = 1;
            hdr_gamut = 1.0f;
            log_msg("HDR: Dolby Vision Profile 5 — DV reshape pipeline active");
        } else {
            log_msg("WARN: DV P5 tagged but not 10-bit 4:2:0 passthrough "
                    "— DV pipeline disabled for this file");
        }
    } else if (is_hdr && has_bt2020_primaries) {
        hdr_gamut = 1.0f;   /* 1.0 = BT.2020 primaries */
    } else if (!is_hdr && has_bt2020_primaries) {
        /* SDR tagged BT.2020: shader converts primaries to 709 in linear
         * light (was displayed unconverted — visibly desaturated). */
        hdr_gamut = 1.0f;
        log_msg("SDR BT.2020 content — gamut conversion to 709 active");
    }

    /* HLG carries no mastering metadata worth trusting; nominal 1000-nit
     * OOTF output. The PQ-domain scene-peak histogram doesn't apply. */
    if (is_hlg)
        peak_nits = 1000.0f;

    ps->hdr_pass_content = is_hdr;   /* hdr_output_apply() consumes */

    ps->gpu_uniforms.is_hdr        = is_hdr ? 1.0f : 0.0f;
    ps->gpu_uniforms.is_hlg        = is_hlg ? 1.0f : 0.0f;
    ps->gpu_uniforms.hdr_peak_nits = peak_nits;
    ps->gpu_uniforms.hdr_gamut     = hdr_gamut;
    ps->gpu_uniforms.hdr_debug     = 0.0f;
    ps->gpu_uniforms.is_dovi       = is_dovi_active ? 1.0f : 0.0f;

    /* Output transfer for tone-mapped content. Displays decode ~2.2
     * regardless of "sRGB support" — the sRGB piecewise toe lifts
     * shadows on a calibrated screen, so pure power 2.2 is the
     * reference-faithful default. DSVP_OUTPUT_GAMMA=srgb|2.2|2.4
     * overrides (2.4 = BT.1886 dark-room grading environment). */
    {
        float out_gamma = 2.2f;
        const char *og = SDL_getenv("DSVP_OUTPUT_GAMMA");
        if (og) {
            if (SDL_strcasecmp(og, "srgb") == 0) out_gamma = 0.0f;
            else {
                double v = SDL_atof(og);
                if (v >= 1.0 && v <= 3.0) out_gamma = (float)v;
                else log_msg("WARN: DSVP_OUTPUT_GAMMA='%s' ignored", og);
            }
        }
        ps->gpu_uniforms.out_gamma = out_gamma;
        log_msg("Output transfer: %s",
                out_gamma == 0.0f ? "sRGB piecewise" :
                out_gamma == 2.2f ? "gamma 2.2 (default)" : "custom gamma");
    }

    /* DV P5 range override: container says limited but IPTPQc2 is full-range.
     * Must happen after normal range setup since it overrides those values. */
    if (is_dovi_active) {
        ps->gpu_uniforms.rangeY[0]  = 0.0f;
        ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
        ps->gpu_uniforms.rangeUV[0] = 0.0f;
        ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;
        log_msg("GPU: DV P5 — range overridden to full-range 10-bit");
    }

    /* Initialize DV uniforms to identity (populated from first frame RPU) */
    if (is_dovi_active) {
        /* Identity reshape: 1 piece per component, pivots [0,1], out = x */
        memset(ps->gpu_uniforms.dovi_num_pieces, 0, sizeof(ps->gpu_uniforms.dovi_num_pieces));
        memset(ps->gpu_uniforms.dovi_pivots, 0, sizeof(ps->gpu_uniforms.dovi_pivots));
        memset(ps->gpu_uniforms.dovi_c0, 0, sizeof(ps->gpu_uniforms.dovi_c0));
        memset(ps->gpu_uniforms.dovi_c1, 0, sizeof(ps->gpu_uniforms.dovi_c1));
        memset(ps->gpu_uniforms.dovi_c2, 0, sizeof(ps->gpu_uniforms.dovi_c2));
        for (int c = 0; c < 3; c++) {
            ps->gpu_uniforms.dovi_num_pieces[c] = 1.0f;
            ps->gpu_uniforms.dovi_pivots[0][c] = 0.0f;
            ps->gpu_uniforms.dovi_pivots[1][c] = 1.0f;
            ps->gpu_uniforms.dovi_c0[0][c] = 0.0f;  /* c0 = 0 */
            ps->gpu_uniforms.dovi_c1[0][c] = 1.0f;  /* c1 = 1 → out = x */
            ps->gpu_uniforms.dovi_c2[0][c] = 0.0f;
        }
        /* MMR off until the first RPU says otherwise (stale values from
         * a previous file would corrupt chroma for the pre-RPU frames) */
        memset(ps->gpu_uniforms.dovi_mmr_meta, 0,
               sizeof(ps->gpu_uniforms.dovi_mmr_meta));
        memset(ps->gpu_uniforms.dovi_mmr_ct, 0,
               sizeof(ps->gpu_uniforms.dovi_mmr_ct));
        memset(ps->gpu_uniforms.dovi_mmr_cp, 0,
               sizeof(ps->gpu_uniforms.dovi_mmr_cp));
        /* Identity matrices (will be overwritten by first frame) */
        memset(ps->gpu_uniforms.dovi_ycc_r0, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_ycc_r1, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_ycc_r2, 0, 4 * sizeof(float));
        ps->gpu_uniforms.dovi_ycc_r0[0] = 1.0f;
        ps->gpu_uniforms.dovi_ycc_r1[1] = 1.0f;
        ps->gpu_uniforms.dovi_ycc_r2[2] = 1.0f;
        memset(ps->gpu_uniforms.dovi_out_r0, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_out_r1, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_out_r2, 0, 4 * sizeof(float));
        ps->gpu_uniforms.dovi_out_r0[0] = 1.0f;
        ps->gpu_uniforms.dovi_out_r1[1] = 1.0f;
        ps->gpu_uniforms.dovi_out_r2[2] = 1.0f;
    }

    /* SDR target nits — preserved across file opens (N key cycles).
     * Only initialize to default if not already set by a previous file. */
    if (ps->gpu_uniforms.hdr_target_nits < 1.0f)
        ps->gpu_uniforms.hdr_target_nits = 203.0f;

    /* Save static peak as ceiling for dynamic detection.
     * Initialize smoothing state — first frame will set the actual peak. */
    ps->hdr_static_peak      = peak_nits;
    ps->hdr_smoothed_peak    = 0.0f;   /* 0 = uninitialized, first frame jumps */
    ps->hdr_prev_frame_peak  = 0.0f;
    ps->dovi_metadata_logged = 0;
    ps->dovi_l1_peak_nits    = 0.0f;   /* per-file: no L1 until a frame carries it */
    ps->dovi_l1_last_log     = 0.0;

    if (is_hdr) {
        float target = ps->gpu_uniforms.hdr_target_nits;
        /* PQ-domain values, matching the shader's fixed EETF (static
         * peak shown; the shader tracks the smoothed peak per frame).
         * Healthy KS is ~0.37-0.85 — a KS near 0 in this line was the
         * field fingerprint of the old linear-domain bug. */
        double maxLum = pq_oetf_cpu(target) / pq_oetf_cpu(peak_nits);
        if (maxLum > 1.0) maxLum = 1.0;
        double ks = 1.5*maxLum - 0.5;
        if (ks < 0.0) ks = 0.0; else if (ks > 0.999) ks = 0.999;
        /* Gamut printed from the UNIFORM, not the container tag: DV P5
         * forces BT.2020 (its output matrix lands in 2020 RGB) even on
         * an untagged stream — the tag-based line said BT.709 while the
         * shader converted 2020→709 (panel-vs-log disagreement caught
         * by Holden's paused screenshot, 2026-09-05). */
        /* m-S2-f: hdr_output_apply has not run yet — in Z/passthrough
         * mode the shader never runs this curve. Say what is ARMED. */
        log_msg("HDR: SDR tone-map curve armed (peak=%.0f nits, target=%.0f nits, gamut=%s%s) — output mode decided by hdr_output_apply",
                peak_nits, target,
                ps->gpu_uniforms.hdr_gamut > 0.5f ? "BT.2020" : "BT.709",
                is_dolby_vision ? ", Dolby Vision" : "");
        log_msg("HDR: BT.2390 EETF PQ-domain (target=%.0f nits, KS=%.3f, maxLum=%.4f)",
                target, ks, maxLum);
        if (ps->gpu_uniforms.out_black_nits >= 0.0f) {
            float bnits = ps->gpu_uniforms.out_black_nits > 0.0f
                        ? ps->gpu_uniforms.out_black_nits
                        : target / 2000.0f;   /* auto only if uniform 0 */
            log_msg("HDR: BT.2390 black lift ACTIVE (display black=%.3f "
                    "nits, %s, minLum=%.5f)",
                    bnits, ps->black_src,
                    pq_oetf_cpu(bnits) / pq_oetf_cpu(peak_nits));
        } else {
            log_msg("HDR: BT.2390 black lift disabled (DSVP_BLACK_NITS=0)");
        }
    }

    static const char *chroma_names[] = {
        "unspecified", "left", "center", "top-left",
        "top", "bottom-left", "bottom"
    };
    const char *cn = (chroma_loc >= 0 && chroma_loc <= 6)
        ? chroma_names[chroma_loc] : "unknown";
    log_msg("GPU: chroma siting=%s (offset %.2f, %.2f texels)",
            cn, ps->gpu_uniforms.chromaOffset[0],
            ps->gpu_uniforms.chromaOffset[1]);
}


/* ═══════════════════════════════════════════════════════════════════
 * Overlay GPU Resources
 * ═══════════════════════════════════════════════════════════════════
 *
 * The overlay system composites debug info, seek bar, subtitles, and
 * other UI elements as a single RGBA texture drawn over the video
 * with alpha blending. The texture is recreated when the window is
 * resized. Upload happens once per frame when overlay_dirty is set.
 */

/* Ensure overlay texture and transfer buffer exist at the given size.
 * Recreates if dimensions changed. Returns 0 on success, -1 on error. */
int gpu_overlay_ensure(PlayerState *ps, int width, int height) {
    if (!ps->gpu_device || width <= 0 || height <= 0) return -1;

    /* Already the right size? */
    if (ps->gpu_overlay_tex &&
        ps->overlay_tex_w == width && ps->overlay_tex_h == height) {
        return 0;
    }

    /* Destroy old resources */
    gpu_overlay_destroy(ps);

    /* Create RGBA8888 texture */
    SDL_GPUTextureCreateInfo tex_info;
    SDL_zero(tex_info);
    tex_info.type                 = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.width                = width;
    tex_info.height               = height;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels           = 1;
    tex_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    ps->gpu_overlay_tex = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_overlay_tex) {
        log_msg("ERROR: Failed to create overlay texture: %s", SDL_GetError());
        return -1;
    }

    /* Create transfer buffer (RGBA = 4 bytes per pixel) */
    SDL_GPUTransferBufferCreateInfo xfer_info;
    SDL_zero(xfer_info);
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xfer_info.size  = (Uint32)width * height * 4;

    ps->gpu_overlay_xfer = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);
    if (!ps->gpu_overlay_xfer) {
        log_msg("ERROR: Failed to create overlay transfer buffer: %s", SDL_GetError());
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_overlay_tex);
        ps->gpu_overlay_tex = NULL;
        return -1;
    }

    ps->overlay_tex_w = width;
    ps->overlay_tex_h = height;
    ps->overlay_dirty = 0;
    ps->overlay_force_full = 1;  /* fresh texture = undefined contents;
                                  * next overlay render must upload the
                                  * full height (overlay.c honors this) */

    log_msg("GPU: overlay texture created (%dx%d RGBA)", width, height);
    return 0;
}

/* Upload RGBA pixel data to the overlay GPU texture.
 * `rgba` must be width×height×4 bytes, tightly packed. */
void gpu_overlay_upload(PlayerState *ps, const uint8_t *rgba,
                        int width, int height, int y0, int y1) {
    if (!ps->gpu_overlay_xfer || !ps->gpu_overlay_tex) return;
    if (width != ps->overlay_tex_w || height != ps->overlay_tex_h) return;

    /* Bound the copy to the changed rows. A full-frame upload moved
     * 2 x ~33MB per 4K frame for a seekbar; the caller already tracks
     * exactly which rows were cleared/drawn. The transfer buffer holds
     * a full frame, so partial copies land at their natural offset and
     * the GPU region matches. (The texture-side copy uses cycle=false —
     * see gpu_overlay_copy_cmd — because rows outside the region must
     * keep their existing content.) */
    if (y0 < 0) y0 = 0;
    if (y1 > height) y1 = height;
    if (y1 <= y0) return;

    /* Union with any pending not-yet-copied window FIRST: map with
     * cycle=true may hand a fresh buffer (no stall on an in-flight
     * copy), so every row the pending GPU region will read must be
     * rewritten from the persistent CPU-side pixel buffer. Rows
     * outside the copy region are never read by the GPU. */
    if (ps->overlay_dirty) {
        if (ps->overlay_up_y0 < y0) y0 = ps->overlay_up_y0;
        if (ps->overlay_up_y1 > y1) y1 = ps->overlay_up_y1;
    }

    uint8_t *dst = SDL_MapGPUTransferBuffer(ps->gpu_device,
                                             ps->gpu_overlay_xfer, true);
    if (!dst) {
        log_msg("ERROR: overlay transfer map failed: %s", SDL_GetError());
        return;
    }
    size_t off = (size_t)y0 * width * 4;
    memcpy(dst + off, rgba + off, (size_t)(y1 - y0) * width * 4);
    SDL_UnmapGPUTransferBuffer(ps->gpu_device, ps->gpu_overlay_xfer);

    ps->overlay_up_y0 = y0;
    ps->overlay_up_y1 = y1;
    ps->overlay_dirty = 1;
}

/* Issue the GPU copy pass to transfer overlay data to the texture.
 * Call this inside an existing command buffer, BEFORE the render pass.
 * Returns the copy pass so the caller can end it, or does it inline. */
void gpu_overlay_copy_cmd(SDL_GPUCommandBuffer *cmd, PlayerState *ps) {
    if (!ps->overlay_dirty || !ps->gpu_overlay_tex) return;

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src_info;
        SDL_GPUTextureRegion dst_region;

        SDL_zero(src_info);
        SDL_zero(dst_region);
        int cy0 = ps->overlay_up_y0;
        int cy1 = ps->overlay_up_y1;
        if (cy0 < 0) cy0 = 0;
        if (cy1 <= cy0 || cy1 > ps->overlay_tex_h) cy1 = ps->overlay_tex_h;
        src_info.transfer_buffer = ps->gpu_overlay_xfer;
        src_info.offset          = (Uint32)((size_t)cy0
                                       * ps->overlay_tex_w * 4);
        src_info.pixels_per_row  = ps->overlay_tex_w;
        src_info.rows_per_layer  = cy1 - cy0;
        dst_region.texture = ps->gpu_overlay_tex;
        dst_region.y = (Uint32)cy0;
        dst_region.w = ps->overlay_tex_w;
        dst_region.h = (Uint32)(cy1 - cy0);
        dst_region.d = 1;
        /* cycle MUST be false for a partial upload: cycling lets SDL
         * hand back a different backing allocation, so every row
         * outside this region would hold undefined recycled content.
         * With a full-frame upload that was invisible; with row-bounded
         * uploads it strobed the elements that weren't re-uploaded on a
         * given tick (seekbar/menubar while the debug panel changed). */
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, false);
    }
    SDL_EndGPUCopyPass(copy);

    ps->overlay_dirty = 0;
}

/* Draw the overlay quad within an existing render pass.
 * Uses the overlay pipeline (alpha blend) and a fullscreen viewport.
 * Call AFTER the video quad has been drawn. */
void gpu_overlay_draw(SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                      PlayerState *ps, Uint32 sc_w, Uint32 sc_h) {
    if (!ps->gpu_overlay_tex || !ps->gpu_pipeline_overlay || !ps->overlay_active)
        return;

    /* OvParams: PQ re-encode flag + encode white (float4-sized).
     * KWin scales the whole PQ surface by (held sdr-brightness)/203 —
     * in passthrough the VIDEO rides that scale by design (the
     * mode-aware hold), but graphics must not: encode overlay white at
     * (displayed target)*(203/held) so subs and OSD DISPLAY at a
     * chosen white regardless of the video's boost. The displayed
     * target is its own dial, DSVP_OVERLAY_NITS (deck 2da890f: 203
     * read too dim against 1000-scaled video, default 300 — a deck
     * eye ruling, x64 eye pending; conditions recorded in the doc). */
    static float s_ov_disp = -1.0f;
    static int   s_ov_logged = 0;
    if (s_ov_disp < 0.0f) {
        s_ov_disp = 300.0f;
        const char *oe = SDL_getenv("DSVP_OVERLAY_NITS");
        if (oe && oe[0]) {
            double ov = SDL_atof(oe);
            if (ov >= 100.0 && ov <= 1000.0) s_ov_disp = (float)ov;
            else log_msg("WARN: DSVP_OVERLAY_NITS='%s' ignored (want 100-1000) — using 300", oe);
        }
    }
    /* Honest instrument: this line used to print on the first overlay
     * draw — the idle screen, in SDR, on a box that cannot do
     * passthrough at all (field 2026-09-06). It describes the PQ
     * re-encode, so it prints the first time that re-encode runs. */
    if (ps->hdr_out_active && !s_ov_logged) {
        s_ov_logged = 1;
        log_msg("overlay: graphics white %.0f nits displayed over passthrough (DSVP_OVERLAY_NITS)", s_ov_disp);
    }
    float ov_white = 203.0f;
    if (ps->hdr_out_active) {
        int held = hdr_sys_held_sdrbr();
        ov_white = held > 0 ? s_ov_disp * 203.0f / (float)held : s_ov_disp;
    }
    float ovp[4] = { ps->hdr_out_active ? 1.0f : 0.0f, ov_white, 0.0f, 0.0f };
    SDL_PushGPUFragmentUniformData(cmd, 0, ovp, sizeof(ovp));

    SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_overlay);

    /* Fullscreen viewport — overlay covers entire window, not just
     * the letterboxed video area. This lets us draw seek bars,
     * debug info, etc. in the black bar regions too. */
    SDL_GPUViewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.w = (float)sc_w;
    viewport.h = (float)sc_h;
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &viewport);

    SDL_GPUTextureSamplerBinding binding = {
        .texture = ps->gpu_overlay_tex,
        .sampler = ps->gpu_sampler_nearest  /* nearest = pixel-perfect bitmap font */
    };
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
}

/* Destroy overlay GPU resources (texture + transfer buffer). */
void gpu_overlay_destroy(PlayerState *ps) {
    if (!ps->gpu_device) return;

    if (ps->gpu_overlay_tex) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_overlay_tex);
        ps->gpu_overlay_tex = NULL;
    }
    if (ps->gpu_overlay_xfer) {
        SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_overlay_xfer);
        ps->gpu_overlay_xfer = NULL;
    }
    ps->overlay_tex_w = 0;
    ps->overlay_tex_h = 0;
    ps->overlay_dirty = 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Packet Queue — thread-safe FIFO for AVPackets
 * ═══════════════════════════════════════════════════════════════════ */

void pq_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCondition();
}

void pq_destroy(PacketQueue *q) {
    pq_flush(q);
    /* NULL the handles: player_open() can fail BEFORE pq_init() runs on
     * a re-open, and player_close() then calls pq_destroy() again on
     * the previous file's already-destroyed (but non-NULL) handles —
     * a use-after-free without this. SDL_DestroyMutex/Condition and
     * SDL_LockMutex are NULL-safe, so double-destroy becomes a no-op. */
    if (q->mutex) { SDL_DestroyMutex(q->mutex);    q->mutex = NULL; }
    if (q->cond)  { SDL_DestroyCondition(q->cond); q->cond  = NULL; }
}

/* Push a packet onto the queue. Caller still owns pkt after this call
 * returns — we move the packet data into a new AVPacket internally. */
int pq_put(PacketQueue *q, AVPacket *pkt) {
    PacketNode *node = av_malloc(sizeof(PacketNode));
    if (!node) return -1;

    node->pkt = av_packet_alloc();
    if (!node->pkt) {
        av_free(node);
        return -1;
    }
    av_packet_move_ref(node->pkt, pkt);
    node->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last) {
        q->first = node;
    } else {
        q->last->next = node;
    }
    q->last = node;
    q->nb_packets++;
    q->size += node->pkt->size;

    SDL_SignalCondition(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

/* Drop head packets older than min_pts (stream time base). Keeps the
 * subtitle queues as rolling windows: every track stays queued so an
 * S-press has the current moment's packets on hand (an empty queue
 * only fills from the demux read position, ~10s ahead of playback —
 * the user-visible "subtitles don't work" delay), while memory stays
 * bounded instead of accumulating for the whole file. Stops at a
 * NOPTS head (can't judge it — the decode-side stale-skip keeps the
 * same policy). */
void pq_prune_stale(PacketQueue *q, int64_t min_pts) {
    SDL_LockMutex(q->mutex);
    while (q->first && q->first->pkt->pts != AV_NOPTS_VALUE
           && q->first->pkt->pts < min_pts) {
        PacketNode *node = q->first;
        q->first = node->next;
        if (!q->first) q->last = NULL;
        q->nb_packets--;
        q->size -= node->pkt->size;
        av_packet_free(&node->pkt);
        av_free(node);
    }
    SDL_UnlockMutex(q->mutex);
}

/* Peek the PTS of the head packet without consuming it. Returns 1 with
 * *pts_out set when a packet is queued, 0 when empty. Used by the
 * subtitle drain to avoid consuming display sets before their time. */
int pq_peek_pts(PacketQueue *q, int64_t *pts_out) {
    SDL_LockMutex(q->mutex);
    int have = (q->first != NULL);
    if (have) *pts_out = q->first->pkt->pts;
    SDL_UnlockMutex(q->mutex);
    return have;
}

/* Pop a packet from the queue. If block=1, waits until data arrives
 * or abort_request is set. Returns 1 on success, 0 if non-blocking
 * and empty, -1 if aborted. */
int pq_get(PacketQueue *q, AVPacket *pkt, int block) {
    int ret = -1;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        PacketNode *node = q->first;
        if (node) {
            q->first = node->next;
            if (!q->first) q->last = NULL;
            q->nb_packets--;
            q->size -= node->pkt->size;

            av_packet_move_ref(pkt, node->pkt);
            av_packet_free(&node->pkt);
            av_free(node);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_WaitCondition(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/* Flush all packets from the queue. Called on seek or close. */
void pq_flush(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    PacketNode *node = q->first;
    while (node) {
        PacketNode *next = node->next;
        av_packet_free(&node->pkt);
        av_free(node);
        node = next;
    }
    q->first = NULL;
    q->last  = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}


/* ═══════════════════════════════════════════════════════════════════
 * Frame Queue — thread-safe FIFO for decoded AVFrames
 * ═══════════════════════════════════════════════════════════════════
 *
 * Sits between the video decode thread (producer) and the main
 * display thread (consumer). Bounded at FRAME_QUEUE_MAX entries.
 * fq_put blocks when full — this is the natural decoder throttle.
 * fq_get is used non-blocking from the display thread; blocking
 * there would stall the render loop.
 */

void fq_init(FrameQueue *q) {
    memset(q, 0, sizeof(FrameQueue));
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCondition();
}

void fq_destroy(FrameQueue *q) {
    fq_flush(q);
    /* NULL the handles — see pq_destroy() for the double-destroy
     * failure mode this prevents. */
    if (q->mutex) { SDL_DestroyMutex(q->mutex);    q->mutex = NULL; }
    if (q->cond)  { SDL_DestroyCondition(q->cond); q->cond  = NULL; }
}

/* Push a decoded frame onto the queue. Takes ownership of `frame`.
 * If block=1, waits until space is available or abort_request fires.
 * Returns:
 *    0 on success (frame is now in the queue)
 *    0 on full+nonblock (caller still owns frame and may retry/drop)
 *   -1 on abort (shutdown — caller should free frame and exit)
 *   -2 if the queue was flushed during our wait — frame is stale,
 *      caller should free it and continue (NOT exit the thread). */
/* expect_serial: the queue serial the caller observed while it still held
 * seek_mutex. Sampling the serial only after acquiring the queue mutex left
 * a window (seek_mutex released → queue mutex acquired) in which the demux
 * thread could complete an ENTIRE seek — flush included — and the stale
 * pre-seek frame then landed at the head of the freshly flushed queue. On a
 * backward seek, main displayed it first and seek-recovery resynced the
 * clocks to the pre-seek position, discarding all audio while video sprinted
 * back. Passing the serial from inside the seek_mutex region closes it. */
int fq_put(FrameQueue *q, AVFrame *frame, int block, int expect_serial) {
    SDL_LockMutex(q->mutex);
    int entry_serial = expect_serial;
    for (;;) {
        if (q->abort_request) {
            SDL_UnlockMutex(q->mutex);
            return -1;
        }
        if (q->flush_serial != entry_serial) {
            /* Queue was flushed (seek) while we were waiting.
             * The frame we hold is from before the flush — stale. */
            SDL_UnlockMutex(q->mutex);
            return -2;
        }
        if (q->nb_frames < FRAME_QUEUE_MAX) {
            FrameNode *node = av_malloc(sizeof(FrameNode));
            if (!node) {
                SDL_UnlockMutex(q->mutex);
                return -1;
            }
            node->frame = frame;
            node->next  = NULL;
            if (!q->last) q->first = node;
            else          q->last->next = node;
            q->last = node;
            q->nb_frames++;
            SDL_SignalCondition(q->cond);
            SDL_UnlockMutex(q->mutex);
            return 0;
        }
        if (!block) {
            SDL_UnlockMutex(q->mutex);
            return 0;  /* full, caller will retry or drop */
        }
        SDL_WaitCondition(q->cond, q->mutex);
    }
}

/* Pop a frame from the queue. Returns 1 with *frame_out set on success,
 * 0 if empty (non-blocking), -1 if aborted. Caller takes ownership of
 * the returned AVFrame and is responsible for av_frame_free. */
int fq_get(FrameQueue *q, AVFrame **frame_out, int block) {
    int ret = -1;
    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }
        FrameNode *node = q->first;
        if (node) {
            q->first = node->next;
            if (!q->first) q->last = NULL;
            q->nb_frames--;
            *frame_out = node->frame;
            av_free(node);
            q->last_pop_serial = q->flush_serial;  /* consumer-side seek
                                                    * race gate (main.c) */
            SDL_SignalCondition(q->cond);  /* wake any blocked fq_put */
            ret = 1;
            break;
        }
        if (!block) {
            ret = 0;
            break;
        }
        SDL_WaitCondition(q->cond, q->mutex);
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/* Flush all frames from the queue. Called on seek and close.
 * Increments flush_serial so any blocked fq_put detects the flush
 * and discards its stale frame instead of pushing it into the
 * freshly-cleared queue. */
void fq_flush(FrameQueue *q) {
    SDL_LockMutex(q->mutex);
    FrameNode *node = q->first;
    while (node) {
        FrameNode *next = node->next;
        if (node->frame) av_frame_free(&node->frame);
        av_free(node);
        node = next;
    }
    q->first = NULL;
    q->last  = NULL;
    q->nb_frames = 0;
    q->flush_serial++;
    SDL_SignalCondition(q->cond);  /* wake any blocked fq_put */
    SDL_UnlockMutex(q->mutex);
}


/* ═══════════════════════════════════════════════════════════════════
 * System-level display HDR — Windows (docs/TODO-X64.md NEXT CYCLE)
 * ═══════════════════════════════════════════════════════════════════
 *
 * The deck's engage/revert model: desktop HDR stays OFF; when an HDR
 * file opens we flip the display into HDR, and we ALWAYS put it back
 * on close/quit/crash. Deck flips KWin via kscreen-doctor; Windows
 * exposes the same per-target switch through DisplayConfig device
 * info: SET_HDR_STATE on Win11 24H2+ (advanced color split HDR from
 * SDR wide-gamut/ACM there), SET_ADVANCED_COLOR_STATE before that.
 * Every failure path falls back to the tone-mapped SDR output, which
 * is today's baseline.
 *
 * Batch B: full arc. Engage display → HDR10/ST2084 swapchain →
 * pipeline recreate (formats are swapchain-bound) → hdr_pass shader
 * path presents PQ as-is. TONE-MAP by default since 2026-08-27
 * (hdr_out_mode=0; Z opts into passthrough, DSVP_HDR_PASS=1 makes
 * passthrough the default — Holden: desktop HDR on typical Windows
 * panels is poor); DSVP_NO_SYS_HDR=1 disables system display control
 * (deck knob). Ordering diverges from deck deliberately: Windows drivers
 * may only advertise ST2084 after display HDR engages, so engage
 * comes first and swapchain support is polled after (the deck's
 * compositor always advertises, so it checks support up front). */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX      /* windows.h min/max macros vs. our identifiers */
#endif
#include <windows.h>
#include <wchar.h>
#include <signal.h>     /* SIGABRT → revert (review DM9) */

/* Win11 24H2 (SDK 26100) DisplayConfig additions, defined locally so
 * any mingw-w64 header vintage compiles (upstream guards them behind
 * NTDDI_WIN11_GA; Kodi ships the same shim). Layouts verified against
 * mingw-w64 master wingdi.h, 2026-08. The ABI is what matters:
 * Windows dispatches on header.type + header.size, and pre-24H2
 * systems answer unknown types with an error — that error IS the
 * version probe. */
#define DSVP_DCDI_GET_ADVANCED_COLOR_INFO_2 ((DISPLAYCONFIG_DEVICE_INFO_TYPE)15)
#define DSVP_DCDI_SET_HDR_STATE             ((DISPLAYCONFIG_DEVICE_INFO_TYPE)16)

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 advancedColorSupported       : 1;
            UINT32 advancedColorActive          : 1;
            UINT32 reserved1                    : 1;
            UINT32 advancedColorLimitedByPolicy : 1;
            UINT32 highDynamicRangeSupported    : 1;
            UINT32 highDynamicRangeUserEnabled  : 1;
            UINT32 wideColorSupported           : 1;
            UINT32 wideColorUserEnabled         : 1;
            UINT32 reserved                     : 24;
        };
        UINT32 value;
    };
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;      /* 0 = SDR, 1 = WCG, 2 = HDR */
} DsvpAdvColorInfo2;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 enableHdr : 1;
            UINT32 reserved  : 31;
        };
        UINT32 value;
    };
} DsvpSetHdrState;

/* All state the revert path needs, file-static so the atexit/crash
 * handlers can restore the display without a PlayerState. */
static struct {
    int           valid;      /* target below is resolved              */
    int           api2;       /* 24H2 SET_HDR_STATE path available     */
    int           supported;  /* display is HDR-capable                */
    int           enabled;    /* display HDR was ACTIVE at probe time  */
    int           user_on;    /* v2: the persistent "Use HDR" toggle   */
    LUID          adapter;    /* DisplayConfig target of the window's  */
    UINT32        target;     /*   monitor                             */
    WCHAR         gdi[32];    /* \\.\DISPLAYn — DEVMODE + path match   */
    volatile LONG engaged;    /* WE turned HDR on — must revert        */
    int           handlers;   /* exit/crash handlers installed         */
    int           have_prior_dm; /* graphics mode captured at engage   */
    DEVMODEW      prior_dm;   /* the PRE-app mode the revert restores  */
    LPTOP_LEVEL_EXCEPTION_FILTER prev_filter; /* chained crash filter  */
} g_winhdr;

/* ── Crash-restore stamp (review DM9 — the Linux contract, ported) ──
 * Written BEFORE the display write (write-ahead: a timed-out or
 * crashed write must leave insurance, review C4), cleared only on a
 * CONFIRMED restore, reconciled at the next launch when the owner pid
 * is dead. Lives under %LOCALAPPDATA% — per-user, not world-writable.
 * Content: adapter LUID (hi lo), target id, api2, owner pid. */
static int winhdr_stamp_path(WCHAR *buf, size_t n) {
    const WCHAR *base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !base[0]) { buf[0] = 0; return 0; }
    _snwprintf(buf, n, L"%s\\dsvp-hdr-restore", base);
    buf[n - 1] = 0;
    return 1;
}

static void winhdr_stamp_write(void) {
    WCHAR path[MAX_PATH];
    if (!winhdr_stamp_path(path, MAX_PATH)) return;
    FILE *f = _wfopen(path, L"w");
    if (!f) { log_msg("HDR sys: stamp NOT written (open failed)"); return; }
    fprintf(f, "%ld %lu %lu %d %lu\n",
            (long)g_winhdr.adapter.HighPart,
            (unsigned long)g_winhdr.adapter.LowPart,
            (unsigned long)g_winhdr.target, g_winhdr.api2,
            (unsigned long)GetCurrentProcessId());
    fclose(f);
    log_msg("HDR sys: stamp armed (target %lu, pid %lu)",
            (unsigned long)g_winhdr.target,
            (unsigned long)GetCurrentProcessId());
}

static void winhdr_stamp_clear(void) {
    WCHAR path[MAX_PATH];
    if (!winhdr_stamp_path(path, MAX_PATH)) return;
    if (_wremove(path) == 0)
        log_msg("HDR sys: stamp cleared (restore confirmed)");
}

/* Is the stamp's owner still running dsvp? Access denied counts as
 * ALIVE (review d-D4-9: never reconcile under a live session). */
static int winhdr_owner_alive(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    if (alive) {
        WCHAR img[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, img, &n)) {
            const WCHAR *b = wcsrchr(img, L'\\');
            b = b ? b + 1 : img;
            alive = (_wcsnicmp(b, L"dsvp", 4) == 0);
        }
    }
    CloseHandle(h);
    return alive;
}

/* Resolve the window's monitor to a DisplayConfig target: GDI name
 * from MONITORINFOEXW, then match it against each active path's
 * source name (Kodi-established pattern). */
static int winhdr_map_target(HWND hwnd) {
    g_winhdr.valid = 0;

    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
        log_msg("HDR sys: GetMonitorInfoW failed");
        return 0;
    }

    UINT32 npath = 0, nmode = 0;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    LONG rc;
    int tries = 0;
    /* Bounded (review d-D3-6): a topology that keeps changing under us
     * (DisplayLink/KVM re-enumeration, TDR recovery) must not spin the
     * main thread — four tries, then tone-mapped output. */
    do {
        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npath, &nmode);
        if (rc != ERROR_SUCCESS) break;
        free(paths); free(modes);
        /* npath/nmode of 0 would make malloc(0) look like OOM */
        paths = malloc((npath ? npath : 1) * sizeof(*paths));
        modes = malloc((nmode ? nmode : 1) * sizeof(*modes));
        if (!paths || !modes) { rc = ERROR_NOT_ENOUGH_MEMORY; break; }
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                                &npath, paths, &nmode, modes, NULL);
    } while (rc == ERROR_INSUFFICIENT_BUFFER && ++tries < 4);
    if (rc == ERROR_INSUFFICIENT_BUFFER)
        log_msg("HDR sys: QueryDisplayConfig unstable after %d tries — "
                "tone-mapped output", tries);

    if (rc != ERROR_SUCCESS) {
        log_msg("HDR sys: QueryDisplayConfig failed (%ld)", rc);
        free(paths); free(modes);
        return 0;
    }

    for (UINT32 i = 0; i < npath; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
        memset(&src, 0, sizeof(src));
        src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size      = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
            wcscmp(src.viewGdiDeviceName, mi.szDevice) == 0) {
            g_winhdr.adapter = paths[i].targetInfo.adapterId;
            g_winhdr.target  = paths[i].targetInfo.id;
            wcsncpy(g_winhdr.gdi, mi.szDevice, 31);
            g_winhdr.gdi[31] = 0;
            g_winhdr.valid   = 1;
            break;
        }
    }
    free(paths); free(modes);
    if (!g_winhdr.valid)
        log_msg("HDR sys: no DisplayConfig path matches the window's monitor");
    return g_winhdr.valid;
}

/* Query HDR capability + current state. Tries the 24H2 v2 info first
 * (HDR and SDR wide-gamut/ACM are separate concepts there); a failure
 * is the version probe and drops us to the legacy struct with the
 * flag decode Kodi established in the field: wideColorEnforced set
 * means an SDR screen running ACM — "advanced color" there is not
 * HDR, and toggling it would not get us one. */
static int winhdr_probe(void) {
    g_winhdr.api2 = g_winhdr.supported = g_winhdr.enabled = 0;
    g_winhdr.user_on = 0;

    DsvpAdvColorInfo2 v2;
    memset(&v2, 0, sizeof(v2));
    v2.header.type      = DSVP_DCDI_GET_ADVANCED_COLOR_INFO_2;
    v2.header.size      = sizeof(v2);
    v2.header.adapterId = g_winhdr.adapter;
    v2.header.id        = g_winhdr.target;
    if (DisplayConfigGetDeviceInfo(&v2.header) == ERROR_SUCCESS) {
        g_winhdr.api2      = 1;
        g_winhdr.supported = v2.highDynamicRangeSupported ? 1 : 0;
        /* Two different facts (review DM6): activeColorMode says what the
         * OS is RUNNING now (the ST2084 signal); highDynamicRangeUserEnabled
         * is the persistent "Use HDR" toggle — the thing SET_HDR_STATE
         * writes and the thing our revert must put back. The prior we
         * restore is the toggle, never the active mode. */
        g_winhdr.enabled   = (v2.activeColorMode == 2);
        g_winhdr.user_on   = v2.highDynamicRangeUserEnabled ? 1 : 0;
        log_msg("HDR sys: advanced-color v2 — hdrSupported=%d hdrUserEnabled=%u "
                "activeMode=%u bits=%u",
                g_winhdr.supported, (unsigned)v2.highDynamicRangeUserEnabled,
                (unsigned)v2.activeColorMode, (unsigned)v2.bitsPerColorChannel);
        return g_winhdr.supported;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO v1;
    memset(&v1, 0, sizeof(v1));
    v1.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    v1.header.size      = sizeof(v1);
    v1.header.adapterId = g_winhdr.adapter;
    v1.header.id        = g_winhdr.target;
    if (DisplayConfigGetDeviceInfo(&v1.header) == ERROR_SUCCESS) {
        g_winhdr.supported = (v1.advancedColorSupported && !v1.wideColorEnforced);
        g_winhdr.enabled   = (g_winhdr.supported && v1.advancedColorEnabled);
        g_winhdr.user_on   = g_winhdr.enabled;  /* v1 has one bit for both */
        log_msg("HDR sys: advanced-color v1 — acSupported=%u acEnabled=%u "
                "wcEnforced=%u forceDisabled=%u",
                (unsigned)v1.advancedColorSupported,
                (unsigned)v1.advancedColorEnabled,
                (unsigned)v1.wideColorEnforced,
                (unsigned)v1.advancedColorForceDisabled);
    } else {
        log_msg("HDR sys: advanced-color query failed");
    }
    return g_winhdr.supported;
}

/* Re-impose a graphics mode only if the current one differs from it
 * (review DM8 / d-D3-9): the pre-app mode is captured ONCE at engage
 * and both directions restore that, never a re-snapshot of the
 * HDR-era mode. Flags 0 = change now, no registry write, not a
 * process-owned temporary mode. */
static void winhdr_restore_mode(const DEVMODEW *want) {
    DEVMODEW cur;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(g_winhdr.gdi, ENUM_CURRENT_SETTINGS, &cur)) {
        log_msg("HDR sys: mode restore skipped — cannot read current mode");
        return;
    }
    if (cur.dmPelsWidth == want->dmPelsWidth &&
        cur.dmPelsHeight == want->dmPelsHeight &&
        cur.dmDisplayFrequency == want->dmDisplayFrequency) {
        log_msg("HDR sys: mode unchanged after flip (%ux%u@%uHz) — no restore",
                (unsigned)cur.dmPelsWidth, (unsigned)cur.dmPelsHeight,
                (unsigned)cur.dmDisplayFrequency);
        return;
    }
    DEVMODEW dm = *want;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT |
                  DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    LONG mrc = ChangeDisplaySettingsExW(g_winhdr.gdi, &dm, NULL, 0, NULL);
    log_msg("HDR sys: mode restore %ux%u@%uHz -> %ux%u@%uHz (%s, %ld)",
            (unsigned)cur.dmPelsWidth, (unsigned)cur.dmPelsHeight,
            (unsigned)cur.dmDisplayFrequency,
            (unsigned)want->dmPelsWidth, (unsigned)want->dmPelsHeight,
            (unsigned)want->dmDisplayFrequency,
            mrc == DISP_CHANGE_SUCCESSFUL ? "ok" : "FAILED", mrc);
}

/* Flip display HDR. The flip can bump the graphics mode (refresh
 * drops on bandwidth-limited links). On ENGAGE the pre-app mode is
 * captured into g_winhdr and put back after the flip; on REVERT the
 * same captured mode is restored — Kodi carries the same restore, but
 * from a fresh snapshot each time, which re-imposed the HDR-era mode
 * after HDR was already off (review DM8). */
static int winhdr_set(int on) {
    if (on) {
        memset(&g_winhdr.prior_dm, 0, sizeof(g_winhdr.prior_dm));
        g_winhdr.prior_dm.dmSize = sizeof(g_winhdr.prior_dm);
        g_winhdr.have_prior_dm = EnumDisplaySettingsW(
            g_winhdr.gdi, ENUM_CURRENT_SETTINGS, &g_winhdr.prior_dm) ? 1 : 0;
        if (g_winhdr.have_prior_dm)
            log_msg("HDR sys: prior graphics mode %ux%u@%uHz captured",
                    (unsigned)g_winhdr.prior_dm.dmPelsWidth,
                    (unsigned)g_winhdr.prior_dm.dmPelsHeight,
                    (unsigned)g_winhdr.prior_dm.dmDisplayFrequency);
    }

    LONG rc;
    if (g_winhdr.api2) {
        DsvpSetHdrState s;
        memset(&s, 0, sizeof(s));
        s.header.type      = DSVP_DCDI_SET_HDR_STATE;
        s.header.size      = sizeof(s);
        s.header.adapterId = g_winhdr.adapter;
        s.header.id        = g_winhdr.target;
        s.enableHdr        = on ? 1 : 0;
        rc = DisplayConfigSetDeviceInfo(&s.header);
    } else {
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE s;
        memset(&s, 0, sizeof(s));
        s.header.type         = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
        s.header.size         = sizeof(s);
        s.header.adapterId    = g_winhdr.adapter;
        s.header.id           = g_winhdr.target;
        s.enableAdvancedColor = on ? 1 : 0;
        rc = DisplayConfigSetDeviceInfo(&s.header);
    }
    log_msg("HDR sys: display HDR %s via %s (rc=%ld)",
            on ? "ENABLED" : "restored to OFF",
            g_winhdr.api2 ? "SET_HDR_STATE" : "SET_ADVANCED_COLOR_STATE", rc);
    if (rc != ERROR_SUCCESS) return 0;

    if (g_winhdr.have_prior_dm && g_winhdr.prior_dm.dmDisplayFrequency != 0)
        winhdr_restore_mode(&g_winhdr.prior_dm);
    return 1;
}

/* Diagnostic (and batch-C input for subtitle nits): where the OS maps
 * SDR white while HDR is on — explains "everything looks dim" reports.
 * Units: 1000 = 80 nits (Windows SDR-content-brightness slider). Our
 * PQ passthrough is NOT scaled by this — only SDR-in-HDR content is. */
static void winhdr_log_sdr_white(void) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL wl;
    memset(&wl, 0, sizeof(wl));
    wl.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    wl.header.size      = sizeof(wl);
    wl.header.adapterId = g_winhdr.adapter;
    wl.header.id        = g_winhdr.target;
    if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS)
        log_msg("HDR sys: OS SDR white level = %.0f nits",
                wl.SDRWhiteLevel * 80.0 / 1000.0);
}

/* The one revert everything funnels through. Interlocked so the
 * normal path, atexit, and the crash filter can race safely — the
 * display is restored exactly once. */
static void winhdr_revert(void) {
    if (InterlockedExchange(&g_winhdr.engaged, 0)) {
        if (winhdr_set(0)) {
            winhdr_stamp_clear();
            winhdr_probe();   /* receipt: the post-revert user/active state */
        } else {
            log_msg("HDR sys: restore NOT confirmed — stamp kept, next "
                    "launch reconciles");
        }
    }
}

static void winhdr_atexit(void) { winhdr_revert(); }

/* Termination paths atexit + the exception filter do not cover
 * (review DM9): console Ctrl+C / window close → ExitProcess, and
 * abort() → raise(SIGABRT) → _exit. Both revert first, then let the
 * default action proceed. */
static BOOL WINAPI winhdr_console_handler(DWORD type) {
    log_msg("HDR sys: console event %lu — reverting display", (unsigned long)type);
    winhdr_revert();
    return FALSE;   /* default handler terminates the process */
}

static void winhdr_abort_handler(int sig) {
    winhdr_revert();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Crash while we hold the display in HDR: put the desktop back, then
 * let WER/the debugger see the exception. log_msg is lock-free
 * (plain FILE*), so no deadlock hazard from crash context. Covers
 * faults; TerminateProcess/power loss cannot be covered — the
 * desktop's own HDR toggle is the manual recovery. */
static LONG WINAPI winhdr_crash_filter(EXCEPTION_POINTERS *ep) {
    winhdr_revert();
    /* Chain (review d-D3-5): whoever was installed before us (SDL, a
     * crash reporter) still gets the exception. */
    if (g_winhdr.prev_filter && g_winhdr.prev_filter != winhdr_crash_filter)
        return g_winhdr.prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void winhdr_install_handlers(void) {
    if (g_winhdr.handlers) return;
    g_winhdr.handlers = 1;
    atexit(winhdr_atexit);
    g_winhdr.prev_filter = SetUnhandledExceptionFilter(winhdr_crash_filter);
    SetConsoleCtrlHandler(winhdr_console_handler, TRUE);
    signal(SIGABRT, winhdr_abort_handler);
    log_msg("HDR sys: crash filter installed (prev=%p); console + abort "
            "handlers installed", (void *)g_winhdr.prev_filter);
}

/* Platform backend: put the window's display into HDR (or confirm
 * it already is). Returns 1 iff the display is in HDR when we
 * return. Never touches a display the user already runs in HDR
 * (engaged stays 0, nothing reverts). */
static int hdr_sys_engage(PlayerState *ps) {
    if (g_winhdr.engaged) return 1;

    HWND hwnd = (HWND)SDL_GetPointerProperty(
                    SDL_GetWindowProperties(ps->window),
                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd || !winhdr_map_target(hwnd) || !winhdr_probe()) {
        log_msg("HDR sys: display not HDR-capable — tone-mapped output");
        return 0;
    }
    if (g_winhdr.enabled) {
        /* The user runs this display in HDR: nothing to write, nothing
         * to revert. */
        winhdr_log_sdr_white();
        return 1;
    }
    if (g_winhdr.user_on) {
        /* "Use HDR" is ON but the OS is not running it right now
         * (battery optimisation, link retrain, display asleep). Writing
         * SET(1) would be a no-op and the later revert SET(0) would turn
         * the user's toggle OFF for good (review DM6). Touch nothing; the
         * swapchain poll decides whether ST2084 is available. */
        log_msg("HDR sys: HDR user-enabled but inactive — not writing; "
                "swapchain poll decides");
        return 1;
    }
    winhdr_install_handlers();
    /* Arm the revert BEFORE the write (review DM7): the flip plus the
     * synchronous mode restore can take seconds, and a fault inside
     * that window must still put the desktop back. Cleared only if the
     * write itself was refused — then nothing changed. */
    InterlockedExchange(&g_winhdr.engaged, 1);
    log_msg("HDR sys: engaging — revert armed (prior: user=%d active=%d)",
            g_winhdr.user_on, g_winhdr.enabled);
    winhdr_stamp_write();   /* write-ahead: insurance exists before the write */
    if (!winhdr_set(1)) {
        InterlockedExchange(&g_winhdr.engaged, 0);
        winhdr_stamp_clear();   /* nothing changed — nothing to insure */
        return 0;
    }
    winhdr_log_sdr_white();
    return 1;
}

/* Platform backend: restore the display iff we changed it. */
static void hdr_sys_revert_all(void) {
    winhdr_revert();
}

/* KWin-specific concept (reference-luminance hold); Windows scales
 * nothing behind our PQ surface. No-op keeps the shared apply layer
 * platform-blind. */
static void hdr_sys_apply_sdrbr(int pass_active) { (void)pass_active; }
int hdr_sys_held_sdrbr(void) { return 0; }

/* Windows crash-restore (review DM9): the OS HDR toggle PERSISTS in
 * Windows' own settings, so a session that died holding it (fail-fast,
 * TerminateProcess, power loss) leaves the user's desktop in HDR with
 * "Use HDR" flipped on behind their back. Runs before any probe. */
void hdr_sys_reconcile_stamp(void) {
    WCHAR path[MAX_PATH];
    if (!winhdr_stamp_path(path, MAX_PATH)) return;
    FILE *f = _wfopen(path, L"r");
    if (!f) return;   /* no stamp: the common case */
    long hi = 0; unsigned long lo = 0, target = 0, pid = 0; int api2 = 0;
    int n = fscanf(f, "%ld %lu %lu %d %lu", &hi, &lo, &target, &api2, &pid);
    fclose(f);
    if (n < 5) {
        log_msg("HDR sys: stamp unparsable (n=%d) — kept, manual check "
                "of Settings > Display > Use HDR advised", n);
        return;
    }
    if (winhdr_owner_alive((DWORD)pid)) {
        log_msg("HDR sys: stamp belongs to live dsvp pid %lu — not reconciling", pid);
        return;
    }
    /* The dead session only ever engaged when the user toggle was OFF
     * (engage refuses otherwise), so the baseline is OFF. Probe first:
     * if the user already put it back, just drop the stamp. */
    g_winhdr.adapter.HighPart = hi;
    g_winhdr.adapter.LowPart  = lo;
    g_winhdr.target           = (UINT32)target;
    g_winhdr.api2             = api2;
    g_winhdr.gdi[0]           = 0;      /* no DEVMODE restore without a name */
    g_winhdr.have_prior_dm    = 0;
    winhdr_probe();
    if (!g_winhdr.user_on && !g_winhdr.enabled) {
        log_msg("HDR sys: stamp from dead pid %lu — display already back "
                "to baseline, stamp dropped", pid);
        winhdr_stamp_clear();
        return;
    }
    log_msg("HDR sys: previous session (pid %lu) died holding the display "
            "— restoring baseline", pid);
    if (winhdr_set(0)) {
        log_msg("HDR sys: reconciled stranded display HDR from dead session (pid %lu)", pid);
        winhdr_stamp_clear();
    } else {
        log_msg("HDR sys: reconcile write FAILED — stamp kept");
    }
}

#else /* !_WIN32 — KDE/kscreen-doctor backend (deck lineage) */

#include <unistd.h>
#include <signal.h>
#include <errno.h>      /* EINTR — bounded external-command waits */
#include <poll.h>       /* bounded kscreen-doctor output reads */
#include <fcntl.h>      /* O_WRONLY */
#include <spawn.h>      /* posix_spawnp — no shell in the display path */
#include <sys/wait.h>   /* waitpid */

/* ── Bounded external-command execution (deck 2f00c37 port, review
 * HDR M3+M4) ── Every kscreen-doctor call is a wedge risk: popen/
 * system block forever if kscreen-doctor hangs (no KScreen service
 * answering — the deck's field wedge mechanism). 5s covers the
 * observed ~2s flip; on expiry the child is SIGKILLed and the caller
 * treats it as failure (fail-open to the tone-mapped baseline).
 * EINTR retried throughout. argv exec — NO SHELL remains in this
 * path; the output-name whitelist is defense-in-depth now. */
#define HDR_CMD_TIMEOUT_SEC 5.0
/* One engage sequence (detect + set + retry + learn + learn-set) is up
 * to five bounded calls; each used to carry TWO independent 5 s
 * deadlines (read, then wait) — ~10 s per call, ~50 s per open on the
 * main thread in the worst case (review DM13). Now: one deadline per
 * call shared by read and wait, and one budget per SEQUENCE that every
 * call inside it is clipped to. The budget is the request's "Z stalls
 * playback" design item stated as a number; the async/pre-probe
 * redesign stays open. */
#define HDR_SEQ_BUDGET_SEC  8.0
static double s_hdr_budget_deadline = 0.0;   /* 0 = no sequence budget armed */
static int    s_hdr_budget_hits     = 0;

static double hdr_call_deadline(void) {
    double d = get_time_sec() + HDR_CMD_TIMEOUT_SEC;
    if (s_hdr_budget_deadline > 0.0 && s_hdr_budget_deadline < d)
        d = s_hdr_budget_deadline;
    return d;
}

/* The kill after a timeout must not wait forever either (review
 * d-D4-8, same class as the dialog's C3): WNOHANG for ~500 ms, then
 * the zombie is left to init. EINTR-correct. */
static void hdr_reap_after_kill(pid_t pid) {
    for (int i = 0; i < 50; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return;
        SDL_Delay(10);
    }
    log_msg("HDR sys: pid %d did not exit after SIGKILL — left to init", (int)pid);
}

static int hdr_sys_wait_bounded_until(pid_t pid, double deadline) {
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0 && errno != EINTR) return -1;
        if (get_time_sec() >= deadline) {
            kill(pid, SIGKILL);
            hdr_reap_after_kill(pid);
            log_msg("HDR sys: kscreen-doctor TIMED OUT (%.1fs budget) — "
                    "killed; the display call may not have landed",
                    HDR_CMD_TIMEOUT_SEC);
            if (s_hdr_budget_deadline > 0.0) s_hdr_budget_hits++;
            return -2;
        }
        SDL_Delay(5);
    }
}
static int hdr_sys_wait_bounded(pid_t pid) {
    return hdr_sys_wait_bounded_until(pid, hdr_call_deadline());
}

static pid_t hdr_spawn_capture(char *const argv[], int *out_fd) {
    int pfds[2];
    if (pipe(pfds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO,
                                     "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, pfds[0]);
    extern char **environ;
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfds[1]);
    if (rc != 0) { close(pfds[0]); return -1; }
    *out_fd = pfds[0];
    return pid;
}

/* Read until EOF or the deadline. On a timeout the child is killed and
 * *timed_out is set — the caller must report a TIMEOUT, not parse the
 * partial output as complete (review d-D4-5: the old read-timeout was
 * indistinguishable from a normal exit, so detect's timeout guard was
 * dead on the dominant hang and a wedged KScreen read as "no
 * HDR-capable output"). When the buffer fills, the rest is drained and
 * discarded so a verbose child never blocks on the pipe (d-D4-7). */
static size_t hdr_read_bounded_until(int fd, pid_t pid, char *buf, size_t cap,
                                     double deadline, int *timed_out) {
    size_t got = 0;
    char scratch[512];
    if (timed_out) *timed_out = 0;
    for (;;) {
        double left = deadline - get_time_sec();
        if (left <= 0.0) { kill(pid, SIGKILL); if (timed_out) *timed_out = 1; break; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, (int)(left * 1000.0) + 1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { kill(pid, SIGKILL); if (timed_out) *timed_out = 1; break; }
        size_t room = cap - 1 - got;
        char *dst = room > 0 ? buf + got : scratch;
        size_t want = room > 0 ? room : sizeof(scratch);
        ssize_t n = read(fd, dst, want);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        if (room > 0) got += (size_t)n;
    }
    buf[got] = '\0';
    return got;
}
static size_t hdr_read_bounded(int fd, pid_t pid, char *buf, size_t cap) {
    return hdr_read_bounded_until(fd, pid, buf, cap, hdr_call_deadline(), NULL);
}

/* Run kscreen-doctor with up to three property args, synchronously,
 * bounded, output discarded. Returns the exit code (-1 abnormal,
 * -2 timeout). Replaces system() — same synchronous ordering
 * guarantee, no shell, no unbounded hang. */
static int hdr_run_kscreen(const char *a1, const char *a2, const char *a3) {
    char *argv[5];
    int n = 0;
    argv[n++] = "kscreen-doctor";
    argv[n++] = (char *)a1;
    if (a2 && a2[0]) argv[n++] = (char *)a2;
    if (a3 && a3[0]) argv[n++] = (char *)a3;
    argv[n] = NULL;
    int fd = -1;
    pid_t pid = hdr_spawn_capture(argv, &fd);
    if (pid < 0) return -1;
    double deadline = hdr_call_deadline();     /* ONE deadline for read + wait */
    char drain[512];
    int timed_out = 0;
    hdr_read_bounded_until(fd, pid, drain, sizeof(drain), deadline, &timed_out);
    close(fd);
    int rc = hdr_sys_wait_bounded_until(pid, deadline);
    return timed_out ? -2 : rc;
}

/* x64-Linux system HDR rides KWin's per-output switch via
 * kscreen-doctor, exactly like the deck (field-proven there;
 * dellbian is the same Plasma 6 stack). KWin ACCEPTS a PQ surface
 * without ever switching the DISPLAY into HDR — it tone-maps to SDR
 * itself — so the engage-first ordering applies here too. Non-KDE
 * desktops: the probe finds nothing and we stay tone-mapped. */

static struct {
    char output[32];        /* HDR-capable output name (kscreen)     */
    int  prior_hdr;         /* output HDR state before we touched it */
    int  prior_wcg;         /* same for wide-colour-gamut: 1 on,
                               0 off, -1 unknown/unreported. We write
                               this property, so we must read it —
                               -1 means never touch it                */
    int  prior_sdrbr;       /* KWin "SDR brightness" (reference
                               luminance, nits) before we touched it;
                               -1 unknown/unreported = never touch.
                               KWin scales every HDR frame by
                               value/203 (deck field 2026-08-25), so
                               engage holds it at the reference and
                               revert restores the desktop value      */
    volatile int engaged;   /* WE flipped it on — must revert        */
    int  handlers;          /* atexit registered                     */
    int  held_sdrbr;        /* sdr-brightness we last WROTE (mode-
                               aware hold, deck caaded5/a56b870 port,
                               hold LIFTED by Holden 2026-09-06):
                               passthrough wants ~1000, SDR/tone-map
                               203; 0 = nothing written, desktop
                               value stands                          */
} g_linhdr;

/* ── Crash-restore stamp (deck 5c8554b lineage; ported 2026-08-26 at
 * Holden's call) ──
 * kscreen persists HDR/WCG/SDR-brightness across sessions and reboots;
 * a SIGKILL, SEGV, or driver death while we hold the display strands
 * the desktop there — atexit covers only clean exits. So: a stamp
 * written when we flip the display, cleared when we restore, and
 * reconciled synchronously at the next launch before any probe.
 * No HOME → no stamp: the old deck fallback of a fixed world-writable
 * /tmp path was a Knot audit finding (pre-seedable — its contents
 * reach a popen'd command line — and symlinkable). */
static int hdr_stamp_path(char *buf, size_t n) {
    const char *h = getenv("HOME");
    if (!h || !h[0]) { buf[0] = '\0'; return 0; }
    snprintf(buf, n, "%s/.dsvp-hdr-restore", h);
    return 1;
}

static void hdr_stamp_update(int held) {
    char path[512];
    if (!hdr_stamp_path(path, sizeof(path))) return;
    if (!held) {
        /* Restore confirmed — drop OUR insurance only (deck c8ba0ad
         * port, review HDR M7): with two instances the second writer
         * owns the stamp, and instance A's confirmed restore must not
         * delete instance B's crash protection. The read side
         * (reconcile) already pid-checks; this is the unlink side's
         * mirror. An unparsable stamp is nobody's — unlink it. */
        FILE *sf = fopen(path, "r");
        if (!sf) return;
        char so[64] = "";
        int sh = 0, sw = -1, spd = 0, ss = -1;
        int sn = fscanf(sf, "%63s %d %d %d %d", so, &sh, &sw, &spd, &ss);
        fclose(sf);
        if (sn < 4 || spd == (int)getpid())
            unlink(path);
        else
            log_msg("HDR sys: stamp now owned by pid %d — left in place",
                    spd);
        return;
    }
    /* Atomic (review DM11): tmp + fsync + rename. A truncating fopen
     * left a 0-byte stamp on a power loss between open and write, and
     * the read side then unlinked the empty file as "nobody's". */
    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { log_msg("HDR sys: stamp NOT written (open failed)"); return; }
    fprintf(f, "%s %d %d %d %d\n", g_linhdr.output,
            g_linhdr.prior_hdr, g_linhdr.prior_wcg,
            (int)getpid(), g_linhdr.prior_sdrbr);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) {
        log_msg("HDR sys: stamp rename failed (errno %d)", errno);
        unlink(tmp);
        return;
    }
    log_msg("HDR sys: stamp armed for %s hdr=%d wcg=%d sdr=%d (pid %d)",
            g_linhdr.output, g_linhdr.prior_hdr, g_linhdr.prior_wcg,
            g_linhdr.prior_sdrbr, (int)getpid());
}

/* Is a stamp on disk owned by a DIFFERENT live dsvp? Then that
 * instance holds the display and this one must ride its state, never
 * own it (review DM12: a second instance used to overwrite the stamp
 * with the first's ENGAGED state as the baseline). */
static int hdr_stamp_owner_alive(int pid);
static int hdr_stamp_foreign_live(int *owner_pid) {
    char path[512];
    if (!hdr_stamp_path(path, sizeof(path))) return 0;
    FILE *sf = fopen(path, "r");
    if (!sf) return 0;
    char so[64] = ""; int sh = 0, sw = -1, spd = 0, ss = -1;
    int sn = fscanf(sf, "%63s %d %d %d %d", so, &sh, &sw, &spd, &ss);
    fclose(sf);
    if (sn < 4 || spd == (int)getpid()) return 0;
    if (!hdr_stamp_owner_alive(spd)) return 0;
    if (owner_pid) *owner_pid = spd;
    return 1;
}

/* True when the pid recorded in a stamp belongs to a LIVE dsvp
 * process. Without this, a second concurrent instance reads the
 * first's stamp as "previous session died", restores the display
 * baseline under its feet mid-playback, and unlinks its crash
 * protection. The comm check keeps a recycled pid from counting. */
static int hdr_stamp_owner_alive(int pid) {
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) != 0) {
        /* EPERM: it exists but is not ours to signal — ALIVE (review
         * d-D4-9). Never reconcile under a process we cannot see into. */
        return errno == EPERM;
    }
    /* Compare with OUR OWN comm, not the literal "dsvp": a renamed
     * binary (dsvp-git, a wrapper) used to read as dead, and reconcile
     * restored the display under a live session (d-D4-9). */
    char cpath[64], comm[32] = "", self[32] = "";
    snprintf(cpath, sizeof(cpath), "/proc/%d/comm", pid);
    FILE *cf = fopen(cpath, "r");
    if (!cf) return 1;   /* cannot read it — treat as alive, the safe side */
    if (!fgets(comm, sizeof(comm), cf)) comm[0] = '\0';
    fclose(cf);
    comm[strcspn(comm, "\n")] = '\0';
    FILE *mf = fopen("/proc/self/comm", "r");
    if (mf) { if (!fgets(self, sizeof(self), mf)) self[0] = '\0'; fclose(mf); }
    self[strcspn(self, "\n")] = '\0';
    if (!self[0]) snprintf(self, sizeof(self), "dsvp");
    return strcmp(comm, self) == 0;
}

void hdr_sys_reconcile_stamp(void) {
    char path[512];
    if (!hdr_stamp_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;   /* no stamp — the common case, guards stay silent */
    /* Session guard (deck fd06dec port, review HDR C1): DSVP_NO_SYS_HDR
     * promises zero display ops — DEFER with the stamp preserved; it
     * is the only record of the stranded desktop state, repairable
     * only by an unguarded launch. (The deck carries a second guard
     * for its Game Mode compositor; deliberately NOT here — x64
     * targets Debian and Windows desktops only, Holden 2026-09-05.) */
    if (SDL_getenv("DSVP_NO_SYS_HDR")) {
        fclose(f);
        log_msg("HDR sys: DSVP_NO_SYS_HDR — stamp reconcile deferred "
                "(stamp preserved)");
        return;
    }
    char out[64] = "";
    int phdr = 0, pwcg = -1, spid = 0, psdr = -1;
    int n = fscanf(f, "%63s %d %d %d %d", out, &phdr, &pwcg, &spid, &psdr);
    fclose(f);
    /* Whitelist on the READ side too: out reaches a shell command
     * line below, and write-side sanitisation does not protect a
     * stamp file we did not write. */
    for (const char *c = out; *c; c++) {
        if (!((*c>='A'&&*c<='Z')||(*c>='a'&&*c<='z')||
              (*c>='0'&&*c<='9')||*c=='-')) {
            log_msg("HDR sys: stamp output name rejected — not restoring");
            unlink(path);
            return;
        }
    }
    if (n < 3 || !out[0]) {
        /* Present but unparsable (torn write, foreign content): KEEP
         * it and say so — deleting the only record of a stranded
         * desktop is the one thing reconcile must never do (DM11). */
        log_msg("HDR sys: stamp unparsable (n=%d) — kept; check Display "
                "settings by hand if the desktop looks stuck in HDR", n);
        return;
    }
    if (n >= 4 && hdr_stamp_owner_alive(spid)) {
        /* The session that wrote this is still running and still owns
         * the display — leave both the display and the stamp alone. */
        log_msg("HDR sys: stamp belongs to live dsvp pid %d — "
                "not reconciling", spid);
        return;
    }
    if (n >= 3 && out[0]) {
        /* Mirror hdr_sys_set's restore semantics: hdr back to the
         * recorded baseline; wcg only if the dead session had turned
         * it on itself (prior_wcg == 0); sdr-brightness back to the
         * recorded desktop value. Synchronous — the session that
         * follows must probe the TRUE baseline. */
        /* argv exec + bounded (deck 2f00c37 port) — and the stamp
         * survives an UNCONFIRMED restore (deck b91f15e / review M2):
         * the old code unlinked unconditionally, deleting the crash
         * insurance on exactly the restore-failed path. */
        char hdr_part[64], wcg_part[64] = "", sdr_part[64] = "";
        snprintf(hdr_part, sizeof(hdr_part), "output.%s.hdr.%s",
                 out, phdr ? "enable" : "disable");
        if (pwcg == 0)
            snprintf(wcg_part, sizeof(wcg_part),
                     "output.%s.wcg.disable", out);
        if (n == 5 && psdr > 0)
            snprintf(sdr_part, sizeof(sdr_part),
                     "output.%s.sdr-brightness.%d", out, psdr);
        log_msg("HDR sys: previous session died holding the display — "
                "restoring baseline on %s (stamp file)", out);
        int rc = hdr_run_kscreen(hdr_part, wcg_part, sdr_part);
        if (rc != 0 && (wcg_part[0] || sdr_part[0])) {
            /* kscreen-doctor parses ALL its arguments before applying
             * any: one rejected property (a version that lacks
             * sdr-brightness) blocked the HDR restore forever (review
             * d-D4-11). Retry the HDR part alone. */
            log_msg("HDR sys: combined restore rc=%d — retrying hdr part alone", rc);
            rc = hdr_run_kscreen(hdr_part, "", "");
        }
        if (rc != 0) {
            log_msg("HDR sys: stamp restore NOT confirmed (rc=%d) — "
                    "stamp kept, will retry next launch", rc);
            return;
        }
        log_msg("HDR sys: reconciled stranded display state on %s", out);
    }
    unlink(path);
}

/* kscreen-doctor colours its output with ANSI escapes even into a
 * pipe — the deck's first field-test parse failed on exactly that.
 * Strip CSI sequences in place before any matching. */
static void strip_ansi(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r == 0x1b && r[1] == '[') {
            r += 2;
            while (*r && !(*r >= '@' && *r <= '~')) r++;
            if (*r) r++;   /* consume the final byte */
        } else *w++ = *r++;
    }
    *w = '\0';
}

/* Read a kscreen-doctor property line's value.
 * 1 = enabled, 0 = disabled, -1 = incapable or unrecognised.
 * ("disabled" contains no "enabled" substring, so the order is safe.) */
static int hdr_prop_state(const char *s) {
    if (strstr(s, "incapable")) return -1;
    if (strstr(s, "enabled"))   return  1;
    if (strstr(s, "disabled"))  return  0;
    return -1;
}

/* Is this the wide-colour-gamut property line? The label has been
 * spelled several ways across Plasma releases, so match the key part
 * case-insensitively rather than pinning one string — an unmatched
 * label leaves WCG at "unknown", which is the safe state (we then
 * never write it). */
static int hdr_line_is_wcg(const char *s) {
    char key[64];
    size_t i = 0;
    for (; i < sizeof(key) - 1 && s[i] && s[i] != ':'; i++)
        key[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    key[i] = '\0';
    return strstr(key, "wide color gamut") != NULL
        || strstr(key, "wide colour gamut") != NULL
        || strstr(key, "wcg") != NULL;
}

/* Adopt this output if it is the one we can drive. Called at each
 * block boundary because the properties we need can appear in any
 * order within an output's block. */
static int hdr_sys_adopt(const char *name, int enabled, int hdr, int wcg,
                         int sdrbr) {
    if (!enabled || !name[0] || hdr < 0) return 0;
    /* Sanitize before this ever reaches a shell */
    for (const char *c = name; *c; c++)
        if (!((*c>='A'&&*c<='Z')||(*c>='a'&&*c<='z')||
              (*c>='0'&&*c<='9')||*c=='-')) return 0;
    snprintf(g_linhdr.output, sizeof(g_linhdr.output), "%s", name);
    g_linhdr.prior_hdr = hdr;
    g_linhdr.prior_wcg = wcg;
    g_linhdr.prior_sdrbr = sdrbr;
    return 1;
}

/* Find the connected, enabled output whose HDR capability is real
 * ("HDR: enabled|disabled" — SDR panels say "incapable"). Records
 * the output name and the CURRENT state of every property we are
 * going to write, so revert restores rather than assumes.
 *
 * WCG is read here because hdr_sys_set writes it. It used to be
 * written but never read: revert issued wcg.disable unconditionally,
 * on the assumption that WCG tracked HDR. On a display where the user
 * had wide gamut on with HDR off, that silently turned it off and
 * LEFT it off — kscreen persists output properties, so the change
 * outlived the process, the session, and reboots. Root-caused on the
 * deck 2026-08-17 (a week of chasing it as a TV fault, because with
 * WCG off KWin's SDR fullscreen path froze on that display); this is
 * the same defect, ported here with the rest of the backend before it
 * ever shipped. Anything we write, we read first.
 *
 * Returns 1 if found. (Deck lineage; the deck's copy of this parser is
 * host-unit-tested against coloured, plain, WCG-absent, prior-enabled
 * and incapable-only fixtures from real kscreen-doctor output.) */
/* Set by hdr_sys_detect when kscreen-doctor ANSWERED and no enabled
 * output was HDR-capable (as opposed to the probe failing or timing
 * out). Only that answer is worth remembering — see hdr_sys_engage. */
static int s_detect_none = 0;

static int hdr_sys_detect(const char *prefer) {
    s_detect_none = 0;
    g_linhdr.output[0] = '\0';
    g_linhdr.prior_hdr = 0;
    g_linhdr.prior_wcg = -1;
    g_linhdr.prior_sdrbr = -1;
    /* prefer = the connector the WINDOW is on (SDL's Wayland display
     * name is the wl_output name, "DP-1"); the first HDR-capable
     * output is only the fallback (review d-D4-12 / deck minor 4: with
     * two HDR monitors the desk monitor was flipped and held while the
     * TV got KWin's tone-map). */
    char first_out[32] = ""; int first_hdr = 0, first_wcg = -1, first_sdrbr = -1;
    int preferred_found = 0;

    /* Bounded probe (deck 2f00c37 port): the plain popen/fgets/pclose
     * had no ceiling — a hung kscreen-doctor wedged the session. */
    static char outbuf[65536];
    int out_fd = -1;
    char *probe_argv[] = { "kscreen-doctor", "-o", NULL };
    pid_t probe_pid = hdr_spawn_capture(probe_argv, &out_fd);
    if (probe_pid < 0) {
        log_msg("HDR sys: spawn(kscreen-doctor) failed");
        return 0;
    }
    double pdl = hdr_call_deadline();
    int ptimeout = 0;
    hdr_read_bounded_until(out_fd, probe_pid, outbuf, sizeof(outbuf), pdl, &ptimeout);
    close(out_fd);
    int probe_rc = hdr_sys_wait_bounded_until(probe_pid, pdl);
    if (ptimeout || probe_rc != 0) {
        log_msg("HDR sys: probe %s (rc=%d) — display control skipped "
                "(tone-mapped fallback)", ptimeout ? "timed out" : "failed",
                probe_rc);
        return 0;
    }

    char cur_name[32] = "";
    int  cur_enabled = 0, cur_hdr = -1, cur_wcg = -1, cur_sdrbr = -1;
    int  lines_seen = 0, outputs_seen = 0, found = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(outbuf, "\n", &saveptr); line;
         line = strtok_r(NULL, "\n", &saveptr)) {
        lines_seen++;
        strip_ansi(line);
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        if (strncmp(s, "Output:", 7) == 0) {
            /* Block boundary: decide on the block that just ended */
            if (!preferred_found &&
                hdr_sys_adopt(cur_name, cur_enabled, cur_hdr, cur_wcg, cur_sdrbr)) {
                if (prefer && prefer[0] && strcmp(cur_name, prefer) == 0) {
                    preferred_found = 1; found = 1;
                } else if (!first_out[0]) {
                    snprintf(first_out, sizeof(first_out), "%s", g_linhdr.output);
                    first_hdr = g_linhdr.prior_hdr; first_wcg = g_linhdr.prior_wcg;
                    first_sdrbr = g_linhdr.prior_sdrbr; found = 1;
                }
            }
            /* "Output: 2 DP-1 <uuid>" — name is the third token */
            outputs_seen++;
            cur_name[0] = '\0';
            cur_enabled = 0;
            cur_hdr = -1;
            cur_wcg = -1;
            cur_sdrbr = -1;
            char idx[16];
            if (sscanf(s, "Output: %15s %31s", idx, cur_name) != 2)
                cur_name[0] = '\0';
        } else if (strncmp(s, "enabled", 7) == 0 &&
                   (s[7]=='\n' || s[7]=='\0' || s[7]=='\r' || s[7]==' ')) {
            cur_enabled = 1;
        } else if (strncmp(s, "HDR:", 4) == 0) {
            cur_hdr = hdr_prop_state(s);
        } else if (hdr_line_is_wcg(s)) {
            cur_wcg = hdr_prop_state(s);
        } else if (strncmp(s, "SDR brightness:", 15) == 0) {
            /* "SDR brightness: 800 nits" — KWin's reference luminance,
             * an HDR sub-property (only reported while that output's
             * HDR is enabled; absent leaves -1 = never touch). */
            int v = -1;
            if (sscanf(s + 15, " %d", &v) == 1 && v > 0) cur_sdrbr = v;
        }
    }
    if (!preferred_found &&
        hdr_sys_adopt(cur_name, cur_enabled, cur_hdr, cur_wcg, cur_sdrbr)) {
        if (prefer && prefer[0] && strcmp(cur_name, prefer) == 0) {
            preferred_found = 1; found = 1;
        } else if (!first_out[0]) {
            snprintf(first_out, sizeof(first_out), "%s", g_linhdr.output);
            first_hdr = g_linhdr.prior_hdr; first_wcg = g_linhdr.prior_wcg;
            first_sdrbr = g_linhdr.prior_sdrbr; found = 1;
        }
    }
    if (found && !preferred_found) {
        /* Fall back to the first capable output, restoring its fields
         * (adopt may have been called for later, non-preferred blocks). */
        snprintf(g_linhdr.output, sizeof(g_linhdr.output), "%s", first_out);
        g_linhdr.prior_hdr = first_hdr; g_linhdr.prior_wcg = first_wcg;
        g_linhdr.prior_sdrbr = first_sdrbr;
        if (prefer && prefer[0])
            log_msg("HDR sys: window display '%s' is not an HDR-capable enabled "
                    "output — using the first capable one (%s)", prefer, first_out);
    }

    if (found) {
        char sb[24] = "unknown";
        if (g_linhdr.prior_sdrbr > 0)
            snprintf(sb, sizeof(sb), "%d nits", g_linhdr.prior_sdrbr);
        log_msg("HDR sys: output %s is HDR-capable — baseline hdr=%s wcg=%s "
                "sdr-brightness=%s",
                g_linhdr.output,
                g_linhdr.prior_hdr ? "enabled" : "disabled",
                g_linhdr.prior_wcg < 0 ? "unknown"
                    : (g_linhdr.prior_wcg ? "enabled" : "disabled"),
                sb);
        return 1;
    }
    log_msg("HDR sys: no HDR-capable enabled output found "
            "(read %d lines, %d outputs) — tone-mapped output",
            lines_seen, outputs_seen);
    s_detect_none = 1;
    return 0;
}

/* Flip the output's HDR state, and wide gamut ONLY when we know the
 * state we found it in. Best-effort: a failure leaves us on the
 * tone-mapped path, which still works.
 *
 * HDR is ours to drive, so on the way out it goes back to what detect
 * recorded rather than a hardcoded "disable". WCG is written only when
 * detect actually read it AND found it off — i.e. only when engaging
 * HDR requires turning it on, and then only to put it back exactly as
 * found. Unknown (-1) or already-on means we never touch it: kscreen
 * persists these properties, so a write we can't reverse is a
 * permanent change to the user's display for a setting they never
 * asked us to manage.
 *
 * This call stays synchronous. The deck went fire-and-forget to stop
 * kscreen-doctor blocking its main loop for ~2 s, and immediately had
 * to add a wait back at the engage site because the display must be in
 * HDR before the PQ swapchain is claimed. x64 stays synchronous via
 * hdr_run_kscreen (bounded, shell-free — deck 2f00c37 port), so that
 * ordering holds for free. If this is ever made async, restore the
 * wait at hdr_sys_engage — see deck c516ed2. */

/* Reference-luminance target while we present (deck 37806c1 lineage).
 * KWin scales every HDR frame by (SDR brightness)/203 before the wire —
 * deck field 2026-08-25: a desktop value of 800 nits (the calibrate
 * wizard's unexplained second slider) overdrove all HDR ~3.9x and
 * crushed it back under the peak ceiling; 203 makes the compositor's
 * multiplier exactly 1.0. DSVP_SDR_BRIGHTNESS overrides; 0 opts out. */
static int hdr_sdr_target(void) {
    static int target = -2;
    if (target == -2) {
        target = 203;
        const char *e = SDL_getenv("DSVP_SDR_BRIGHTNESS");
        if (e && e[0]) {
            char *end = NULL;
            long lv = strtol(e, &end, 10);
            int v = (end && *end == '\0') ? (int)lv : -1;   /* junk → out-of-range branch */
            if (v == 0) {
                target = 0;
                log_msg("HDR sys: DSVP_SDR_BRIGHTNESS=0 — reference "
                        "luminance left to the desktop setting");
            } else if (v >= 50 && v <= 2000) {
                target = v;
                log_msg("HDR sys: reference luminance target %d nits "
                        "(DSVP_SDR_BRIGHTNESS)", v);
            } else {
                log_msg("HDR sys: DSVP_SDR_BRIGHTNESS=%s out of range "
                        "(50-2000) — using 203", e);
            }
        }
    }
    return target;
}

/* True when engaging should also move KWin's SDR-brightness: we could
 * read the desktop value (read-first rule — unreported means never
 * touch), a target is configured, and the desktop value differs. */
/* Playback reference target while PASSTHROUGH is live (deck caaded5
 * port; hold lifted for x64 by Holden 2026-09-06). Field-convicted on
 * the deck 2026-08-28: KWin scales the ENTIRE PQ surface — passthrough
 * frames included — by (SDR brightness)/203, so the "accurate" 203
 * hold rendered absolute-nits content ~5x too dark through this chain.
 * Holden's eye ladder settled 1000 nits for passthrough vs 203 for
 * SDR/tone-map (conditions: KWin peak ceiling 1700, permanent-HDR
 * desktop, C4 Filmmaker Mode). A Plasma user pressing Z on x64 got the
 * pre-fix picture until this port. DSVP_PASS_SDR_BRIGHTNESS overrides;
 * 0 falls back to the uniform hold. DSVP_SDR_BRIGHTNESS=0 (the global
 * opt-out) still disables every write. */
static int hdr_pass_sdr_target(void) {
    static int target = -2;
    if (target == -2) {
        target = 1000;
        const char *e = SDL_getenv("DSVP_PASS_SDR_BRIGHTNESS");
        if (e && e[0]) {
            char *end = NULL;
            long lv = strtol(e, &end, 10);
            int v = (end && *end == '\0') ? (int)lv : -1;
            if (v == 0) {
                target = 0;
                log_msg("HDR sys: DSVP_PASS_SDR_BRIGHTNESS=0 — "
                        "passthrough holds the uniform target");
            } else if (v >= 50 && v <= 2000) {
                target = v;
                log_msg("HDR sys: passthrough reference target %d nits "
                        "(DSVP_PASS_SDR_BRIGHTNESS)", v);
            } else {
                log_msg("HDR sys: DSVP_PASS_SDR_BRIGHTNESS=%s out of "
                        "range (50-2000) — using 1000", e);
            }
        }
    }
    return target;
}

/* The reference-luminance value the current output MODE wants held:
 * passthrough live → the playback target; SDR/tone-map output → the
 * uniform target. 0 = no hold (opt-out). */
static int hdr_mode_sdr_target(int pass_active) {
    int base = hdr_sdr_target();
    if (base <= 0) return 0;            /* global opt-out is absolute */
    if (pass_active) {
        int p = hdr_pass_sdr_target();
        return p > 0 ? p : base;
    }
    return base;
}

/* True when engaging should also own KWin's SDR-brightness: we could
 * read the desktop value (read-first rule — unreported means never
 * touch), a target is configured, and the desktop value differs from
 * EITHER mode's target (deck a56b870: with the desktop parked at
 * exactly 203 the single-target test answered "no work", nothing was
 * owned, and the passthrough write was gated off with it). */
static int hdr_sdrbr_needed(void) {
    int t_sdr  = hdr_mode_sdr_target(0);
    int t_pass = hdr_mode_sdr_target(1);
    return g_linhdr.prior_sdrbr > 0
        && ((t_sdr  > 0 && g_linhdr.prior_sdrbr != t_sdr)
         || (t_pass > 0 && g_linhdr.prior_sdrbr != t_pass));
}

static int hdr_sys_set(int on) {
    if (!g_linhdr.output[0]) return -1;

    char hdr_part[64];
    snprintf(hdr_part, sizeof(hdr_part), "output.%s.hdr.%s",
             g_linhdr.output,
             (on || g_linhdr.prior_hdr) ? "enable" : "disable");
    int write_wcg = (g_linhdr.prior_wcg == 0);
    char wcg_part[80] = "";
    if (write_wcg)
        snprintf(wcg_part, sizeof(wcg_part), "output.%s.wcg.%s",
                 g_linhdr.output, on ? "enable" : "disable");

    /* Reference luminance rides the same write and the same
     * restore-as-found contract: engage holds the target, revert puts
     * the exact desktop value back. hdr_sdrbr_needed carries the
     * read-first rule, so an unreported value is never written. */
    char sdr_part[80] = "";
    int write_sdrbr = hdr_sdrbr_needed();
    if (write_sdrbr)
        snprintf(sdr_part, sizeof(sdr_part),
                 "output.%s.sdr-brightness.%d",
                 g_linhdr.output,
                 on ? hdr_sdr_target() : g_linhdr.prior_sdrbr);

    /* Synchronous + bounded + shell-free (deck 2f00c37 port): same
     * ordering guarantee system() gave (display in HDR before the PQ
     * swapchain is claimed), no unbounded hang, no shell. */
    /* WRITE-AHEAD stamp (review C4): the insurance exists BEFORE the
     * display write. A timed-out write may still have landed (the
     * D-Bus request outlives the killed client), and the old order —
     * stamp only on rc == 0 — left the display changed with no record
     * and a log that promised "next launch reconciles" a stamp that was
     * never written. Restore clears it only on a CONFIRMED rc == 0. */
    if (on) hdr_stamp_update(1);
    int rc = hdr_run_kscreen(hdr_part, wcg_part, sdr_part);
    if (rc != 0 && (write_wcg || write_sdrbr)) {
        /* One rejected property blocks the whole call (d-D4-11):
         * retry the HDR part alone, then the rest, so a version that
         * lacks sdr-brightness cannot pin the display. */
        log_msg("HDR sys: combined write rc=%d — retrying hdr part alone", rc);
        rc = hdr_run_kscreen(hdr_part, "", "");
        if (rc == 0 && (write_wcg || write_sdrbr))
            hdr_run_kscreen(write_wcg ? wcg_part : sdr_part,
                            (write_wcg && write_sdrbr) ? sdr_part : "", "");
    }
    if (!on && rc == 0) hdr_stamp_update(0);
    if (write_sdrbr && rc == 0)
        g_linhdr.held_sdrbr = on ? hdr_sdr_target() : 0;
    char sbdesc[40] = "untouched";
    if (write_sdrbr) {
        if (on)
            snprintf(sbdesc, sizeof(sbdesc), "held at %d",
                     hdr_sdr_target());
        else
            snprintf(sbdesc, sizeof(sbdesc), "restored to %d",
                     g_linhdr.prior_sdrbr);
    }
    log_msg("HDR sys: display HDR %s on %s (wcg %s, sdr-brightness %s, "
            "rc=%d)",
            on ? "ENABLED" : "restored to baseline",
            g_linhdr.output,
            write_wcg ? (on ? "enabled" : "restored to disabled")
                      : "untouched",
            sbdesc, rc);
    return rc;
}

static void linhdr_revert(void) {
    if (!g_linhdr.engaged) return;
    /* `engaged` stays set until the restore is CONFIRMED (review
     * DM12): clearing it first let the next open re-detect the HELD
     * state as the baseline, and a clean quit then wrote hdr.enable
     * and unlinked the stamp. */
    s_hdr_budget_deadline = get_time_sec() + HDR_SEQ_BUDGET_SEC;
    int rc = hdr_sys_set(0);
    if (rc != 0) {
        /* The restore direction gets a retry (deck b91f15e port,
         * review M2) — it is the direction restore-as-found actually
         * protects. */
        log_msg("HDR sys: restore FAILED (rc=%d) — retrying once", rc);
        rc = hdr_sys_set(0);
    }
    s_hdr_budget_deadline = 0.0;
    if (rc == 0) {
        g_linhdr.engaged = 0;
    } else {
        log_msg("HDR sys: retry also failed — display may be left in HDR; "
                "the stamp written at engage stays armed, next launch "
                "reconciles (engaged kept: no re-detect as baseline)");
    }
}

static void linhdr_atexit(void) { linhdr_revert(); }

/* ── Mode-transition write for the reference luminance ONLY (deck
 * caaded5 port) ── the held value FOLLOWS THE OUTPUT MODE; the
 * desktop baseline and the crash stamp are untouched, so
 * restore-as-found still puts the desktop value back on exit. One
 * bounded call; a failed write logs and plays on — wrong brightness,
 * never a wedge (fail open). Called from both hdr_output_apply exits. */
static void hdr_sys_apply_sdrbr(int pass_active) {
    if (!g_linhdr.engaged || !g_linhdr.output[0]) return;
    if (g_linhdr.prior_sdrbr <= 0) return;   /* read-first: never write blind */
    int t = hdr_mode_sdr_target(pass_active);
    if (t <= 0) return;                        /* opt-out */
    int cur = g_linhdr.held_sdrbr > 0 ? g_linhdr.held_sdrbr : g_linhdr.prior_sdrbr;
    if (t == cur) return;
    char sdr_part[80];
    snprintf(sdr_part, sizeof(sdr_part), "output.%s.sdr-brightness.%d",
             g_linhdr.output, t);
    int rc = hdr_run_kscreen(sdr_part, "", "");
    if (rc == 0) g_linhdr.held_sdrbr = t;
    log_msg("HDR sys: sdr-brightness -> %d (%s hold, rc=%d%s)",
            t, pass_active ? "passthrough" : "tone-map/SDR", rc,
            rc == 0 ? "" : " — NOT applied, playing on");
}
int hdr_sys_held_sdrbr(void) { return g_linhdr.engaged ? g_linhdr.held_sdrbr : 0; }

/* ── Read-after-enable for the reference luminance (deck b0615b9
 * port, review HDR M8) ── kscreen-doctor only REPORTS sdr-brightness
 * while the output's HDR is enabled, so an engage from an HDR-off
 * baseline could never read the value the read-first rule requires
 * before writing — the 800-nit 3.9x overdrive stayed live for
 * exactly those sessions. After a CONFIRMED enable, the value read
 * is KWin's stored per-output setting — the user's own configuration
 * and therefore the restore-as-found baseline. Learn it, re-stamp,
 * hold the target. An unreported value is still never written. */
static void hdr_sys_learn_sdrbr(void) {
    if (!g_linhdr.engaged) return;
    if (g_linhdr.prior_sdrbr > 0) return;   /* already known */
    if (hdr_sdr_target() <= 0) return;       /* DSVP_SDR_BRIGHTNESS=0 */
    if (!g_linhdr.output[0]) return;

    static char lbuf[65536];
    int lfd = -1;
    char *largv[] = { "kscreen-doctor", "-o", NULL };
    pid_t lpid = hdr_spawn_capture(largv, &lfd);
    if (lpid < 0) return;
    hdr_read_bounded(lfd, lpid, lbuf, sizeof(lbuf));
    close(lfd);
    if (hdr_sys_wait_bounded(lpid) != 0) return;

    int in_ours = 0, val = -1;
    char *lsp = NULL;
    for (char *line = strtok_r(lbuf, "\n", &lsp); line;
         line = strtok_r(NULL, "\n", &lsp)) {
        strip_ansi(line);
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "Output:", 7) == 0) {
            char idx[16], name[32] = "";
            in_ours = (sscanf(s, "Output: %15s %31s", idx, name) == 2
                       && strcmp(name, g_linhdr.output) == 0);
        } else if (in_ours && strncmp(s, "SDR brightness:", 15) == 0) {
            int v = -1;
            if (sscanf(s + 15, " %d", &v) == 1 && v > 0) val = v;
        }
    }
    if (val <= 0) return;   /* still unreported — never write */

    g_linhdr.prior_sdrbr = val;
    if (!hdr_sdrbr_needed()) {
        log_msg("HDR sys: reference luminance already %d nits — "
                "no hold needed in either mode (read-after-enable)", val);
        return;
    }
    log_msg("HDR sys: learned desktop reference luminance %d nits "
            "post-enable — holding %d for playback, %d restores "
            "(read-after-enable)", val, hdr_sdr_target(), val);
    hdr_sys_set(1);   /* no-op hdr re-enable; brightness hold + re-stamp ride */
}

static int hdr_sys_engage(PlayerState *ps) {
    if (g_linhdr.engaged) return 1;
    const char *win_disp = NULL;
    if (ps && ps->window) {
        SDL_DisplayID did = SDL_GetDisplayForWindow(ps->window);
        if (did) win_disp = SDL_GetDisplayName(did);
    }
    int owner = 0;
    if (hdr_stamp_foreign_live(&owner)) {
        /* Another dsvp holds the display: what detect would read now is
         * ITS held state, not the user's baseline. Ride it, never own
         * it (DM12). The swapchain poll decides whether ST2084 works. */
        log_msg("HDR sys: display already owned by live dsvp pid %d — "
                "riding its state, not engaging", owner);
        return 1;
    }
    /* Detect-negative session memory (field 2026-09-07, dellbian: on a
     * box with NO HDR-capable output every Z re-ran the bounded
     * kscreen probe on the main thread and dropped a frame each time —
     * the d-D3-8 memory in hdr_output_apply only covers a failed
     * post-engage ST2084 poll, which that box never reaches). Only a
     * probe that ANSWERED "nothing capable" is remembered, keyed on
     * the display set (window's display name + display count): a
     * hot-plugged or moved display re-probes, so an HDR monitor
     * connected later is not vetoed by a memory taken before it
     * existed. Probe failures and timeouts are not remembered — they
     * retry on the next Z, as before. */
    static int  s_none_remembered = 0;
    static char s_none_disp[64] = "";
    static int  s_none_ndisp = -1;
    int ndisp = 0;
    {
        SDL_DisplayID *ids = SDL_GetDisplays(&ndisp);
        if (ids) SDL_free(ids);
    }
    if (s_none_remembered && ndisp == s_none_ndisp &&
        strcmp(win_disp ? win_disp : "", s_none_disp) == 0) {
        log_msg("HDR sys: no HDR-capable output earlier this session "
                "(display '%s', %d display%s) — probe skipped, tone-mapped output",
                s_none_disp, ndisp, ndisp == 1 ? "" : "s");
        return 0;
    }
    s_hdr_budget_deadline = get_time_sec() + HDR_SEQ_BUDGET_SEC;
    s_hdr_budget_hits = 0;
    double seq_t0 = get_time_sec();
    if (!hdr_sys_detect(win_disp)) {
        s_hdr_budget_deadline = 0.0;
        if (s_detect_none) {
            s_none_remembered = 1;
            s_none_ndisp = ndisp;
            snprintf(s_none_disp, sizeof(s_none_disp), "%s", win_disp ? win_disp : "");
            log_msg("HDR sys: remembering that for this session — the next Z "
                    "skips the probe unless the display set changes");
        }
        return 0;
    }
    s_none_remembered = 0;   /* a capable output exists now */
    if (!g_linhdr.prior_hdr || hdr_sdrbr_needed()) {
        if (!g_linhdr.handlers) {
            g_linhdr.handlers = 1;
            atexit(linhdr_atexit);
        }
        int erc = hdr_sys_set(1);
        g_linhdr.engaged = 1;
        /* The engage checks its own exit status (deck b91f15e port,
         * review M5): retry once, then report failure so the caller
         * stays on the reference tone map instead of handing KWin a
         * PQ surface over an SDR output. `engaged` stays set — the
         * revert is a baseline no-op and cleans up the stamp. */
        if (erc != 0) {
            log_msg("HDR sys: engage failed (rc=%d) — retrying", erc);
            erc = hdr_sys_set(1);
        }
        if (erc != 0) {
            log_msg("HDR sys: engage failed twice%s — passthrough "
                    "vetoed, reference tone map",
                    s_hdr_budget_hits ? " (sequence budget exhausted)" : "");
            s_hdr_budget_deadline = 0.0;
            return 0;
        }
        hdr_sys_learn_sdrbr();   /* HDR-off baselines: readable now */
    }
    log_msg("HDR sys: engage sequence %.2fs (budget %.0fs, %d timeout%s)",
            get_time_sec() - seq_t0, HDR_SEQ_BUDGET_SEC,
            s_hdr_budget_hits, s_hdr_budget_hits == 1 ? "" : "s");
    s_hdr_budget_deadline = 0.0;
    /* else: output already in HDR by the user's hand AND its reference
     * luminance already at target — use it, never own it. An
     * already-HDR output whose SDR brightness needs holding IS
     * engaged (deck field 2026-08-25: this is the common config):
     * the hdr part of that write is a no-op re-enable, and `engaged`
     * routes the brightness restore-as-found through the normal
     * revert. */
    return 1;
}

static void hdr_sys_revert_all(void) {
    linhdr_revert();
}

#endif /* _WIN32 */

/* Reconcile (display HDR, swapchain, shader path) with the current
 * (mode, content). Called at file open (after gpu_setup_uniforms),
 * file close, and the Z toggle — deck lineage. Pipelines are format-
 * bound to the swapchain, so a switch recreates them; the frame
 * cache texture is swapchain-format-bound too and is dropped
 * explicitly (x64 keeps it on video-texture lifetime, not pipeline
 * lifetime — deck difference). Every failure path lands on the
 * tone-mapped SDR output, which is today's baseline. */
void hdr_output_apply(PlayerState *ps) {
    if (!ps->gpu_device || !ps->window) return;

    /* A post-engage ST2084 poll that failed is remembered for the
     * session (review d-D3-8): without this every HDR open paid the
     * display flip, the 2.5 s frozen poll and the revert again — and
     * on a box with no HDR output (dellbian) every Z press did. */
    static int s_st2084_denied = 0;

    int want = ps->hdr_out_mode == 1 && ps->hdr_pass_content
            && !SDL_getenv("DSVP_NO_SYS_HDR");
    if (want && s_st2084_denied) {
        log_msg("HDR out: ST2084 denied earlier this session — skipping "
                "display engage, tone-mapped output");
        want = 0;
    }

    /* Engage the DISPLAY first: Windows drivers may only advertise
     * ST2084 once display HDR is on, and KWin tone-maps a PQ surface
     * to SDR itself unless the output is switched. (No Game Mode
     * compositor branch here by decision — x64 is Debian + Windows
     * desktops only, Holden 2026-09-05; the deck keeps its own.) */
    if (want)
        want = hdr_sys_engage(ps);

    /* Post-engage support poll. SDL's Vulkan backend re-queries
     * surface formats live on every call (verified in SDL 3.4
     * source); field measurement on NVIDIA showed support=1 at ~0 ms,
     * the poll covers slower drivers. */
    if (want) {
        int sup = SDL_WindowSupportsGPUSwapchainComposition(
                      ps->gpu_device, ps->window,
                      SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
        int waited = 0;
        while (!sup && waited < 2500) {
            SDL_Delay(100);
            waited += 100;
            sup = SDL_WindowSupportsGPUSwapchainComposition(
                      ps->gpu_device, ps->window,
                      SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
        }
        if (!sup) {
            log_msg("HDR out: no ST2084 swapchain support after %d ms — "
                    "tone-mapped output (remembered for this session)", waited);
            s_st2084_denied = 1;
            want = 0;
        } else if (waited) {
            log_msg("HDR out: ST2084 support appeared after ~%d ms", waited);
        }
    }

    if (want == ps->hdr_out_active) {
        ps->gpu_uniforms.hdr_pass = want ? 1.0f : 0.0f;
        if (!want)
            hdr_sys_revert_all();   /* e.g. engaged but support didn't come */
        else
            hdr_sys_apply_sdrbr(1); /* passthrough already live: hold follows the mode */
        return;
    }

    /* Swapchain switch + pipeline recreate. The display's own HDR
     * mode-switch blank overlaps the shader recompile. */
    SDL_WaitForGPUIdle(ps->gpu_device);
    if (!SDL_SetGPUSwapchainParameters(ps->gpu_device, ps->window,
            want ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084
                 : SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_PRESENTMODE_VSYNC)) {
        log_msg("HDR out: swapchain switch failed (%s) — staying %s",
                SDL_GetError(), ps->hdr_out_active ? "HDR" : "SDR");
        ps->gpu_uniforms.hdr_pass = ps->hdr_out_active ? 1.0f : 0.0f;
        if (want)
            hdr_sys_revert_all();  /* don't hold the display in HDR for SDR */
        return;
    }

    gpu_destroy_pipelines(ps);
    /* Frame cache is swapchain-format-bound: drop it so the next
     * ensure recreates it in the new format. */
    if (ps->gpu_tex_cache) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_cache);
        ps->gpu_tex_cache = NULL;
    }
    ps->cache_w = ps->cache_h = 0;
    ps->cache_valid = 0;

    if (gpu_create_pipelines(ps) < 0) {
        /* Limp back to SDR — an SDR-format pipeline set is the known-
         * good configuration from startup. */
        log_msg("ERROR: pipeline recreation after HDR switch failed — "
                "reverting to SDR");
        int sdr_ok = SDL_SetGPUSwapchainParameters(ps->gpu_device, ps->window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
        if (!sdr_ok)
            log_msg("FATAL: SDR swapchain set also failed (%s) — swapchain "
                    "stays %s", SDL_GetError(), want ? "HDR10" : "as it was");
        if (gpu_create_pipelines(ps) < 0)
            log_msg("FATAL: SDR pipeline recreation also failed");
        /* Record what the swapchain IS, not what we wished (deck minor
         * 13): forcing active=0 against a live HDR10 swapchain desynced
         * every later decision. */
        ps->hdr_out_active = sdr_ok ? 0 : (want ? 1 : ps->hdr_out_active);
        ps->gpu_uniforms.hdr_pass = ps->hdr_out_active ? 1.0f : 0.0f;
        if (!ps->hdr_out_active) hdr_sys_revert_all();
        return;
    }

    ps->hdr_out_active = want;
    ps->gpu_uniforms.hdr_pass = want ? 1.0f : 0.0f;
    log_msg("HDR out: %s", want
            ? "PASSTHROUGH — HDR10/ST2084 swapchain, display tone-maps"
            : "SDR — tone-mapped output");

    /* Leaving passthrough: restore the display to its prior state.
     * Entering it: the reference-luminance hold moves to the
     * passthrough target (KWin scales the PQ surface by held/203 —
     * deck 2026-08-28 conviction, ported 2026-09-06). */
    if (!want)
        hdr_sys_revert_all();
    else
        hdr_sys_apply_sdrbr(1);
}

/* App-exit revert: covers quitting while an HDR file was playing.
 * player_close covers the normal file-close path. */
void hdr_output_shutdown(PlayerState *ps) {
    (void)ps;
    hdr_sys_revert_all();
}


/* ═══════════════════════════════════════════════════════════════════
 * Open / Close
 * ═══════════════════════════════════════════════════════════════════ */

/* Open a media file: probe format, find best streams, init decoders,
 * set up scaling context, create GPU textures, start demux thread. */
int player_open(PlayerState *ps, const char *filename) {
    int ret;

    strncpy(ps->filepath, filename, sizeof(ps->filepath) - 1);
    ps->filepath[sizeof(ps->filepath) - 1] = '\0';
    log_msg("player_open: %s", filename);

    /* ── Open container ── */
    ps->fmt_ctx = NULL;
    /* "No networking whatsoever" is enforced, not just claimed: the
     * bundled FFmpeg has network protocols compiled in, so without this
     * whitelist a URL argument would happily demux over the network. */
    AVDictionary *open_opts = NULL;
    av_dict_set(&open_opts, "protocol_whitelist", "file", 0);
    ret = avformat_open_input(&ps->fmt_ctx, filename, NULL, &open_opts);
    av_dict_free(&open_opts);
    if (ret < 0) {
        log_msg("ERROR: avformat_open_input failed: %s", av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(ps->fmt_ctx, NULL);
    if (ret < 0) {
        log_msg("ERROR: avformat_find_stream_info failed: %s", av_err2str(ret));
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    log_msg("Container: %s (%s), streams=%d",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name,
        ps->fmt_ctx->nb_streams);

    /* ── Find best video stream ── */
    ps->video_stream_idx = av_find_best_stream(ps->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ps->audio_stream_idx = av_find_best_stream(ps->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, ps->video_stream_idx, NULL, 0);

    /* ── Skip TrueHD audio (unusable without HDMI bitstreaming) ──
     *
     * TrueHD Atmos 7.1 MLP decode is extremely CPU-heavy and starves the
     * video pipeline on complex files (4K HEVC 10-bit + 29 streams).
     * Without an AVR/soundbar via HDMI, it just gets crushed to S16 stereo
     * anyway — pointless pain. Every Blu-ray with TrueHD ships an AC3 or
     * EAC3 compatibility track. Pick that instead.
     *
     * Will be removed when HDMI bitstreaming support lands. */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        if (as->codecpar->codec_id == AV_CODEC_ID_TRUEHD) {
            log_msg("Audio: default stream is TrueHD — skipping (no bitstream support)");
            int fallback = -1;
            for (unsigned i = 0; i < ps->fmt_ctx->nb_streams; i++) {
                AVStream *st = ps->fmt_ctx->streams[i];
                if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
                if ((int)i == ps->audio_stream_idx) continue;
                if (st->codecpar->codec_id == AV_CODEC_ID_TRUEHD) continue;
                fallback = (int)i;
                break;
            }
            if (fallback >= 0) {
                const AVCodec *fc = avcodec_find_decoder(
                    ps->fmt_ctx->streams[fallback]->codecpar->codec_id);
                log_msg("Audio: fallback to stream %d (%s)",
                    fallback, fc ? fc->name : "unknown");
                ps->audio_stream_idx = fallback;
            } else {
                log_msg("Audio: no non-TrueHD fallback found — playing without audio");
                ps->audio_stream_idx = -1;
            }
        }
    }

    if (ps->video_stream_idx < 0 && ps->audio_stream_idx < 0) {
        log_msg("ERROR: No playable video or audio stream found");
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    if (ps->video_stream_idx < 0)
        log_msg("No video stream — audio-only playback");
    log_msg("Video stream: idx=%d, Audio stream: idx=%d",
        ps->video_stream_idx, ps->audio_stream_idx);

    /* ── Open video decoder (SOFTWARE ONLY) ── */
    if (ps->video_stream_idx >= 0) {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        const AVCodec *codec = NULL;

        /* FFmpeg 8.1's generic 'av1' decoder probes for hardware accel
         * first and fails catastrophically on systems without AV1 HW
         * decode (spams "Failed to get pixel format", zero frames output).
         * Force libdav1d — it's pure software, always works, and is the
         * reference AV1 decoder. */
        if (vs->codecpar->codec_id == AV_CODEC_ID_AV1) {
            codec = avcodec_find_decoder_by_name("libdav1d");
            if (codec)
                log_msg("Video codec: libdav1d forced for AV1 (avoiding hw probe)");
        }
        if (!codec)
            codec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!codec) {
            log_msg("ERROR: Unsupported video codec id=%d", vs->codecpar->codec_id);
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }
        if (vs->codecpar->codec_id != AV_CODEC_ID_AV1)
            log_msg("Video codec: %s (%s)", codec->name, codec->long_name);

        ps->video_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ps->video_codec_ctx, vs->codecpar);

        /* Thread count: auto-detect, capped at 12 for HEVC.
         *
         * FF_THREAD_FRAME buffers N frames before outputting the first.
         * Pipeline fill latency = N × frame_dur (linear, not log).
         * At 24fps: 16T=667ms, 12T=500ms, 8T=333ms, 4T=167ms.
         *
         * On high-core machines (16T+), the 667ms fill causes permanent
         * A/V desync on heavy files (4K HEVC 10-bit + TrueHD + 29 streams).
         * Cap at 12 limits fill to 500ms while retaining decode throughput.
         * Low-core machines (≤12 cores) are unaffected — auto stays below cap.
         *
         * Non-HEVC codecs (VC-1, H.264) are uncapped — their pipeline fill
         * is small enough that the existing A/V sync handles it fine.
         *
         * H.264 cap at 8: on high-core machines (16T+), uncapped auto gives
         * 16 threads → 533ms pipeline fill at 30fps.  Combined with MPEG-TS
         * interleaved audio/video, this widens the PTS gap between the first
         * audio and video packets after seek, amplifying post-seek A/V drift.
         * Cap at 8 (267ms fill) retains full decode throughput for 1080p. */
        ps->video_codec_ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

        /* Default: auto-detect logical cores, with per-codec upper caps to
         * limit FF_THREAD_FRAME pipeline-fill latency on high-core machines.
         * Caps tuned for ~30fps content; lower fps = proportionally less fill. */
        int auto_count = SDL_GetNumLogicalCPUCores();
        int cap;
        switch (codec->id) {
            case AV_CODEC_ID_HEVC:  cap = 12; break;  /* ~500ms fill at 24fps */
            case AV_CODEC_ID_H264:  cap = 8;  break;  /* ~267ms fill at 30fps */
            default:                cap = 16; break;  /* generous default */
        }
        ps->video_codec_ctx->thread_count = (auto_count < cap) ? auto_count : cap;

        ret = avcodec_open2(ps->video_codec_ctx, codec, NULL);
        if (ret < 0) {
            log_msg("ERROR: Cannot open video codec: %s", av_err2str(ret));
            /* Free the context too — leaving the stale pointer leaked one
             * context (+ params) per failed open, and this failure never
             * reached the log file (fprintf only). */
            avcodec_free_context(&ps->video_codec_ctx);
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }

        /* Colour tags: the container is authoritative when it says
         * something; when it says UNSPECIFIED, the bitstream VUI the
         * decoder just parsed out of the extradata is the authored
         * value and every reader downstream (HDR detection, the
         * YCbCr matrix pick, the 2020-family siting predicate, sws)
         * reads the stream's codecpar — so the backfill happens here,
         * once. Field 2026-09-07 (dellbian, FFmpeg 9.0): a PQ clip
         * whose container carried only the matrix opened with
         * stream trc=unknown/primaries=unknown while the decoder
         * reported smpte2084/bt2020 — no HDR line, SDR shader on an
         * HDR stream. Older muxers write no colour element at all;
         * this is the real-world shape, not a synthetic-clip quirk. */
        {
            AVCodecParameters *cp = vs->codecpar;
            const AVCodecContext *cc = ps->video_codec_ctx;
            int filled = 0;
            if (cp->color_trc == AVCOL_TRC_UNSPECIFIED &&
                cc->color_trc != AVCOL_TRC_UNSPECIFIED) {
                cp->color_trc = cc->color_trc; filled |= 1;
            }
            if (cp->color_primaries == AVCOL_PRI_UNSPECIFIED &&
                cc->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                cp->color_primaries = cc->color_primaries; filled |= 2;
            }
            if (cp->color_space == AVCOL_SPC_UNSPECIFIED &&
                cc->colorspace != AVCOL_SPC_UNSPECIFIED) {
                cp->color_space = cc->colorspace; filled |= 4;
            }
            if (cp->color_range == AVCOL_RANGE_UNSPECIFIED &&
                cc->color_range != AVCOL_RANGE_UNSPECIFIED) {
                cp->color_range = cc->color_range; filled |= 8;
            }
            if (filled)
                log_msg("Colour tags: container unspecified — taken from the "
                        "bitstream VUI:%s%s%s%s",
                        (filled & 1) ? " transfer"   : "",
                        (filled & 2) ? " primaries"  : "",
                        (filled & 4) ? " matrix"     : "",
                        (filled & 8) ? " range"      : "");
        }

        ps->vid_w = ps->video_codec_ctx->width;
        ps->vid_h = ps->video_codec_ctx->height;
        ps->expected_pix_fmt = ps->video_codec_ctx->pix_fmt;
        log_msg("Video: %dx%d, pix_fmt=%s, threads=%d",
            ps->vid_w, ps->vid_h,
            av_get_pix_fmt_name(ps->video_codec_ctx->pix_fmt),
            ps->video_codec_ctx->thread_count);
    } else {
        /* Audio-only: no video dimensions */
        ps->vid_w = 0;
        ps->vid_h = 0;
    }

    /* ── Open audio decoder ── */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
        if (codec) {
            ps->audio_codec_ctx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(ps->audio_codec_ctx, as->codecpar);
            ps->audio_codec_ctx->thread_count = 0;
            ret = avcodec_open2(ps->audio_codec_ctx, codec, NULL);
            if (ret < 0) {
                fprintf(stderr, "[DSVP] Cannot open audio codec: %s\n", av_err2str(ret));
                avcodec_free_context(&ps->audio_codec_ctx);
                ps->audio_stream_idx = -1;
            }
        } else {
            ps->audio_stream_idx = -1;
        }
    }

    /* ── Find subtitle streams ── */
    sub_find_streams(ps);

    /* ── Find audio streams ── */
    audio_find_streams(ps);

    /* ── Discard unused streams (reduces demux I/O) ──
     *
     * Tell the demuxer to skip packets for streams we won't decode.
     * Saves I/O on files with many streams (e.g. 41-stream DV files).
     * Also eliminates DV dual-layer enhancement layer overhead if present.
     */
    {
        int discarded = 0;

        for (unsigned i = 0; i < ps->fmt_ctx->nb_streams; i++) {
            int idx = (int)i;

            /* Keep the selected video and audio streams */
            if (idx == ps->video_stream_idx) continue;
            if (idx == ps->audio_stream_idx) continue;

            /* Keep all cataloged subtitle streams */
            int is_sub = 0;
            for (int s = 0; s < ps->sub_count; s++) {
                if (idx == ps->sub_stream_indices[s]) {
                    is_sub = 1;
                    break;
                }
            }
            if (is_sub) continue;

            /* Keep all cataloged audio streams (for audio cycling) */
            int is_aud = 0;
            for (int a = 0; a < ps->aud_count; a++) {
                if (idx == ps->aud_stream_indices[a]) {
                    is_aud = 1;
                    break;
                }
            }
            if (is_aud) continue;

            /* Check if this is a DV enhancement layer video stream */
            if (ps->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                log_msg("Stream %d: DV enhancement layer video — discarding", idx);
            }

            ps->fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
            discarded++;
        }

        if (discarded > 0)
            log_msg("Demux: discarding %d unused stream(s) to reduce I/O", discarded);
    }

    /* ── Allocate decode frames ── */
    ps->video_frame = av_frame_alloc();
    ps->rgb_frame   = av_frame_alloc();
    ps->audio_frame = av_frame_alloc();
    if (!ps->video_frame || !ps->rgb_frame || !ps->audio_frame) {
        log_msg("ERROR: frame allocation failed");
        return -1;
    }

    /* ── Set up swscale (or skip for GPU passthrough) ──
     *
     * yuv420p10le: bypass swscale. Raw 10-bit planes → R16_UNORM textures.
     * yuv420p:     bypass swscale. Raw 8-bit planes → R8_UNORM textures.
     *              Range expansion (limited→full) done in fragment shader.
     *
     * All other pixel formats need swscale conversion to YUV420P first.
     * Shader handles the color matrix and any remaining range work.
     */
    if (ps->video_codec_ctx) {
        enum AVPixelFormat src_fmt = ps->video_codec_ctx->pix_fmt;
        int is_10bit  = (src_fmt == AV_PIX_FMT_YUV420P10LE);
        int is_yuv420p = (src_fmt == AV_PIX_FMT_YUV420P);

        if (is_10bit) {
            /* ── 10-bit GPU passthrough — no swscale needed ── */
            ps->sws_ctx    = NULL;
            ps->rgb_buffer = NULL;
            log_msg("swscale: bypassed (10-bit GPU passthrough)");

        } else if (is_yuv420p) {
            /* ── 8-bit YUV420P passthrough — range in shader ── */
            ps->sws_ctx    = NULL;
            ps->rgb_buffer = NULL;
            log_msg("swscale: bypassed (8-bit YUV420P, range in shader)");

        } else {
            /* ── swscale path for all other formats ──
             * Deep sources (12-bit HEVC, 10-bit 4:2:2/4:4:4) convert to
             * yuv420p10le and ride the R16 upload path — converting to
             * 8-bit here quantized the PQ signal to 256 codes BEFORE
             * the EETF stretched it, guaranteeing shadow banding that
             * no output dither can repair. 8-bit sources keep the
             * 8-bit destination. */
            const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(src_fmt);
            int src_depth = src_desc ? src_desc->comp[0].depth : 8;
            ps->sws_out_10bit = (src_depth > 8);
            enum AVPixelFormat dst_fmt = ps->sws_out_10bit
                ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
            int dst_w = ps->vid_w;
            int dst_h = ps->vid_h;

            int sws_flags = SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
            const char *sws_mode = ps->sws_out_10bit
                ? "format convert to 10-bit (SWS_LANCZOS + ED dither)"
                : "format convert (SWS_LANCZOS + ED dither)";

            ps->sws_ctx = sws_getContext(
                ps->vid_w, ps->vid_h, src_fmt,
                dst_w, dst_h, dst_fmt,
                sws_flags,
                NULL, NULL, NULL
            );

            if (!ps->sws_ctx) {
                log_msg("ERROR: Cannot create swscale context");
                player_close(ps);
                return -1;
            }

            /* Error-diffusion dithering for format conversions */
            av_opt_set_int(ps->sws_ctx, "dithering", 1, 0);

            /* ── Colorspace and range ── */
            {
                AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;

                /* One matrix authority for both sides (deck a4b844d
                 * port, review PQUALITY P3): pick_ycbcr_matrix is the
                 * same function gpu_setup_uniforms uses, so whatever
                 * sws encodes into the planes is exactly what the
                 * shader decodes out of them — including the
                 * primaries/PQ fallback and width in the heuristic,
                 * which the old mirror here lacked, and BT.2020 tags
                 * now map to SWS_CS_BT2020 instead of falling to the
                 * heuristic arm. For RGB→YCbCr only the dst table
                 * governs the encode; src sharing it is harmless. */
                const char *sws_cs_reason;
                int pick = pick_ycbcr_matrix(par, ps->vid_w, ps->vid_h,
                                             &sws_cs_reason);
                int src_cs = (pick == 2020) ? SWS_CS_BT2020
                           : (pick == 709)  ? SWS_CS_ITU709
                                            : SWS_CS_ITU601;

                int dst_cs = src_cs;

                int src_range;
                if (par->color_range == AVCOL_RANGE_JPEG) {
                    src_range = 1;
                } else if (par->color_range == AVCOL_RANGE_MPEG) {
                    src_range = 0;
                } else {
                    src_range = 0;
                }
                int dst_range = 1;

                int *inv_table, *table;
                int cur_src_range, cur_dst_range, brightness, contrast, saturation;
                sws_getColorspaceDetails(ps->sws_ctx,
                    &inv_table, &cur_src_range, &table, &cur_dst_range,
                    &brightness, &contrast, &saturation);

                sws_setColorspaceDetails(ps->sws_ctx,
                    sws_getCoefficients(src_cs), src_range,
                    sws_getCoefficients(dst_cs), dst_range,
                    brightness, contrast, saturation);

                log_msg("swscale: colorspace=%s (%s) range=%s->full",
                    (src_cs == SWS_CS_BT2020) ? "BT.2020"
                        : (src_cs == SWS_CS_ITU709) ? "BT.709" : "BT.601",
                    sws_cs_reason,
                    src_range ? "full" : "limited");
            }

            /* ── Chroma siting ──
             * Pin BOTH sides explicitly and remember the output siting.
             * The old zero-the-shader-offset approach assumed sws always
             * re-sites chroma to center, but same-geometry conversions
             * (420 depth changes — the common case here now) take
             * unscaled per-plane converters that move nothing: the
             * output keeps the SOURCE siting. Rule: 4:2:0 sources keep
             * their siting (no resample happens or is needed); formats
             * that genuinely resample chroma (422/444/RGB → 420) are
             * pinned to LEFT, the H.264/HEVC convention. The shader
             * offset is then derived from sws_dst_siting instead of
             * being zeroed. */
            {
                AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
                enum AVChromaLocation src_loc = par->chroma_location;
                if (src_loc == AVCHROMA_LOC_UNSPECIFIED)
                    /* BT.2020-family spec-sites 4:2:0 top-left; everything
                     * else defaults LEFT as before (in lockstep with the
                     * GPU-path selection in gpu_setup_uniforms) */
                    src_loc = content_is_2020_family(par)
                              ? AVCHROMA_LOC_TOPLEFT : AVCHROMA_LOC_LEFT;

                /* AVChromaLocation → swscale chr_pos (1/256 luma units):
                 * h: left=0 center=128; v: top=0 center=128 bottom=256 */
                static const struct { int h, v; } chr_pos[] = {
                    [AVCHROMA_LOC_LEFT]       = {   0, 128 },
                    [AVCHROMA_LOC_CENTER]     = { 128, 128 },
                    [AVCHROMA_LOC_TOPLEFT]    = {   0,   0 },
                    [AVCHROMA_LOC_TOP]        = { 128,   0 },
                    [AVCHROMA_LOC_BOTTOMLEFT] = {   0, 256 },
                    [AVCHROMA_LOC_BOTTOM]     = { 128, 256 },
                };
                int src_is_420 = src_desc
                    && src_desc->log2_chroma_w == 1
                    && src_desc->log2_chroma_h == 1;
                enum AVChromaLocation dst_loc =
                    src_is_420 ? src_loc : AVCHROMA_LOC_LEFT;

                av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", chr_pos[src_loc].h, 0);
                av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", chr_pos[src_loc].v, 0);
                av_opt_set_int(ps->sws_ctx, "dst_h_chr_pos", chr_pos[dst_loc].h, 0);
                av_opt_set_int(ps->sws_ctx, "dst_v_chr_pos", chr_pos[dst_loc].v, 0);
                ps->sws_dst_siting = (int)dst_loc;

                log_msg("swscale: chroma siting src=%s dst=%s",
                        av_chroma_location_name(src_loc),
                        av_chroma_location_name(dst_loc));
            }

            log_msg("swscale: mode=%s", sws_mode);

            /* Allocate buffer for the converted frame */
            int buf_size = av_image_get_buffer_size(dst_fmt, dst_w, dst_h, 32);
            ps->rgb_buffer = av_malloc(buf_size);
            if (!ps->rgb_buffer) {
                log_msg("ERROR: conversion buffer allocation failed (%d bytes)",
                        buf_size);
                return -1;
            }
            av_image_fill_arrays(ps->rgb_frame->data, ps->rgb_frame->linesize,
                                 ps->rgb_buffer, dst_fmt, dst_w, dst_h, 32);
        }
    }

    /* ── Resize window to video dimensions ── */
    {
        if (ps->video_stream_idx >= 0 && !ps->fullscreen) {
            /* Cap to 80% of screen, maintain aspect ratio */
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(
                SDL_GetPrimaryDisplay());
            int max_w = dm ? (int)(dm->w * 0.8) : 1920;
            int max_h = dm ? (int)(dm->h * 0.8) : 1080;

            int w = ps->vid_w;
            int h = ps->vid_h;

            if (w > max_w || h > max_h) {
                double scale = fmin((double)max_w / w, (double)max_h / h);
                w = (int)(w * scale);
                h = (int)(h * scale);
            }

            ps->win_w = w;
            ps->win_h = h;

            SDL_SetWindowSize(ps->window, w, h);
            SDL_SetWindowPosition(ps->window,
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        } else {
            /* Fullscreen: don't resize, just read actual dimensions */
            int fw, fh;
            SDL_GetWindowSize(ps->window, &fw, &fh);
            ps->win_w = fw;
            ps->win_h = fh;
        }

        /* Update window title with filename */
        const char *basename = strrchr(filename, '/');
        if (!basename) basename = strrchr(filename, '\\');
        basename = basename ? basename + 1 : filename;
        char title[512];
        snprintf(title, sizeof(title), "DSVP — %s", basename);
        SDL_SetWindowTitle(ps->window, title);
    }

    /* ── Create GPU textures and transfer buffers (video streams only) ── */
    if (ps->video_stream_idx >= 0) {
        if (gpu_create_video_textures(ps) < 0) {
            log_msg("ERROR: GPU texture creation failed");
            player_close(ps);
            return -1;
        }

        /* ── Set up GPU color uniforms ── */
        gpu_setup_uniforms(ps);

        /* ── System display HDR: engage/probe per content ── */
        hdr_output_apply(ps);
    }

    /* ── Init packet queues ── */
    pq_init(&ps->video_pq);
    pq_init(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_init(&ps->sub_pqs[i]);

    /* ── Init decoded video frame queue ── */
    fq_init(&ps->video_frame_q);

    /* ── Seek mutex (protects codec flush vs decode) ── */
    ps->seek_mutex = SDL_CreateMutex();
    ps->seeking    = 0;

    /* ── Init timing ── */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;   /* assume ~25fps initially */
    ps->frame_last_pts   = 0.0;
    ps->audio_clock      = 0.0;
    ps->audio_clock_sync = 0.0;
    ps->av_bias = 0.0;
    ps->av_bias_samples = 0;
    ps->av_hold = 0;
    ps->av_hold_frames = 0;
    ps->av_trace_left = 0;
    ps->video_clock      = 0.0;

    /* Suppress frame drops until the first frame is displayed.
     * Adapts automatically to any codec's keyframe recovery time. */
    ps->seek_recovering = 1;
    ps->seek_recovering_start = get_time_sec();
    ps->seekdiag_vid_pending = 0;   /* armed only by real seeks */
    ps->seekdiag_aud_pending = 0;
    ps->seekdiag_target_valid = 0;
    ps->video_ready = 0;

    /* ── Reset diagnostics ── */
    ps->diag_frames_displayed = 0;
    ps->diag_frames_decoded   = 0;
    ps->diag_frames_dropped   = 0;
    ps->diag_multi_decodes    = 0;
    ps->diag_timer_snaps      = 0;
    ps->diag_max_av_drift     = 0.0;
    ps->diag_last_report      = get_time_sec();

    /* Real-time FPS window */
    ps->fps_window_start   = 0.0;
    ps->fps_window_frames  = 0;
    ps->rfps_window_frames = 0;
    ps->fps_content        = 0.0;
    ps->fps_render         = 0.0;

    /* ── Open audio output ── */
    if (ps->audio_codec_ctx) {
        if (audio_open(ps) < 0) {
            /* No audio device (PipeWire down, WASAPI endpoint busy, bad
             * sample rate). Fall back to the established no-audio path:
             * with the codec open and audio_stream_idx still set, demux
             * keeps queueing packets nothing drains, the queue-full
             * throttle then spins forever ~6s in, and video freezes with
             * EOF unreachable. Video-only is strictly better. */
            log_msg("ERROR: audio device open failed — continuing video-only");
            avcodec_free_context(&ps->audio_codec_ctx);
            ps->audio_stream_idx = -1;
        }
    }

    /* ── Deinterlacing policy for this file ──
     * Content-aware by default: the graph appears only if a frame
     * arrives flagged interlaced. DSVP_DEINT=0 disables entirely,
     * DSVP_DEINT=1 forces the graph from the first frame. */
    {
        const char *de = SDL_getenv("DSVP_DEINT");
        ps->deint_disabled = (de && de[0] == '0');
        ps->deint_force    = (de && de[0] == '1');
        ps->deint_graph    = NULL;
        ps->deint_src      = NULL;
        ps->deint_sink     = NULL;
    }

    /* ── Start demux and video decode threads ── */
    ps->eof      = 0;
    ps->io_error = 0;
    ps->res_change_logged = 0;
    ps->cache_fail_logged = 0;
    ps->playing  = 1;
    ps->paused   = 0;
    ps->demux_thread        = SDL_CreateThread(demux_thread_func,        "demux",        ps);
    ps->video_decode_thread = SDL_CreateThread(video_decode_thread_func, "video-decode", ps);
    if (!ps->demux_thread || !ps->video_decode_thread) {
        /* Unchecked, a failed thread creation yielded playing=1 with a
         * permanently black player and no log. player_close handles
         * NULL thread handles. */
        log_msg("ERROR: playback thread creation failed: %s", SDL_GetError());
        player_close(ps);
        ps->quit = 0;   /* player_close sets it; this is a failed open,
                           not an app exit */
        return -1;
    }

    /* Build media info string */
    /* Chapters: the container's own segments (MKV chapters, MP4 chapter
     * tracks); AVChapter start is in the chapter's time_base. Bounded;
     * titles from the "title" tag, else numbered. */
    ps->nb_chapters = 0;
    ps->chapter_nav_idx = -1;
    for (unsigned ci = 0; ci < ps->fmt_ctx->nb_chapters && ps->nb_chapters < DSVP_MAX_CHAPTERS; ci++) {
        AVChapter *ch = ps->fmt_ctx->chapters[ci];
        double st = (double)ch->start * av_q2d(ch->time_base);
        if (st < 0.0) st = 0.0;
        int k = ps->nb_chapters;
        ps->chapter_start[k] = st;
        AVDictionaryEntry *te = av_dict_get(ch->metadata, "title", NULL, 0);
        if (te && te->value && te->value[0])
            snprintf(ps->chapter_title[k], sizeof(ps->chapter_title[k]), "%s", te->value);
        else
            snprintf(ps->chapter_title[k], sizeof(ps->chapter_title[k]), "Chapter %d", k + 1);
        ps->nb_chapters++;
    }
    if (ps->nb_chapters > 0)
        log_msg("Chapters: %d (first '%s' at %.1f s, last '%s' at %.1f s)",
                ps->nb_chapters, ps->chapter_title[0], ps->chapter_start[0],
                ps->chapter_title[ps->nb_chapters - 1],
                ps->chapter_start[ps->nb_chapters - 1]);

    player_build_media_info(ps);

    return 0;
}

/* Close playback: stop threads, free all resources. */
void player_close(PlayerState *ps) {
    ps->nb_chapters = 0;   /* per-file; the next open reloads */
    if (!ps->playing && !ps->fmt_ctx) return;
    log_msg("player_close: stopping playback");

    /* Give the desktop back before teardown (deck lineage). */
    ps->hdr_pass_content = 0;
    hdr_output_apply(ps);

    /* ── Playback diagnostics summary ── */
    if (ps->diag_frames_decoded > 0) {
        double drop_pct = (ps->diag_frames_decoded > 0)
            ? (100.0 * ps->diag_frames_dropped / ps->diag_frames_decoded)
            : 0.0;
        log_msg("DIAG: === Playback Summary ===");
        log_msg("DIAG:   Frames decoded:   %d", ps->diag_frames_decoded);
        log_msg("DIAG:   Frames displayed:  %d", ps->diag_frames_displayed);
        log_msg("DIAG:   Frames dropped:    %d (%.2f%%)",
                ps->diag_frames_dropped, drop_pct);
        log_msg("DIAG:   Multi-decode ticks: %d", ps->diag_multi_decodes);
        log_msg("DIAG:   Timer snap-forwards: %d", ps->diag_timer_snaps);
        log_msg("DIAG:   Peak A/V drift:    %.1fms",
                ps->diag_max_av_drift * 1000.0);
        log_msg("DIAG:   A/V bias:          %.1fms",
                ps->av_bias * 1000.0);
    }

    ps->quit = 1;

    /* Signal queues to unblock any waiting threads.
     *
     * abort_request MUST be set (and signaled) under each queue's mutex:
     * a waiter that had already evaluated its predicate and was between
     * "decide to wait" and "actually sleeping" would otherwise miss this
     * single signal forever — textbook lost wakeup. The frame queue is
     * usually FULL at close (main stopped consuming), which is exactly
     * the state where the decode thread sits in fq_put's wait loop, so
     * the race window was live on every close. */
    SDL_LockMutex(ps->video_pq.mutex);
    ps->video_pq.abort_request = 1;
    SDL_SignalCondition(ps->video_pq.cond);
    SDL_UnlockMutex(ps->video_pq.mutex);

    SDL_LockMutex(ps->audio_pq.mutex);
    ps->audio_pq.abort_request = 1;
    SDL_SignalCondition(ps->audio_pq.cond);
    SDL_UnlockMutex(ps->audio_pq.mutex);

    SDL_LockMutex(ps->video_frame_q.mutex);
    ps->video_frame_q.abort_request = 1;
    SDL_SignalCondition(ps->video_frame_q.cond);
    SDL_UnlockMutex(ps->video_frame_q.mutex);

    /* Wait for video decode thread first — it consumes the packet queue,
     * so it must exit before the demux thread tears that queue down. */
    if (ps->video_decode_thread) {
        SDL_WaitThread(ps->video_decode_thread, NULL);
        ps->video_decode_thread = NULL;
    }

    /* Wait for demux thread */
    if (ps->demux_thread) {
        SDL_WaitThread(ps->demux_thread, NULL);
        ps->demux_thread = NULL;
    }

    /* Close audio */
    audio_close(ps);

    /* Close subtitles */
    sub_close_codec(ps);
    sub_ass_close_file(ps);   /* per-file libass library + renderer (embedded fonts) */

    /* Flush queues */
    pq_destroy(&ps->video_pq);
    pq_destroy(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_destroy(&ps->sub_pqs[i]);
    fq_destroy(&ps->video_frame_q);

    /* Destroy seek mutex */
    if (ps->seek_mutex) { SDL_DestroyMutex(ps->seek_mutex); ps->seek_mutex = NULL; }

    /* Free frames */
    if (ps->video_frame)  av_frame_free(&ps->video_frame);
    if (ps->rgb_frame)    av_frame_free(&ps->rgb_frame);
    if (ps->audio_frame)  av_frame_free(&ps->audio_frame);

    /* Free buffers */
    if (ps->rgb_buffer)    { av_free(ps->rgb_buffer);    ps->rgb_buffer    = NULL; }
    if (ps->audio_buf)     { av_free(ps->audio_buf);     ps->audio_buf     = NULL; ps->audio_buf_cap = 0; }

    /* Free scale/resample contexts */
    if (ps->sws_ctx)      { sws_freeContext(ps->sws_ctx); ps->sws_ctx = NULL; }
    ps->sws_out_10bit = 0;
    deint_graph_free(ps);   /* decode thread is already joined */
    if (ps->swr_ctx)      { swr_free(&ps->swr_ctx); ps->swr_ctx = NULL; }
    av_channel_layout_uninit(&ps->swr_in_layout);

    /* Free codecs */
    if (ps->video_codec_ctx) avcodec_free_context(&ps->video_codec_ctx);
    if (ps->audio_codec_ctx) avcodec_free_context(&ps->audio_codec_ctx);

    /* Close format */
    if (ps->fmt_ctx) avformat_close_input(&ps->fmt_ctx);

    /* ── Destroy GPU video textures and transfer buffers ── */
    gpu_destroy_video_textures(ps);

    /* Reset state */
    ps->playing            = 0;
    ps->paused             = 0;
    ps->eof                = 0;
    ps->quit               = 0;
    ps->video_stream_idx   = -1;
    ps->audio_stream_idx   = -1;
    ps->audio_buf_size     = 0;
    ps->audio_buf_index    = 0;
    ps->seek_request       = 0;
    ps->seeking            = 0;
    ps->seek_recovering    = 0;
    ps->seek_recovering_start = 0.0;
    ps->seekdiag_vid_pending = 0;
    ps->seekdiag_aud_pending = 0;
    ps->seekdiag_target_valid = 0;
    ps->audio_pts_floor    = 0.0;
    ps->video_ready        = 0;
    ps->show_debug         = 0;
    ps->show_info          = 0;
    ps->show_seekbar       = 0;
    ps->seekbar_hide_time  = 0.0;
    ps->overlay_active     = 0;
    ps->aud_count          = 0;
    ps->aud_selection      = 0;
    ps->aud_osd[0]         = '\0';
    ps->sub_count          = 0;
    ps->sub_selection      = 0;
    ps->sub_active_idx     = -1;
    sub_clear_display(ps);   /* cues, bitmaps, text, valid */
    ps->sub_is_bitmap      = 0;
    ps->sub_osd[0]         = '\0';

    /* Reset window (skip resize if fullscreen — actual size is monitor) */
    SDL_SetWindowTitle(ps->window, DSVP_WINDOW_TITLE);
    if (!ps->fullscreen) {
        SDL_SetWindowSize(ps->window, DEFAULT_WIN_W, DEFAULT_WIN_H);
        SDL_SetWindowPosition(ps->window,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Video Decode Thread
 * ═══════════════════════════════════════════════════════════════════
 *
 * Continuously pulls packets from video_pq, feeds the decoder, and
 * pushes finished AVFrames into video_frame_q. Blocks naturally when
 * the frame queue is full (fq_put block=1) — this is the throttle
 * that keeps decode tracking display rate without unbounded RAM use.
 *
 * Decode work that previously ran inline in the display thread now
 * happens here. The display thread consumes via video_decode_frame,
 * which is a non-blocking fq_get + av_frame_move_ref into the
 * existing persistent ps->video_frame container. video_display sees
 * the same ps->video_frame it always did.
 *
 * Seek handling: when ps->seeking is set, this thread skips work
 * (the demux thread holds seek_mutex during flush, and our codec API
 * calls would race the flush otherwise). We also hold seek_mutex
 * briefly around avcodec_receive_frame / avcodec_send_packet, the
 * same protection the inline decode used to take.
 *
 * Shutdown: ps->quit + abort_request on both queues wakes any
 * blocked waits. player_close joins this thread before the demux
 * thread to avoid races with packet-queue teardown.
 */

/* ── Content-aware deinterlacing (bwdif) ──
 *
 * Created lazily when a decoded frame arrives flagged interlaced;
 * deint=interlaced means progressive frames pass through untouched, so
 * mixed streams are correct per frame. Freed on seek (demux thread,
 * under seek_mutex — bwdif carries temporal field state that must not
 * span a discontinuity) and lazily recreated. */

void deint_graph_free(PlayerState *ps) {
    if (ps->deint_graph) {
        avfilter_graph_free(&ps->deint_graph);
        ps->deint_src  = NULL;
        ps->deint_sink = NULL;
    }
}

static int deint_graph_create(PlayerState *ps, const AVFrame *frame) {
    AVFilterInOut *inputs = NULL, *outputs = NULL;
    ps->deint_graph = avfilter_graph_alloc();
    if (!ps->deint_graph) return -1;

    AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
    char args[256];
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        frame->width, frame->height, frame->format,
        vs->time_base.num, vs->time_base.den,
        frame->sample_aspect_ratio.num,
        frame->sample_aspect_ratio.den > 0
            ? frame->sample_aspect_ratio.den : 1);

    int ret = avfilter_graph_create_filter(&ps->deint_src,
        avfilter_get_by_name("buffer"), "in", args, NULL, ps->deint_graph);
    if (ret >= 0)
        ret = avfilter_graph_create_filter(&ps->deint_sink,
            avfilter_get_by_name("buffersink"), "out", NULL, NULL,
            ps->deint_graph);

    if (ret >= 0) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) ret = AVERROR(ENOMEM);
    }
    if (ret >= 0) {
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = ps->deint_src;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;
        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = ps->deint_sink;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;
        /* send_frame: one output per input (no field-rate doubling —
         * pacing and A/V sync stay untouched); deint=interlaced: only
         * frames flagged interlaced are filtered. */
        ret = avfilter_graph_parse_ptr(ps->deint_graph,
            "bwdif=mode=send_frame:parity=auto:deint=interlaced",
            &inputs, &outputs, NULL);
    }
    if (ret >= 0)
        ret = avfilter_graph_config(ps->deint_graph, NULL);

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        log_msg("ERROR: deinterlace graph failed (%s) — playing as-is",
                av_err2str(ret));
        deint_graph_free(ps);
        return -1;
    }
    log_msg("Deinterlace: bwdif active (%dx%d, send_frame, deint=interlaced)",
            frame->width, frame->height);
    return 0;
}

int video_decode_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;

    /* Audio-only files have no video codec — nothing to decode. */
    if (!ps->video_codec_ctx) {
        log_msg("Video decode thread: no video stream, exiting");
        return 0;
    }

    log_msg("Video decode thread started");

    int hard_err_streak = 0;  /* consecutive non-EAGAIN receive errors */
    int drain_sent      = 0;  /* NULL packet sent for EOF drain */
    int filter_drained  = 0;  /* deint graph flushed at EOF */

    /* Reusable frame container. avcodec_receive_frame() unrefs the frame
     * on entry, so a frame that was NOT handed to the queue can be reused
     * directly instead of paying an alloc/free on every loop iteration
     * (including every EAGAIN poll). A fresh alloc only happens after a
     * successful handoff — the queue owns the previous one then. */
    AVFrame *frame = NULL;

    while (!ps->quit) {
        /* Skip while seek is flushing the codec */
        if (ps->seeking) {
            SDL_Delay(2);
            continue;
        }

        if (!frame) {
            frame = av_frame_alloc();
            if (!frame) {
                SDL_Delay(10);
                continue;
            }
        }

        /* Brief codec-API mutex protects against demux's flush.
         * Try-lock so we don't deadlock if demux is mid-seek. */
        if (!SDL_TryLockMutex(ps->seek_mutex)) {
            SDL_Delay(2);
            continue;
        }

#ifdef DSVP_PROFILE
        double t_dec0 = get_time_sec();
#endif
        int ret = avcodec_receive_frame(ps->video_codec_ctx, frame);

        if (ret == 0) {
            hard_err_streak = 0;

            /* ── Content-aware deinterlacing ── (still under seek_mutex:
             * the demux seek path frees the graph under the same lock) */
            if (!ps->deint_disabled && !ps->deint_graph
                    && (ps->deint_force
                        || (frame->flags & AV_FRAME_FLAG_INTERLACED))) {
                if (deint_graph_create(ps, frame) < 0)
                    ps->deint_disabled = 1;
            }
            if (ps->deint_graph) {
                /* add_frame moves the refs out of our container */
                if (av_buffersrc_add_frame(ps->deint_src, frame) < 0) {
                    log_msg("ERROR: deinterlace feed failed — disabling");
                    deint_graph_free(ps);
                    ps->deint_disabled = 1;
                    av_frame_unref(frame);
                    SDL_UnlockMutex(ps->seek_mutex);
                    continue;
                }
                int fret = av_buffersink_get_frame(ps->deint_sink, frame);
                if (fret < 0) {
                    /* EAGAIN: bwdif is priming (needs a following frame
                     * for the temporal taps) — nothing to queue yet. */
                    SDL_UnlockMutex(ps->seek_mutex);
                    continue;
                }
            }

#ifdef DSVP_PROFILE
            {
                /* Frame production cost: receive + deint. Excludes the
                 * fq_put below — a full queue is backpressure, not
                 * decode time. */
                double dec_ms = (get_time_sec() - t_dec0) * 1000.0;
                ps->prof_dec_n++;
                ps->prof_sum_decode += dec_ms;
                if (dec_ms > ps->prof_max_decode)
                    ps->prof_max_decode = dec_ms;
                /* Spike: decode nearing the content frame budget
                 * (80% of period; 24p→33ms, 60fps→13ms). */
                double dec_thr = ps->frame_last_delay > 0.001
                    ? ps->frame_last_delay * 800.0 : 13.0;
                if (dec_ms > dec_thr)
                    log_msg("PROF DECODE SPIKE: %.1fms (frame %d)",
                            dec_ms, ps->diag_frames_decoded);
            }
#endif

            /* Capture the queue serial while still under seek_mutex — a
             * seek cannot run concurrently here, so this frame provably
             * belongs to the current (pre- or post-flush) epoch. fq_put
             * compares against it after reacquiring the queue mutex. */
            int serial = ps->video_frame_q.flush_serial;
            /* Release codec mutex BEFORE fq_put — a full queue would
             * otherwise pin seek_mutex and deadlock the demux thread
             * mid-seek. */
            SDL_UnlockMutex(ps->seek_mutex);

            int pret = fq_put(&ps->video_frame_q, frame, 1, serial);
            if (pret == 0) {
                /* Queue owns the frame now — alloc a new container next loop */
                frame = NULL;
                continue;
            }
            if (pret == -1) {
                /* -1 is EITHER shutdown abort or a FrameNode alloc
                 * failure. Treating OOM as abort silently killed the
                 * decode thread on one transient malloc failure —
                 * frozen video with no log and no escape. Escalate
                 * real OOM to io_error so main.c tears down cleanly. */
                if (!ps->quit && !ps->video_frame_q.abort_request) {
                    log_msg("ERROR: frame queue allocation failed — ending playback");
                    ps->io_error = 1;
                }
                av_frame_free(&frame);
                frame = NULL;
                break;
            }
            /* pret == -2: queue was flushed during our wait (seek) — the
             * frame is stale. Recycle the container, keep producing. */
            av_frame_unref(frame);
            continue;
        }

        if (ret == AVERROR(EAGAIN)) {
            hard_err_streak = 0;
            /* Decoder needs more input. Pull one packet non-blocking
             * (demux is on its own pacing); if none, brief sleep. */
            AVPacket pkt;
            int pret = pq_get(&ps->video_pq, &pkt, 0);
            if (pret > 0) {
                if (avcodec_send_packet(ps->video_codec_ctx, &pkt) < 0)
                    log_msg("WARN: avcodec_send_packet (video) rejected a packet");
                av_packet_unref(&pkt);
                drain_sent = 0;
                filter_drained = 0;
            } else if (pret == 0 && ps->eof && !drain_sent) {
                /* Demux hit end of file and the packet queue is dry.
                 * With frame-threading the decoder withholds up to
                 * ~thread_count frames (plus B-frame reorder delay) that
                 * only a NULL-packet drain flushes — without this, every
                 * file ended ~0.3-0.5s early and the final frames
                 * (fade-outs, closing cards) were never shown. */
                avcodec_send_packet(ps->video_codec_ctx, NULL);
                drain_sent = 1;
                log_msg("Video decode: EOF drain issued");
            }
            SDL_UnlockMutex(ps->seek_mutex);

            if (pret == 0) SDL_Delay(2);
            continue;
        }

        if (ret == AVERROR_EOF) {
            /* Codec fully drained (post-NULL-packet). Flush the
             * deinterlacer's temporal taps too — bwdif holds the last
             * frame(s) until its own EOF. A seek clears eof and frees
             * the graph; packets flowing again reset both flags. */
            if (ps->deint_graph && !filter_drained) {
                if (av_buffersrc_add_frame(ps->deint_src, NULL) < 0)
                    log_msg("WARN: deinterlace EOF flush failed");
                filter_drained = 1;
            }
            if (ps->deint_graph
                    && av_buffersink_get_frame(ps->deint_sink, frame) == 0) {
                int serial = ps->video_frame_q.flush_serial;
                SDL_UnlockMutex(ps->seek_mutex);
                int pret = fq_put(&ps->video_frame_q, frame, 1, serial);
                if (pret == 0) {
                    frame = NULL;
                } else if (pret == -1) {
                    /* shutdown abort vs OOM — same distinction as the
                     * main put site above */
                    if (!ps->quit && !ps->video_frame_q.abort_request) {
                        log_msg("ERROR: frame queue allocation failed — ending playback");
                        ps->io_error = 1;
                    }
                    av_frame_free(&frame);
                    frame = NULL;
                    break;
                } else {
                    av_frame_unref(frame);
                }
                continue;
            }
            SDL_UnlockMutex(ps->seek_mutex);
            SDL_Delay(20);
            continue;
        }

        SDL_UnlockMutex(ps->seek_mutex);

        /* Hard decoder error. A persistent one used to be retried forever
         * at 50ms intervals — frozen picture, log spam, no escape short of
         * Q. Escalate to io_error, which main.c tears down cleanly. */
        log_msg("ERROR: avcodec_receive_frame (video) failed: %s",
                av_err2str(ret));
        if (++hard_err_streak >= 30) {
            log_msg("ERROR: video decode failing persistently — ending playback");
            ps->io_error = 1;
            break;
        }
        SDL_Delay(50);
    }

    if (frame) av_frame_free(&frame);

    log_msg("Video decode thread exiting");
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Demux Thread
 * ═══════════════════════════════════════════════════════════════════
 *
 * Reads packets from the container file and distributes them to
 * the video and audio packet queues.
 */

int demux_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;
    AVPacket *pkt = av_packet_alloc();
    int input_dead = 0;   /* unrecoverable read error — stay alive to
                             service (reject) seeks; see error path */
    log_msg("Demux thread started");

    while (!ps->quit) {
        /* ── Dead input: the demuxer hit an unrecoverable error.
         * The old code exited the thread here, which wedged the whole
         * player on the user's NEXT action: a seek set seek_request=1
         * that nothing would ever service, and the audio callback
         * gates on that flag — audio died, audio_pq never drained,
         * and the EOF close (audio_pq == 0) was blocked forever.
         * Stay alive; reject seeks cleanly; let EOF drain-out finish. */
        if (input_dead) {
            if (ps->seek_request) {
                log_msg("Demux: seek ignored — input is in error state");
                ps->seek_request = 0;
            }
            SDL_Delay(100);
            continue;
        }

        /* ── Handle seek requests ── */
        if (ps->seek_request) {
            int64_t target = ps->seek_target;
            int     tflags = ps->seek_flags;
            /* m-S1-c: the recovery gate and every SEEKDIAG line read
             * THIS value, not the live seek_target — a second request
             * issued during service overwrote the live one and the
             * 2026-08-29 double-seek printed seek#1's landing against
             * seek#2's target ('delta=-640', field 2026-09-07). */
            ps->seekdiag_target = target;
            /* Re-arm IMMEDIATELY: clearing only after the full
             * seek+flush swallowed any second seek issued during
             * service (double-tap on slow media; audio_cycle's
             * recovery seek). The audio callback stays gated via
             * ps->seeking for the whole window. */
            ps->seek_request = 0;
            log_msg("Demux: seeking to %.3f s (flags=%s)",
                    (double)target / AV_TIME_BASE,
                    (tflags & AVSEEK_FLAG_BACKWARD) ? "backward"
                                                    : "default");

            /* CRITICAL: Lock the seek mutex. This prevents the main thread
             * from calling avcodec_send_packet/receive_frame on the video
             * codec while we flush it. The audio callback is also paused. */
            SDL_LockMutex(ps->seek_mutex);
            ps->seeking = 1;

            /* Pause audio device so callback can't touch audio codec.
             * Pause stops FUTURE callbacks but does NOT wait for one
             * already in flight. SDL runs the get-callback while holding
             * the stream lock, so lock+unlock acts as a barrier — it
             * returns only after any in-flight audio_decode_frame() has
             * finished. Only then is the codec safe to flush. */
            if (ps->audio_stream) {
                SDL_PauseAudioStreamDevice(ps->audio_stream);
                SDL_LockAudioStream(ps->audio_stream);
                SDL_UnlockAudioStream(ps->audio_stream);
            }

            int ret = av_seek_frame(ps->fmt_ctx, -1, target, tflags);
            if (ret < 0) {
                /* Nothing moved — playback continues from the old
                 * position. The old code still rewrote both clocks to the
                 * unreached target, cleared EOF, flushed the SDL audio
                 * queue, and armed seek-recovery: an audible gap and a
                 * transient sync scramble for a seek that never happened. */
                log_msg("ERROR: Seek failed: %s — position unchanged (the frame timer was reset at request)",
                        av_err2str(ret));
                ps->seeking = 0;
                /* Resume under the mutex: audio_cycle and the
                 * device-removed handler destroy/replace audio_stream
                 * under this same lock — touching it after unlock was
                 * a NULL-check-then-use race on a freeable pointer. */
                if (ps->audio_stream && !ps->paused)
                    SDL_ResumeAudioStreamDevice(ps->audio_stream);
                SDL_UnlockMutex(ps->seek_mutex);
                continue;
            }
            {
                log_msg("Demux: av_seek_frame OK, flushing queues");
                pq_flush(&ps->video_pq);
                pq_flush(&ps->audio_pq);
                for (int i = 0; i < ps->sub_count; i++)
                    pq_flush(&ps->sub_pqs[i]);
                fq_flush(&ps->video_frame_q);
                log_msg("Demux: queues flushed, flushing video codec");
                if (ps->video_codec_ctx)
                    avcodec_flush_buffers(ps->video_codec_ctx);
                log_msg("Demux: video codec flushed, flushing audio codec");
                if (ps->audio_codec_ctx)
                    avcodec_flush_buffers(ps->audio_codec_ctx);
                /* Drop the resampler's FIR history too — it convolves a
                 * few ms of pre-seek samples into the first post-seek
                 * frame otherwise. Safe here: seek_mutex held, callback
                 * paused + barriered. Lazily rebuilt on the next frame. */
                if (ps->swr_ctx)
                    swr_free(&ps->swr_ctx);
                /* And the deinterlacer — bwdif's temporal field state
                 * must not span the discontinuity. Lazily recreated by
                 * the decode thread (which only touches the graph under
                 * this same mutex). */
                deint_graph_free(ps);
                if (ps->sub_codec_ctx)
                    avcodec_flush_buffers(ps->sub_codec_ctx);
                /* Not sub_clear_display(): this runs on the demux thread
                 * and the main thread may be uploading the bitmap rects —
                 * freeing them here would race. Drop what is safe to drop;
                 * the main-thread decode path frees bitmaps on its next
                 * call. The active cue set (M4) is dropped with the text. */
                ps->sub_valid = 0;
                ps->sub_text[0] = '\0';
                ps->sub_cue_count = 0;
                log_msg("Demux: all codecs flushed");
                /* ── Drain flush survivors (deck 8da58aa port) ──
                 * A frame already COMPLETE inside the decoder can
                 * survive avcodec_flush_buffers: the deck's raw log
                 * showed receive_frame returning a fully-decoded
                 * pre-seek 4K frame in the SAME millisecond as "all
                 * codecs flushed". Received after the seek, it earns
                 * the NEW queue serial — the serial gate at the
                 * recovery clear is structurally blind to it — and on
                 * the deck it anchored recovery 2284s above a
                 * backward target (the floor then discarded all
                 * replay audio: the poisoned-timer death spiral).
                 * Drain both codecs to EAGAIN here, same mutex hold
                 * (decode thread locked out, audio callback paused),
                 * so nothing decoded from pre-seek input remains
                 * deliverable. Async stragglers are caught by the
                 * backward-landing veto at the clear (main.c), which
                 * fails open via the existing 2s recovery timeout. */
                {
                    AVFrame *scratch = av_frame_alloc();
                    if (scratch) {
                        int dv = 0, da = 0;
                        if (ps->video_codec_ctx)
                            while (dv < 16 && avcodec_receive_frame(
                                       ps->video_codec_ctx, scratch) == 0) {
                                av_frame_unref(scratch);
                                dv++;
                            }
                        if (ps->audio_codec_ctx)
                            while (da < 16 && avcodec_receive_frame(
                                       ps->audio_codec_ctx, scratch) == 0) {
                                av_frame_unref(scratch);
                                da++;
                            }
                        av_frame_free(&scratch);
                        if (dv || da)
                            log_msg("SEEKDIAG: drained flush survivors "
                                    "(video=%d audio=%d)", dv, da);
                    }
                }
            }
            ps->eof = 0;

            /* Reset audio decode buffer (safe — callback is paused) */
            ps->audio_buf_size  = 0;
            ps->audio_buf_index = 0;

            /* Reset both clocks to the seek target. Without this,
             * video_clock retains the old position until the first
             * frame is decoded, causing a phantom drift spike equal
             * to the entire seek distance (e.g. 895 seconds). */
            {
                double seek_pos = (double)target / AV_TIME_BASE;
                ps->audio_clock = seek_pos;
                ps->audio_clock_sync = seek_pos;
                ps->video_clock = seek_pos;
            }

            /* Suppress frame drops until the first frame is displayed
             * post-seek. Adapts to any codec — H.264 recovers in
             * ~100ms, HEVC with long GOPs may take 5–10 seconds.
             * Cleared in main.c when a frame is actually shown. */
            ps->seek_recovering = 1;
            ps->seek_recovering_start = get_time_sec();
            /* SEEKDIAG one-shots (deck 20325d1 port): first decoded
             * video/audio PTS post-seek, logged against the target —
             * where the demuxer actually put each stream is the fact
             * every recovery theory has to survive. */
            ps->seekdiag_vid_pending = 1;
            ps->seekdiag_aud_pending = 1;
            ps->seekdiag_target_valid = 1;
            /* av_bias is NOT reset here. It models the systematic latency
             * of the audio output path (SDL + device buffering that
             * audio_clock_sync under-counts) — a property of the output
             * path, not of playback position; identical either side of a
             * seek. Zeroing it made the steady-state offset be measured
             * raw again after every seek, crossing the drop threshold and
             * discarding 2-5 good frames until the EMA re-converged: the
             * entire post-seek "frame dropped ... A/V drift" burst. */

            /* Flush any pre-seek audio still queued in SDL pipeline.
             * Audio stays PAUSED here until the first video frame is
             * displayed (cleared in main.c's seek_recovering branch).
             * Resuming audio before the first frame decodes on heavy
             * content (4K HEVC HDR keyframe takes ~300–400ms) causes
             * audio_clock to run that far ahead, then snap-forwards
             * and frame drops cascade as video catches up. The deck
             * branch hit this with VAAPI's DPB rebuild
             * (commit b3177ba in DSVP-deck, Mar 24, 2026); the same
             * mechanism applies to software HEVC decode on lower-spec
             * hosts. Pause+resume must be paired: demux thread pauses
             * (above), main thread resumes on first-frame display.
             *
             * Audio-only files have no video frame to wait for; the
             * timeout in main.c (seek_recovering_start + 2s, or
             * immediate when video_stream_idx<0) forces resume.
             *
             * Cleared under seek_mutex: audio_cycle / device-removed
             * destroy audio_stream under this lock, so touching it
             * after unlock was a use-after-free window. */
            if (ps->audio_stream)
                SDL_ClearAudioStream(ps->audio_stream);

            ps->seeking = 0;
            SDL_UnlockMutex(ps->seek_mutex);

            log_msg("Demux: seek complete");
        }

        /* ── Throttle if queues are full ── */
        if (ps->video_pq.nb_packets > PACKET_QUEUE_MAX ||
            ps->audio_pq.nb_packets > PACKET_QUEUE_MAX) {
            SDL_Delay(10);
            continue;
        }

        /* ── Read next packet ── */
        int ret = av_read_frame(ps->fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ps->fmt_ctx->pb)) {
                if (!ps->eof) log_msg("Demux: reached end of file");
                ps->eof = 1;
                SDL_Delay(100);
                continue;
            }
            /* Real mid-file error (I/O error, truncated container, media
             * removed). Without eof set, main.c's close condition never
             * fired: the last frame reblitted forever, silent, no escape
             * short of pressing Q. Setting eof lets the queues drain and
             * playback end cleanly. */
            log_msg("ERROR: av_read_frame failed: %s — ending playback",
                    av_err2str(ret));
            ps->eof = 1;
            input_dead = 1;
            continue;
        }

        /* Route packet to the correct queue. pq_put takes ownership only
         * on success — an ignored failure leaked the payload ref (the next
         * av_read_frame overwrites the packet). */
        if (pkt->stream_index == ps->video_stream_idx) {
            if (pq_put(&ps->video_pq, pkt) < 0) av_packet_unref(pkt);
        } else if (pkt->stream_index == ps->audio_stream_idx) {
            if (pq_put(&ps->audio_pq, pkt) < 0) av_packet_unref(pkt);
        } else {
            /* Subtitle streams: EVERY track is queued, pruned to a
             * rolling window just behind the playback clock.
             *
             * Selected-only queueing (tried first) made S feel broken:
             * the fresh queue only fills from the demux read position,
             * which runs ~10s ahead of playback, so the packets covering
             * the moment on screen were already read and discarded —
             * subtitles appeared only once playback caught up to the
             * press-time read position. Queueing everything unpruned
             * (the original code) grew unbounded for the life of the
             * file on multi-track remuxes. The rolling window keeps
             * switching instant AND memory bounded (~35s of sub packets
             * per track, a few hundred KB even for PGS). The 35s floor
             * tracks the decoder's 30s stale-skip so nothing prunable
             * would have been decoded anyway. */
            int routed = 0;
            for (int i = 0; i < ps->sub_count; i++) {
                if (pkt->stream_index == ps->sub_stream_indices[i]) {
                    if (pq_put(&ps->sub_pqs[i], pkt) < 0)
                        av_packet_unref(pkt);
                    double now = (ps->audio_stream_idx >= 0)
                                 ? ps->audio_clock_sync : ps->video_clock;
                    AVStream *st =
                        ps->fmt_ctx->streams[ps->sub_stream_indices[i]];
                    double tb = av_q2d(st->time_base);
                    if (now > 35.0 && tb > 0.0)
                        pq_prune_stale(&ps->sub_pqs[i],
                                       (int64_t)((now - 35.0) / tb));
                    routed = 1;
                    break;
                }
            }
            if (!routed) {
                av_packet_unref(pkt);
            }
        }
    }

    av_packet_free(&pkt);
    log_msg("Demux thread exiting");
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Video Decode & Display
 * ═══════════════════════════════════════════════════════════════════ */

/* Retrieve the next decoded frame for display.
 *
 * Decode work itself runs in video_decode_thread_func. This function
 * is a non-blocking pop from the frame queue; the popped AVFrame's
 * data is moved into the persistent ps->video_frame container so
 * the existing video_display path reads it without change.
 *
 * Returns 1 if a frame is now ready in ps->video_frame, 0 if the
 * frame queue is currently empty (decoder hasn't produced one yet —
 * caller should re-blit the previous frame). Never returns -1 from
 * this path; decoder errors are handled inside the decode thread. */
int video_decode_frame(PlayerState *ps) {
    if (ps->seeking) return 0;

    AVFrame *queued = NULL;
    int ret = fq_get(&ps->video_frame_q, &queued, 0);
    if (ret <= 0 || !queued) return 0;

    /* Move the queued frame's buffers into ps->video_frame.
     * av_frame_unref releases the previous frame's refcounted data
     * (the decoder no longer needs it, display has already uploaded
     * to GPU); av_frame_move_ref transfers ownership without copying.
     * The now-empty queued container is freed. */
    av_frame_unref(ps->video_frame);
    av_frame_move_ref(ps->video_frame, queued);
    av_frame_free(&queued);
    /* Same-thread read of the value fq_get just stored under the queue
     * mutex — only main pops this queue. */
    ps->video_frame_serial = ps->video_frame_q.last_pop_serial;

    /* Update video clock from new frame's PTS in seconds.
     * best_effort_timestamp is preferred: FFmpeg computes it from
     * DTS/packet timing even when the codec doesn't set frame->pts
     * (required for VC-1, some MPEG-2, etc.). */
    AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
    int64_t frame_pts = ps->video_frame->best_effort_timestamp;
    if (frame_pts == AV_NOPTS_VALUE)
        frame_pts = ps->video_frame->pts;
    if (frame_pts != AV_NOPTS_VALUE) {
        ps->video_clock = (double)frame_pts * av_q2d(vs->time_base);
    }
    /* A PTS-less frame (broken-index VC-1/MPEG-2, raw elementary streams)
     * used to timestamp itself 0.0, jumping the clock to file start for
     * one frame: phantom multi-second A/V drift, a spurious drop, and a
     * poisoned EMA. Leave the clock at the previous frame's value —
     * off by one frame duration at most, self-correcting. (Post-seek
     * that previous value is the demux preset = the target — the same
     * base the deck's 9832c24 fix converges on.) */

    /* SEEKDIAG one-shot (deck 20325d1 port): first genuinely post-seek
     * frame only — a stale pre-seek frame popped mid-straddle carries
     * the old serial and must not impersonate the landing. */
    if (ps->seekdiag_vid_pending
            && ps->video_frame_serial == ps->video_frame_q.flush_serial) {
        ps->seekdiag_vid_pending = 0;
        double tgt = (double)ps->seekdiag_target / AV_TIME_BASE;
        if (frame_pts != AV_NOPTS_VALUE)
            log_msg("SEEKDIAG: first video frame post-seek "
                    "pts=%.3f target=%.3f delta=%+.3f",
                    ps->video_clock, tgt, ps->video_clock - tgt);
        else
            log_msg("SEEKDIAG: first video frame post-seek "
                    "pts=NOPTS (clock held %.3f) target=%.3f",
                    ps->video_clock, tgt);
    }

    return 1;
}

/* Compute the letterboxed display rectangle for the video.
 * Maintains aspect ratio within the current window, centering with
 * black bars on the shorter axis. Call after window resize or video open. */
void player_update_display_rect(PlayerState *ps) {
    if (ps->vid_w <= 0 || ps->vid_h <= 0 || ps->win_w <= 0 || ps->win_h <= 0) {
        ps->display_rect = (SDL_Rect){ 0, 0, ps->win_w, ps->win_h };
        return;
    }

    double video_aspect = (double)ps->vid_w / ps->vid_h;
    double win_aspect   = (double)ps->win_w / ps->win_h;

    int disp_w, disp_h;
    if (video_aspect > win_aspect) {
        /* Video is wider than window — pillarbox (bars top/bottom) */
        disp_w = ps->win_w;
        disp_h = (int)(ps->win_w / video_aspect);
    } else {
        /* Video is taller than window — letterbox (bars left/right) */
        disp_h = ps->win_h;
        disp_w = (int)(ps->win_h * video_aspect);
    }

    ps->display_rect.x = (ps->win_w - disp_w) / 2;
    ps->display_rect.y = (ps->win_h - disp_h) / 2;
    ps->display_rect.w = disp_w;
    ps->display_rect.h = disp_h;
}


/* ── Upload one YUV plane from AVFrame to GPU transfer buffer ──
 *
 * Handles stride mismatch two ways:
 *   1. If the stride-padded plane fits in the transfer buffer, copy it
 *      with ONE memcpy (padding included) and return the source stride
 *      as pixels_per_row — the GPU copy skips the padding for free.
 *      One large memcpy into write-combined memory beats thousands of
 *      per-row copies (≈25MB/frame at 4K 10-bit).
 *   2. Otherwise fall back to tightly-packed row-by-row copy.
 *
 * Returns the pixels_per_row value (in TEXELS, not bytes) the caller
 * must pass to SDL_UploadToGPUTexture for this plane. */
static Uint32 upload_plane(
    SDL_GPUDevice *device,
    SDL_GPUTransferBuffer *xfer, Uint32 xfer_capacity,
    const uint8_t *src, int src_stride,
    int width_bytes, int height, int bpp)
{
    Uint32 texels_per_row = (Uint32)(width_bytes / bpp);

    uint8_t *dst = SDL_MapGPUTransferBuffer(device, xfer, true);
    if (!dst) {
        /* Returning a valid ppr here made the caller upload whatever
         * the transfer buffer last held (cycle=true: undefined) with
         * zero diagnostics — silent single-plane corruption. Return 0
         * so the caller skips this plane's GPU copy (previous texture
         * content holds) and log the failure. */
        log_msg("ERROR: transfer buffer map failed: %s", SDL_GetError());
        return 0;
    }

    if ((Uint64)width_bytes * (Uint64)height > (Uint64)xfer_capacity) {
        /* Never write past the mapped buffer even if a caller-side
         * guard is bypassed (mid-stream format change class). */
        log_msg("ERROR: plane upload %dx%d exceeds transfer capacity %u "
                "— frame skipped", width_bytes, height,
                (unsigned)xfer_capacity);
        SDL_UnmapGPUTransferBuffer(device, xfer);
        return 0;
    }

    if (src_stride == width_bytes) {
        /* Tightly packed — single memcpy */
        memcpy(dst, src, (size_t)width_bytes * height);
    } else if (src_stride > width_bytes
               && (src_stride % bpp) == 0
               && (Uint64)src_stride * (Uint64)height <= (Uint64)xfer_capacity) {
        /* Stride-padded — copy padding too, still one memcpy */
        memcpy(dst, src, (size_t)src_stride * height);
        texels_per_row = (Uint32)(src_stride / bpp);
    } else {
        /* Fallback: per-row tight pack (negative/odd stride, or a
         * stride wider than the padded buffer capacity) */
        for (int row = 0; row < height; row++) {
            memcpy(dst + (size_t)row * width_bytes,
                   src + (ptrdiff_t)row * src_stride, width_bytes);
        }
    }

    SDL_UnmapGPUTransferBuffer(device, xfer);
    return texels_per_row;
}


/* ═══════════════════════════════════════════════════════════════════
 * Shaded-Frame Cache
 * ═══════════════════════════════════════════════════════════════════
 *
 * The video pass (Lanczos + Catmull-Rom + tone map ≈ 48 texture taps
 * per screen pixel) is rendered ONCE per content frame into an
 * offscreen texture sized to the swapchain. Steady-state reblit ticks
 * (VSync presents with no new content frame — most ticks for 24fps
 * content on a 60Hz display) blit the cached result with a trivial
 * 1-fetch shader instead of re-running the full filter chain.
 *
 * Output-identical by construction: frameCount only advances on new
 * frames, so reblits always re-produced the exact same shading anyway.
 * Invalidation: swapchain resize (gpu_cache_ensure), letterbox change
 * (display_rect compare in video_reblit), or HDR uniform change
 * (H/T/G key handlers in main.c clear cache_valid). */

/* Ensure the cache texture matches the swapchain dimensions.
 * Returns 0 on success; any recreation invalidates the cache. */
static int gpu_cache_ensure(PlayerState *ps, int w, int h) {
    if (!ps->gpu_device || w <= 0 || h <= 0) return -1;

    if (ps->gpu_tex_cache && ps->cache_w == w && ps->cache_h == h)
        return 0;

    if (ps->gpu_tex_cache) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_cache);
        ps->gpu_tex_cache = NULL;
    }
    ps->cache_valid = 0;

    SDL_GPUTextureCreateInfo tex_info;
    SDL_zero(tex_info);
    tex_info.type                 = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format               = SDL_GetGPUSwapchainTextureFormat(
                                        ps->gpu_device, ps->window);
    tex_info.width                = (Uint32)w;
    tex_info.height               = (Uint32)h;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels           = 1;
    tex_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER
                                  | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    ps->gpu_tex_cache = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_cache) {
        /* Latch per file: a persistent failure (VRAM exhaustion) hit
         * this every reblit tick — unbuffered log spam at vsync rate
         * while already degraded to the full-shader path. */
        if (ps->cache_fail_logged) return -1;
        ps->cache_fail_logged = 1;
        log_msg("ERROR: Failed to create frame cache texture: %s",
                SDL_GetError());
        ps->cache_w = ps->cache_h = 0;
        return -1;
    }

    ps->cache_w = w;
    ps->cache_h = h;
    log_msg("GPU: frame cache texture created (%dx%d)", w, h);
    return 0;
}

/* Bind the YUV pipeline and draw the video quad into the current pass.
 * target_w/h are the dimensions of the pass's color target (swapchain
 * or cache — both share the swapchain format and viewport math). */
static void gpu_draw_video_quad(SDL_GPURenderPass *pass,
                                SDL_GPUCommandBuffer *cmd,
                                PlayerState *ps,
                                Uint32 target_w, Uint32 target_h)
{
    SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_yuv);

    player_update_display_rect(ps);
    float scale_x = (target_w > 0) ? (float)target_w / ps->win_w : 1.0f;
    float scale_y = (target_h > 0) ? (float)target_h / ps->win_h : 1.0f;

    SDL_GPUViewport viewport;
    viewport.x = ps->display_rect.x * scale_x;
    viewport.y = ps->display_rect.y * scale_y;
    viewport.w = ps->display_rect.w * scale_x;
    viewport.h = ps->display_rect.h * scale_y;
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &viewport);

    ps->gpu_uniforms.frameCount = (float)ps->diag_frames_displayed;

    SDL_PushGPUFragmentUniformData(cmd, 0,
        &ps->gpu_uniforms, sizeof(ps->gpu_uniforms));

    /* Video planes bind NEAREST (deck d2f5351 port, review PQUALITY
     * P6a): the Lanczos/Catmull kernels do ALL filtering themselves
     * at explicit texel centers, so nearest is bit-identical to the
     * old LINEAR binding — and correctness stops depending on every
     * tap quantizing to a center. x64 has no PQ LUTs, so nothing here
     * wants LINEAR at all. */
    SDL_GPUTextureSamplerBinding bindings[4] = {
        { .texture = ps->gpu_tex_y,     .sampler = ps->gpu_sampler_nearest },
        { .texture = ps->gpu_tex_u,     .sampler = ps->gpu_sampler_nearest },
        { .texture = ps->gpu_tex_v,     .sampler = ps->gpu_sampler_nearest },
        { .texture = ps->gpu_tex_noise, .sampler = ps->gpu_sampler_nearest },
    };
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);

    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
}

/* Render the shaded video into the cache texture (own render pass,
 * inside the caller's command buffer, BEFORE the swapchain pass). */
static void gpu_render_to_cache(SDL_GPUCommandBuffer *cmd, PlayerState *ps) {
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture     = ps->gpu_tex_cache;
    ct.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    ct.load_op     = SDL_GPU_LOADOP_CLEAR;
    ct.store_op    = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    gpu_draw_video_quad(pass, cmd, ps,
                        (Uint32)ps->cache_w, (Uint32)ps->cache_h);
    SDL_EndGPURenderPass(pass);

    ps->cache_valid = 1;
    ps->cache_rect  = ps->display_rect;
}

/* Blit the cached shaded frame to the swapchain (1:1, nearest — exact). */
static void gpu_blit_cache(SDL_GPURenderPass *pass, PlayerState *ps,
                           Uint32 sc_w, Uint32 sc_h) {
    SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_blit);

    SDL_GPUViewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.w = (float)sc_w;
    viewport.h = (float)sc_h;
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &viewport);

    SDL_GPUTextureSamplerBinding binding = {
        .texture = ps->gpu_tex_cache,
        .sampler = ps->gpu_sampler_nearest   /* 1:1 texels — exact copy */
    };
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
}


/* ═══════════════════════════════════════════════════════════════════
 * HDR Dynamic Peak Detection (Layer 1 — CPU histogram scan)
 *
 * Builds a 256-bin histogram of Y plane values per frame, reads off
 * the 99.875th percentile, converts to nits via PQ EOTF, and applies
 * temporal smoothing. Using a percentile instead of max avoids
 * specular highlights (sun glints, lamp reflections) inflating the
 * peak, which would cause BT.2390 to over-compress midtones.
 *
 * 99.875th percentile (skip top 0.125%) matches the spirit of
 * libplacebo/mpv's approach (default 99.995, user-tunable).
 * We use a slightly more aggressive value to better handle older
 * film content with occasional bright hotspots.
 *
 * Layer 2 will move this to a GPU compute shader for zero CPU cost.
 * ═══════════════════════════════════════════════════════════════════ */

/* PQ EOTF (SMPTE ST 2084): PQ code value [0,1] → linear nits [0,10000].
 * Scalar version of the shader's pq_eotf() for CPU-side use. */
static float pq_eotf_scalar(float pq) {
    const float m1 = 0.1593017578125f;   /* 2610/16384 */
    const float m2 = 78.84375f;          /* 2523/32 * 128 */
    const float c1 = 0.8359375f;         /* 3424/4096 */
    const float c2 = 18.8515625f;        /* 2413/128 */
    const float c3 = 18.6875f;           /* 2392/128 */

    float Np  = powf(fmaxf(pq, 0.0f), 1.0f / m2);
    float num = fmaxf(Np - c1, 0.0f);
    float den = c2 - c3 * Np;
    return 10000.0f * powf(fmaxf(num / den, 0.0f), 1.0f / m1);
}

/* Temporal smoothing parameters.
 * Fast attack (bright → brighter): adapt quickly so highlights aren't clipped.
 * Slow decay (bright → darker): prevent flickering from fading highlights.
 * Scene cut: jump immediately on large changes. */
/* ── Dolby Vision RPU Metadata Extraction ──
 *
 * FFmpeg's HEVC decoder parses DV RPU NALs and attaches parsed metadata
 * as AV_FRAME_DATA_DOVI_METADATA side data on each decoded frame.
 * This function extracts and logs that metadata so we can understand
 * the reshaping curves and color matrices needed for shader implementation.
 *
 * DV Profile 5 stores data in IPTPQc2 color space, not standard YCbCr.
 * The RPU contains per-component piecewise polynomial (or MMR) reshaping
 * curves that transform from the encoded IPTPQc2 signal back to standard
 * PQ-encoded BT.2020 RGB, plus color matrices for the conversion chain. */
static void dovi_log_frame_metadata(PlayerState *ps, const AVFrame *frame)
{
    /* Only log once per file open (first frame with DV metadata) */
    if (ps->dovi_metadata_logged) return;

    /* Check for raw RPU buffer first (always present if DV) */
    const AVFrameSideData *rpu_sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (rpu_sd) {
        log_msg("DOVI: raw RPU buffer present (%d bytes)", rpu_sd->size);
    }

    /* Check for parsed metadata (what we actually need) */
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!sd) {
        if (rpu_sd) {
            log_msg("DOVI: WARNING — raw RPU present but parsed "
                    "AV_FRAME_DATA_DOVI_METADATA missing! "
                    "FFmpeg may not be parsing this profile.");
        }
        /* No DV metadata on this frame — not a DV file or decoder
         * doesn't expose it. Will retry next frame. */
        return;
    }

    ps->dovi_metadata_logged = 1;
    const AVDOVIMetadata *dovi = (const AVDOVIMetadata *)sd->data;

    /* ── RPU Header ── */
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(dovi);
    log_msg("DOVI RPU header: rpu_type=%u, rpu_format=%u, "
            "vdr_rpu_profile=%u, vdr_rpu_level=%u",
            hdr->rpu_type, hdr->rpu_format,
            hdr->vdr_rpu_profile, hdr->vdr_rpu_level);
    log_msg("DOVI RPU header: coef_data_type=%u, coef_log2_denom=%u, "
            "bl_video_full_range=%u, bl_bit_depth=%u, el_bit_depth=%u, "
            "vdr_bit_depth=%u",
            hdr->coef_data_type, hdr->coef_log2_denom,
            hdr->bl_video_full_range_flag, hdr->bl_bit_depth,
            hdr->el_bit_depth, hdr->vdr_bit_depth);
    log_msg("DOVI RPU header: disable_residual=%u, "
            "spatial_resampling=%u, el_spatial_resampling=%u",
            hdr->disable_residual_flag,
            hdr->spatial_resampling_filter_flag,
            hdr->el_spatial_resampling_filter_flag);

    /* ── Data Mapping (reshaping curves) ── */
    const AVDOVIDataMapping *mapping = av_dovi_get_mapping(dovi);
    log_msg("DOVI mapping: vdr_rpu_id=%u, mapping_color_space=%u, "
            "mapping_chroma_format=%u, nlq_method=%d",
            mapping->vdr_rpu_id, mapping->mapping_color_space,
            mapping->mapping_chroma_format_idc, mapping->nlq_method_idc);

    double coef_scale = (double)(1LL << hdr->coef_log2_denom);
    const char *comp_names[] = { "I/Y", "Ct/Cb", "Cp/Cr" };

    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *curve = &mapping->curves[c];
        log_msg("DOVI reshape [%s]: num_pivots=%u",
                comp_names[c], curve->num_pivots);

        /* Log pivot values */
        char pivot_str[256] = "";
        int pos = 0;
        for (int i = 0; i < curve->num_pivots && i < AV_DOVI_MAX_PIECES + 1; i++) {
            pos += snprintf(pivot_str + pos, sizeof(pivot_str) - pos,
                           "%s%u", i ? "," : "", curve->pivots[i]);
        }
        log_msg("DOVI reshape [%s]: pivots=[%s]", comp_names[c], pivot_str);

        /* Log each piece */
        int num_pieces = curve->num_pivots - 1;
        for (int p = 0; p < num_pieces && p < AV_DOVI_MAX_PIECES; p++) {
            if (curve->mapping_idc[p] == AV_DOVI_MAPPING_POLYNOMIAL) {
                int order = curve->poly_order[p];
                double c0 = (double)curve->poly_coef[p][0] / coef_scale;
                double c1 = (double)curve->poly_coef[p][1] / coef_scale;
                double c2 = (order >= 2)
                    ? (double)curve->poly_coef[p][2] / coef_scale : 0.0;
                log_msg("DOVI reshape [%s] piece %d: POLY order=%d "
                        "range=[%u,%u] coef=[%.6f, %.6f, %.6f]",
                        comp_names[c], p, order,
                        curve->pivots[p], curve->pivots[p + 1],
                        c0, c1, c2);
            } else if (curve->mapping_idc[p] == AV_DOVI_MAPPING_MMR) {
                log_msg("DOVI reshape [%s] piece %d: MMR order=%d "
                        "range=[%u,%u] constant=%.6f",
                        comp_names[c], p, curve->mmr_order[p],
                        curve->pivots[p], curve->pivots[p + 1],
                        (double)curve->mmr_constant[p] / coef_scale);
                /* Log MMR coefficient matrix for each order */
                for (int o = 0; o < curve->mmr_order[p] && o < 3; o++) {
                    log_msg("DOVI reshape [%s] piece %d: MMR[%d] "
                            "coef=[%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f]",
                            comp_names[c], p, o + 1,
                            (double)curve->mmr_coef[p][o][0] / coef_scale,
                            (double)curve->mmr_coef[p][o][1] / coef_scale,
                            (double)curve->mmr_coef[p][o][2] / coef_scale,
                            (double)curve->mmr_coef[p][o][3] / coef_scale,
                            (double)curve->mmr_coef[p][o][4] / coef_scale,
                            (double)curve->mmr_coef[p][o][5] / coef_scale,
                            (double)curve->mmr_coef[p][o][6] / coef_scale);
                }
            }
        }
    }

    /* ── NLQ parameters (if present) ── */
    if (mapping->nlq_method_idc != AV_DOVI_NLQ_NONE) {
        for (int c = 0; c < 3; c++) {
            log_msg("DOVI NLQ [%s]: offset=%u, vdr_in_max=%llu, "
                    "dz_slope=%llu, dz_threshold=%llu",
                    comp_names[c],
                    mapping->nlq[c].nlq_offset,
                    (unsigned long long)mapping->nlq[c].vdr_in_max,
                    (unsigned long long)mapping->nlq[c].linear_deadzone_slope,
                    (unsigned long long)mapping->nlq[c].linear_deadzone_threshold);
        }
    }

    /* ── Color Metadata ── */
    const AVDOVIColorMetadata *color = av_dovi_get_color(dovi);
    log_msg("DOVI color: dm_metadata_id=%u, scene_refresh=%u, "
            "signal_eotf=%u, signal_bit_depth=%u, signal_color_space=%u, "
            "signal_full_range=%u",
            color->dm_metadata_id, color->scene_refresh_flag,
            color->signal_eotf, color->signal_bit_depth,
            color->signal_color_space, color->signal_full_range_flag);
    log_msg("DOVI color: source_min_pq=%u, source_max_pq=%u, "
            "source_diagonal=%u",
            color->source_min_pq, color->source_max_pq,
            color->source_diagonal);

    /* YCC→RGB matrix (applied before PQ linearization) */
    log_msg("DOVI ycc_to_rgb_matrix:");
    for (int row = 0; row < 3; row++) {
        log_msg("  [%.6f  %.6f  %.6f]  offset=%.6f",
                av_q2d(color->ycc_to_rgb_matrix[row * 3 + 0]),
                av_q2d(color->ycc_to_rgb_matrix[row * 3 + 1]),
                av_q2d(color->ycc_to_rgb_matrix[row * 3 + 2]),
                av_q2d(color->ycc_to_rgb_offset[row]));
    }

    /* RGB→LMS matrix (applied after PQ linearization) */
    log_msg("DOVI rgb_to_lms_matrix:");
    for (int row = 0; row < 3; row++) {
        log_msg("  [%.6f  %.6f  %.6f]",
                av_q2d(color->rgb_to_lms_matrix[row * 3 + 0]),
                av_q2d(color->rgb_to_lms_matrix[row * 3 + 1]),
                av_q2d(color->rgb_to_lms_matrix[row * 3 + 2]));
    }
}

/* ── Dolby Vision Uniform Population ──
 *
 * Extracts reshape coefficients and color matrices from the DV RPU
 * metadata on each decoded frame and populates the GPU uniforms.
 * Called every frame; skips frames with no RPU side data.
 * Verbose logging only on first populate (state 1 → 2).
 *
 * The DV decode chain in the shader is:
 *   1. Reshape: affine per-component (poly_coef from RPU)
 *   2. ycc_to_rgb_matrix: ICtCp → PQ-encoded signal (with offsets)
 *   3. PQ EOTF → linear light
 *   4. Output matrix: precomputed (cone_inv × rgb_to_lms) → BT.2020 linear
 *
 * The ICtCp "cone" matrix (BT.2020 RGB → LMS, from ITU-R BT.2100):
 *   [1688/4096  2146/4096   262/4096]
 *   [ 683/4096  2951/4096   462/4096]
 *   [  99/4096   309/4096  3688/4096]
 *
 * Its inverse (LMS → BT.2020 linear RGB) is precomputed and multiplied
 * with rgb_to_lms on the CPU to save a shader matrix multiply. */

/* LMS → BT.2020 linear RGB for the DV chain.
 *
 * NOT the inverse of the full BT.2100 ICtCp cone matrix: that matrix
 * already contains the 4% crosstalk term (CT × M_HPE), but Dolby
 * Vision outputs BT.2020-referred *HPE* LMS — crosstalk-free. Using
 * inv(CT × M_HPE) here (as this table originally did) conjugated an
 * uncompensated crosstalk through every DV P5 pixel: a systematic ~4%
 * cross-channel mix, subtle enough to pass single-sided eye checks.
 * These constants are the crosstalk-free HPE inverse, matching
 * libplacebo's hardcoded dovi_lms2rgb (verified against
 * src/shaders/colorspace.c: "Dolby Vision always outputs
 * BT.2020-referred HPE LMS"). */
static const double ictcp_lms_to_bt2020[3][3] = {
    {  3.06441879, -2.16597676,  0.10155818 },
    { -0.65612108,  1.78554118, -0.12943749 },
    {  0.01736321, -0.04725154,  1.03004253 },
};

/* ── DV L1 dynamic brightness → per-scene tone-map peak ──
 * (deck a4a90b5 port, review PQUALITY P5.) L1 ext blocks carry the
 * AUTHORED per-scene min/max/avg as 12-bit PQ codes. max_pq → nits
 * becomes the live peak: dark scenes of high-peak masters get their
 * own knee, and P8 files with no container metadata stop guessing
 * 1000 nits. Self-gating on side-data presence so it serves P5 AND
 * P8. Clamp is [100, 10000] only — L1 is authored like the mastering
 * block, not computed like MaxCLL. Stale-carries on RPU-less frames
 * like the reshape curves. Outranks the histogram (authored beats
 * measured beats static). */
static void dovi_update_l1_peak(PlayerState *ps, const AVFrame *frame)
{
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!sd) return;
    const AVDOVIMetadata *dovi = (const AVDOVIMetadata *)sd->data;
    const AVDOVIDmData *dm = av_dovi_find_level(dovi, 1);
    if (!dm || dm->l1.max_pq == 0) return;

    float scene = pq_eotf_scalar((float)dm->l1.max_pq / 4095.0f);
    if (scene < 100.0f)   scene = 100.0f;
    if (scene > 10000.0f) scene = 10000.0f;

    float prev = ps->dovi_l1_peak_nits;
    ps->dovi_l1_peak_nits = scene;

    if (prev <= 0.0f) {
        log_msg("DOVI L1: dynamic tone-map peak engaged — scene max=%.0f "
                "nits (max_pq=%u, avg_pq=%u)",
                scene, dm->l1.max_pq, dm->l1.avg_pq);
        ps->dovi_l1_last_log = get_time_sec();
    } else if (fabsf(scene - prev) > 0.25f * prev
               && get_time_sec() - ps->dovi_l1_last_log > 5.0) {
        log_msg("DOVI L1: scene peak %.0f -> %.0f nits", prev, scene);
        ps->dovi_l1_last_log = get_time_sec();
    }
}

static void dovi_populate_uniforms(PlayerState *ps, const AVFrame *frame)
{
    if (ps->gpu_uniforms.is_dovi < 0.5f) return;
    if (ps->dovi_metadata_logged < 1) return; /* wait for logging pass */

    /* Per-frame RPU read — re-extract uniforms from each frame's side data */
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!sd) return;

    const AVDOVIMetadata *dovi = (const AVDOVIMetadata *)sd->data;
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(dovi);
    const AVDOVIDataMapping *mapping = av_dovi_get_mapping(dovi);
    const AVDOVIColorMetadata *color = av_dovi_get_color(dovi);

    double coef_scale = (double)(1LL << hdr->coef_log2_denom);

    /* ── Piecewise reshape coefficients (all pieces, all components) ──
     * DV spec allows up to 8 pieces per component with independent
     * polynomial coefficients. Pivots are normalized to [0,1] by
     * dividing by (2^bl_bit_depth - 1). Coefficients are packed into
     * float4 arrays indexed [piece][component] for GPU access. */
    float pivot_scale = (float)((1 << hdr->bl_bit_depth) - 1);
    if (pivot_scale < 1.0f) pivot_scale = 1023.0f; /* safety fallback */

    /* MMR banks default to off (orders 0) — set below if the RPU uses
     * MMR for a chroma component. Re-zeroed per frame: RPUs can switch
     * mapping method between scenes. */
    memset(ps->gpu_uniforms.dovi_mmr_meta, 0,
           sizeof(ps->gpu_uniforms.dovi_mmr_meta));
    memset(ps->gpu_uniforms.dovi_mmr_ct, 0,
           sizeof(ps->gpu_uniforms.dovi_mmr_ct));
    memset(ps->gpu_uniforms.dovi_mmr_cp, 0,
           sizeof(ps->gpu_uniforms.dovi_mmr_cp));

    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *curve = &mapping->curves[c];
        int num_pieces = 0;
        if (curve->num_pivots >= 2)
            num_pieces = curve->num_pivots - 1;
        if (num_pieces > 8) num_pieces = 8;
        ps->gpu_uniforms.dovi_num_pieces[c] = (float)num_pieces;

        /* Pack normalized pivots — [pivot_idx][component] */
        for (int i = 0; i < (int)curve->num_pivots && i < 9; i++)
            ps->gpu_uniforms.dovi_pivots[i][c] =
                (float)curve->pivots[i] / pivot_scale;

        /* Pack per-piece polynomial coefficients */
        for (int p = 0; p < num_pieces; p++) {
            if (curve->mapping_idc[p] == AV_DOVI_MAPPING_POLYNOMIAL) {
                int order = curve->poly_order[p];
                ps->gpu_uniforms.dovi_c0[p][c] =
                    (float)((double)curve->poly_coef[p][0] / coef_scale);
                ps->gpu_uniforms.dovi_c1[p][c] =
                    (order >= 1)
                    ? (float)((double)curve->poly_coef[p][1] / coef_scale)
                    : 0.0f;
                ps->gpu_uniforms.dovi_c2[p][c] =
                    (order >= 2)
                    ? (float)((double)curve->poly_coef[p][2] / coef_scale)
                    : 0.0f;
            } else if (curve->mapping_idc[p] == AV_DOVI_MAPPING_MMR
                       && (c == 1 || c == 2) && p == 0) {
                /* MMR chroma reshaping — the cross-channel polynomial
                 * nearly all real P5 content uses for Ct/Cp. Single
                 * piece (the universal case; the pivot span covers the
                 * full range). The poly slot gets identity so a stray
                 * evaluation of the unused path is harmless. */
                int order = curve->mmr_order[p];
                if (order < 1) order = 1;
                if (order > 3) order = 3;
                float *meta = ps->gpu_uniforms.dovi_mmr_meta;
                float (*bank)[4] = (c == 1) ? ps->gpu_uniforms.dovi_mmr_ct
                                            : ps->gpu_uniforms.dovi_mmr_cp;
                meta[c - 1] = (float)order;               /* x=Ct, y=Cp */
                meta[c + 1] =                             /* z=Ct, w=Cp */
                    (float)((double)curve->mmr_constant[p] / coef_scale);
                for (int o = 0; o < order; o++)
                    for (int t = 0; t < 7; t++) {
                        int idx = o * 7 + t;
                        bank[idx / 4][idx % 4] = (float)
                            ((double)curve->mmr_coef[p][o][t] / coef_scale);
                    }
                ps->gpu_uniforms.dovi_c0[p][c] = 0.0f;
                ps->gpu_uniforms.dovi_c1[p][c] = 1.0f;
                ps->gpu_uniforms.dovi_c2[p][c] = 0.0f;
            } else {
                /* MMR on the I component or on a non-first piece —
                 * unseen in real content; identity passthrough. */
                ps->gpu_uniforms.dovi_c0[p][c] = 0.0f;
                ps->gpu_uniforms.dovi_c1[p][c] = 1.0f;
                ps->gpu_uniforms.dovi_c2[p][c] = 0.0f;
                if (ps->dovi_metadata_logged < 2) {
                    const char *comp_names[] = {"I", "Ct", "Cp"};
                    log_msg("DOVI: WARNING — piece %d comp %s uses an "
                            "unsupported mapping shape, identity fallback",
                            p, comp_names[c]);
                }
            }
        }
    }
    ps->gpu_uniforms.dovi_num_pieces[3] = 0.0f; /* w component unused */

    /* ── ycc_to_rgb matrix + offsets → packed as float4 rows ──
     * Row format: [m0, m1, m2, offset] */
    for (int row = 0; row < 3; row++) {
        float *dst;
        switch (row) {
            case 0: dst = ps->gpu_uniforms.dovi_ycc_r0; break;
            case 1: dst = ps->gpu_uniforms.dovi_ycc_r1; break;
            default: dst = ps->gpu_uniforms.dovi_ycc_r2; break;
        }
        dst[0] = (float)av_q2d(color->ycc_to_rgb_matrix[row * 3 + 0]);
        dst[1] = (float)av_q2d(color->ycc_to_rgb_matrix[row * 3 + 1]);
        dst[2] = (float)av_q2d(color->ycc_to_rgb_matrix[row * 3 + 2]);
        dst[3] = (float)av_q2d(color->ycc_to_rgb_offset[row]);
    }

    /* ── Output matrix: precompute cone_inv × rgb_to_lms ──
     * Saves one 3×3 matmul per pixel in the shader. */
    double lms[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            lms[r][c] = av_q2d(color->rgb_to_lms_matrix[r * 3 + c]);

    for (int i = 0; i < 3; i++) {
        float *dst;
        switch (i) {
            case 0: dst = ps->gpu_uniforms.dovi_out_r0; break;
            case 1: dst = ps->gpu_uniforms.dovi_out_r1; break;
            default: dst = ps->gpu_uniforms.dovi_out_r2; break;
        }
        for (int j = 0; j < 3; j++) {
            double sum = 0.0;
            for (int k = 0; k < 3; k++)
                sum += ictcp_lms_to_bt2020[i][k] * lms[k][j];
            dst[j] = (float)sum;
        }
        dst[3] = 0.0f;
    }

    /* ── Peak nits from DV source_max_pq ──
     * More accurate than the 1000 nit fallback — DV RPU knows the actual
     * mastering peak. PQ code in 12-bit domain [0, 4095].
     * Updated per-frame; only logs when the peak changes. */
    if (color->source_max_pq > 0) {
        float pq_norm = (float)color->source_max_pq / 4095.0f;
        float dv_peak = pq_eotf_scalar(pq_norm);
        if (dv_peak > 100.0f) {
            float prev_peak = ps->hdr_static_peak;
            ps->gpu_uniforms.hdr_peak_nits = dv_peak;
            ps->hdr_static_peak = dv_peak;
            if (fabsf(dv_peak - prev_peak) > 0.5f) {
                log_msg("DOVI: source_max_pq=%u → peak=%.0f nits%s",
                        color->source_max_pq, dv_peak,
                        prev_peak < 1.0f ? " (initial)" : " (scene change)");
            }
        }
    }

    /* Log on first populate only (avoid per-frame log spam) */
    if (ps->dovi_metadata_logged < 2) {
        log_msg("DOVI: uniforms populated — pieces I=%d Ct=%d Cp=%d",
                (int)ps->gpu_uniforms.dovi_num_pieces[0],
                (int)ps->gpu_uniforms.dovi_num_pieces[1],
                (int)ps->gpu_uniforms.dovi_num_pieces[2]);
        /* Log piece-0 coefficients as representative sample */
        log_msg("DOVI: piece 0 — c0=[%.4f,%.4f,%.4f] c1=[%.4f,%.4f,%.4f] "
                "c2=[%.4f,%.4f,%.4f]",
                ps->gpu_uniforms.dovi_c0[0][0], ps->gpu_uniforms.dovi_c0[0][1],
                ps->gpu_uniforms.dovi_c0[0][2],
                ps->gpu_uniforms.dovi_c1[0][0], ps->gpu_uniforms.dovi_c1[0][1],
                ps->gpu_uniforms.dovi_c1[0][2],
                ps->gpu_uniforms.dovi_c2[0][0], ps->gpu_uniforms.dovi_c2[0][1],
                ps->gpu_uniforms.dovi_c2[0][2]);
        log_msg("DOVI: output matrix (cone_inv × rgb_to_lms):");
        log_msg("  [%.6f  %.6f  %.6f]", ps->gpu_uniforms.dovi_out_r0[0],
                ps->gpu_uniforms.dovi_out_r0[1], ps->gpu_uniforms.dovi_out_r0[2]);
        log_msg("  [%.6f  %.6f  %.6f]", ps->gpu_uniforms.dovi_out_r1[0],
                ps->gpu_uniforms.dovi_out_r1[1], ps->gpu_uniforms.dovi_out_r1[2]);
        log_msg("  [%.6f  %.6f  %.6f]", ps->gpu_uniforms.dovi_out_r2[0],
                ps->gpu_uniforms.dovi_out_r2[1], ps->gpu_uniforms.dovi_out_r2[2]);
    }

    /* Mark as populated at least once (allows log pass gate, peak shortcut) */
    ps->dovi_metadata_logged = 2;
}

#define PEAK_ATTACK_RATE    0.3f     /* rise towards new peak per frame   */
#define PEAK_DECAY_RATE     0.03f    /* decay towards new peak per frame  */
#define PEAK_SCENE_CUT_THR  0.5f     /* 50% increase = scene cut, jump up */
#define PEAK_MIN_NITS       100.0f   /* floor to prevent near-zero peaks   */
#define PEAK_PERCENTILE     99.875f  /* skip top 0.125% (specular hotspots) */

/* Scan the Y plane, build histogram, extract percentile peak,
 * convert to nits, smooth, and update the uniform.
 * Called once per frame from video_display() for HDR content only. */
/* NOTE (perf, deliberate): this scan runs on the render thread, in the
 * frame's critical path. Moving it to the decode thread was considered
 * and rejected for 0.3.0: on the swscale path the histogram must read
 * the CONVERTED frame, which only exists here — a per-path split would
 * add real risk for a bounded win. The per-sample work is already
 * minimal (¼×¼ subsample, one shift and one increment per sample) and
 * the sample count is now computed arithmetically rather than counted.
 * Revisit alongside any future frame-queue rework. */
static void hdr_compute_scene_peak(PlayerState *ps, const AVFrame *frame,
                                   int is_10bit)
{
    /* Skip if not HDR or in PQ bypass debug mode */
    if (ps->gpu_uniforms.is_hdr < 0.5f) return;
    if (ps->gpu_uniforms.hdr_debug > 1.5f && ps->gpu_uniforms.hdr_debug < 2.5f)
        return;  /* mode 2: PQ bypass, use static peak */

    /* DV Profile 5: skip CPU histogram — I-plane is IPTPQc2, not PQ luma.
     * Histogram reads garbage, stuck at PEAK_MIN_NITS floor.  Use the
     * static peak from source_max_pq (updated per-frame by dovi_populate_uniforms). */
    if (ps->gpu_uniforms.is_dovi > 0.5f) {
        ps->hdr_smoothed_peak = ps->hdr_static_peak;
        ps->hdr_prev_frame_peak = ps->hdr_static_peak;
        ps->gpu_uniforms.hdr_peak_nits = ps->hdr_static_peak;
        return;
    }

    const uint8_t *data = frame->data[0];
    int stride = frame->linesize[0];
    int w = ps->vid_w;
    int h = ps->vid_h;

    /* ── Build 256-bin histogram of Y plane ──
     * Subsample 4× in each dimension to reduce work.
     * For 10-bit: bin = uint16 >> 2 (10-bit value 0..1023 → 256 bins).
     *   NOTE: yuv420p10le stores 10-bit data right-justified in uint16
     *   containers (low 10 bits, high 6 bits zero). The shift must be 2,
     *   not 8 — shifting by 8 collapses the range to only 4 effective bins
     *   (0-3) and forces a binary percentile output. See commit notes.
     * For 8-bit:  bin = uint8 value directly. */
    int histogram[256];
    memset(histogram, 0, sizeof(histogram));
    /* Sample count is pure arithmetic — it was being incremented once
     * per sample (~518k times per 4K frame) for no reason. */
    int total_samples = ((h + 3) / 4) * ((w + 3) / 4);

    if (is_10bit) {
        for (int y = 0; y < h; y += 4) {
            const uint16_t *row = (const uint16_t *)(data + y * stride);
            for (int x = 0; x < w; x += 4) {
                /* 10-bit codes live in 0..1023; a nonconforming plane could
                 * carry larger values and index past histogram[256]. Clamp
                 * to the top bin — semantically "max brightness". */
                int bin = row[x] >> 2;
                histogram[bin > 255 ? 255 : bin]++;
            }
        }
    } else {
        for (int y = 0; y < h; y += 4) {
            const uint8_t *row = data + y * stride;
            for (int x = 0; x < w; x += 4) {
                histogram[row[x]]++;
            }
        }
    }

    /* ── Find the 99.875th percentile bin ──
     * Walk from the top bin downward, accumulating counts until
     * we've passed (100 - PEAK_PERCENTILE)% of total samples. */
    int skip_count = (int)((100.0f - PEAK_PERCENTILE) / 100.0f * total_samples);
    if (skip_count < 1) skip_count = 1;

    int accumulated = 0;
    int percentile_bin = 255;
    for (int i = 255; i >= 0; i--) {
        accumulated += histogram[i];
        if (accumulated >= skip_count) {
            percentile_bin = i;
            break;
        }
    }

    /* ── Convert bin to normalized value [0,1] ──
     * Reconstruct the bin's midpoint in uint16-normalized space.
     *
     * For 10-bit (bin = value>>2, value ∈ [0,1023]): bin N covers values
     *   [4N, 4N+3]; midpoint ≈ 4N + 1.5. Normalized: (4*bin + 2) / 65535
     *   ≡ (bin + 0.5) * 4 / 65535.
     *
     * For 8-bit (bin = value, value ∈ [0,255]): midpoint (bin + 0.5) / 256
     *   ≈ bin / 255 (close enough).
     */
    float raw_max_norm;
    if (is_10bit) {
        raw_max_norm = ((float)percentile_bin + 0.5f) * 4.0f / 65535.0f;
    } else {
        raw_max_norm = ((float)percentile_bin + 0.5f) / 256.0f;
    }

    /* ── Apply range expansion (same math as shader) ──
     * Convert from texture-space to PQ code [0,1] */
    float pq_code = (raw_max_norm - ps->gpu_uniforms.rangeY[0])
                  * ps->gpu_uniforms.rangeY[1];
    if (pq_code < 0.0f) pq_code = 0.0f;
    if (pq_code > 1.0f) pq_code = 1.0f;

    /* ── PQ → linear nits ── */
    float raw_peak_nits = pq_eotf_scalar(pq_code);

    /* ── Temporal smoothing ── */
    float smoothed = ps->hdr_smoothed_peak;
    float prev     = ps->hdr_prev_frame_peak;

    if (smoothed < 1.0f) {
        /* First frame — initialize directly */
        smoothed = raw_peak_nits;
    } else {
        /* Scene cut detection: large *increase* from previous frame → jump up.
         * Downward changes always use smooth decay to prevent strobe flicker
         * when dark frames temporarily depress the peak. */
        float change = (raw_peak_nits - prev) / fmaxf(prev, 1.0f);
        if (change > PEAK_SCENE_CUT_THR) {
            /* Bright scene cut — jump to avoid highlight clipping */
            smoothed = raw_peak_nits;
        } else if (raw_peak_nits > smoothed) {
            /* Attack: scene getting brighter — rise quickly */
            smoothed += PEAK_ATTACK_RATE * (raw_peak_nits - smoothed);
        } else {
            /* Decay: scene getting darker — fade slowly */
            smoothed += PEAK_DECAY_RATE * (raw_peak_nits - smoothed);
        }
    }

    /* Clamp: floor at max(PEAK_MIN_NITS, target_nits), ceiling at static peak.
     *
     * The floor MUST be ≥ target_nits to keep the BT.2390 EETF well-defined.
     * Below target, PQ(target)/PQ(smoothed) exceeds 1.0, the shader clamps
     * maxLum to 1.0 with ks -> 0.999, and the tone curve degenerates to a
     * pure pass-through (knee at the top of the range, no compression).
     * When smoothed crosses this boundary on scene cuts, tone mapping toggles
     * between "engaged" and "disengaged", producing a visible brightness strobe.
     * Flooring at target_nits keeps the curve continuous: low-peak content
     * naturally fits in SDR range (curve is near-identity), bright content
     * compresses gradually as smoothed rises. */
    float target_nits = ps->gpu_uniforms.hdr_target_nits;
    float floor_nits  = PEAK_MIN_NITS > target_nits ? PEAK_MIN_NITS : target_nits;
    if (smoothed < floor_nits) smoothed = floor_nits;
    if (ps->hdr_static_peak > 0.0f && smoothed > ps->hdr_static_peak)
        smoothed = ps->hdr_static_peak;

    /* Update state */
    ps->hdr_smoothed_peak   = smoothed;
    ps->hdr_prev_frame_peak = raw_peak_nits;

    /* Feed dynamic peak to the tone mapper */
    ps->gpu_uniforms.hdr_peak_nits = smoothed;

    /* Periodic log (every 120 frames ≈ 5s at 24fps). PQ-domain values,
     * matching the shader (2026-08-26 fix): healthy KS is 0.37-0.999;
     * KS printed near 0 would mean the linear-domain bug is back. */
    if (ps->diag_frames_displayed % 120 == 0) {
        float target = ps->gpu_uniforms.hdr_target_nits;
        /* m-S2-e: debug mode 1 runs the shader at target+100 — the
         * printed KS/maxLum must be the ones the shader uses. */
        int dbg1 = ps->gpu_uniforms.hdr_debug > 0.5f && ps->gpu_uniforms.hdr_debug < 1.5f;
        float eff_target = dbg1 ? target + 100.0f : target;
        double maxLum = pq_oetf_cpu(eff_target) / pq_oetf_cpu(smoothed);
        if (maxLum > 1.0) maxLum = 1.0;
        double ks = 1.5*maxLum - 0.5;
        if (ks < 0.0) ks = 0.0; else if (ks > 0.999) ks = 0.999;
        char tgt_s[48];
        if (dbg1) snprintf(tgt_s, sizeof(tgt_s), "%.0f (+100 debug = %.0f)", target, eff_target);
        else      snprintf(tgt_s, sizeof(tgt_s), "%.0f", target);
        log_msg("HDR peak: raw=%.0f nits, smoothed=%.0f nits "
                "(p%.1f, target=%s, static=%.0f, KS=%.3f, maxLum=%.4f)",
                raw_peak_nits, smoothed, PEAK_PERCENTILE,
                tgt_s, ps->hdr_static_peak, ks, maxLum);
    }
}


/* Display the current video frame: upload to GPU → shader draw.
 *
 * This is the hot path. Called once per new frame from main.c.
 *
 * Three source modes, all using the YUV planar pipeline (3 textures):
 *   1. 10-bit passthrough: direct upload, 2 bytes/sample (R16_UNORM)
 *   2. 8-bit YUV420P passthrough: direct upload, 1 byte/sample (R8_UNORM)
 *      Range expansion (limited→full) done in fragment shader.
 *   3. All other formats: swscale → upload, 1 byte/sample (R8_UNORM)
 */
void video_display(PlayerState *ps) {
    if (!ps->gpu_tex_y || !ps->video_frame || !ps->video_frame->data[0]) return;
    if (ps->seeking) return;

    /* Mid-stream resolution change (broadcast TS with SD interstitials
     * inside an HD recording). Textures and upload geometry are sized at
     * open time; feeding a differently-sized frame through them reads
     * past the smaller plane's allocation on the last rows. Skip such
     * frames — the previous picture holds until the stream returns to
     * its open-time size. */
    if (ps->video_frame->width  != ps->vid_w ||
        ps->video_frame->height != ps->vid_h ||
        ps->video_frame->format != ps->expected_pix_fmt) {
        /* Format changes get the resolution treatment: textures AND
         * transfer buffers were sized at open — an 8->10-bit switch
         * doubles the row bytes and overran the mapped staging
         * memory. Skip the frame; the previous picture holds. */
        /* per-file, not per-process — a static here silenced every
         * later file's resolution anomaly after the first */
        if (!ps->res_change_logged) {
            log_msg("WARN: mid-stream resolution/format change "
                    "%dx%d fmt %d -> %dx%d fmt %d — "
                    "frames at the new geometry are skipped",
                    ps->vid_w, ps->vid_h, ps->expected_pix_fmt,
                    ps->video_frame->width, ps->video_frame->height,
                    ps->video_frame->format);
            ps->res_change_logged = 1;
        }
        return;
    }

    int w  = ps->vid_w;
    int h  = ps->vid_h;
    int cw = (w + 1) / 2;   /* ceil — matches texture + FFmpeg alloc */
    int ch = (h + 1) / 2;

#ifdef DSVP_PROFILE
    double t_enter = get_time_sec();
#endif

    /* ── Determine source frame and byte width ── */
    AVFrame *src_frame;
    int bpp;  /* bytes per sample for upload_plane */

    int is_10bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
         && !ps->sws_ctx);

    if (is_10bit_passthrough) {
        /* 10-bit passthrough — raw frame directly to R16_UNORM textures */
        src_frame = ps->video_frame;
        bpp = 2;
    } else if (!ps->sws_ctx) {
        /* 8-bit YUV420P passthrough — direct upload, range in shader */
        src_frame = ps->video_frame;
        bpp = 1;
    } else {
        /* swscale path — format conversion to yuv420p / yuv420p10le */
        sws_scale(ps->sws_ctx,
            (const uint8_t *const *)ps->video_frame->data,
            ps->video_frame->linesize,
            0, ps->vid_h,
            ps->rgb_frame->data,
            ps->rgb_frame->linesize);
        src_frame = ps->rgb_frame;
        bpp = ps->sws_out_10bit ? 2 : 1;
    }

    /* ── Dolby Vision RPU metadata extraction ──
     * Extract and log reshaping curves from first DV frame.
     * Uses original decoded frame (side data not on swscale output). */
    dovi_log_frame_metadata(ps, ps->video_frame);
    dovi_populate_uniforms(ps, ps->video_frame);
    dovi_update_l1_peak(ps, ps->video_frame);   /* P5 AND P8 */

    /* ── HDR dynamic peak detection (CPU scan) ──
     * Scan luma plane to find actual scene peak before uploading.
     * Updates hdr_peak_nits uniform with temporally smoothed value.
     * PQ only — the histogram converts bins through the PQ EOTF, which
     * is meaningless for HLG's relative signal (fixed 1000-nit OOTF). */
#ifdef DSVP_PROFILE
    ps->prof_peak_ms = 0.0;
    double t_before_peak = get_time_sec();
#endif
    if (ps->gpu_uniforms.is_hlg >= 0.5f) {
        /* HLG: peak pinned at open; histogram's PQ bin decode is
         * meaningless for HLG's relative signal. */
    } else if (ps->dovi_l1_peak_nits > 0.0f) {
        /* DV L1 authored per-scene peak (deck a4a90b5 port):
         * dynamic metadata outranks the measured histogram. */
        ps->gpu_uniforms.hdr_peak_nits = ps->dovi_l1_peak_nits;
    } else {
        hdr_compute_scene_peak(ps, src_frame, bpp == 2 /* upload is 10-bit */);
    }
#ifdef DSVP_PROFILE
    ps->prof_peak_ms = (get_time_sec() - t_before_peak) * 1000.0;
#endif

    /* ── Upload plane data to GPU transfer buffers ── */
    Uint32 ppr_y = upload_plane(ps->gpu_device, ps->gpu_xfer_y, ps->gpu_xfer_y_cap,
                 src_frame->data[0], src_frame->linesize[0], w * bpp, h, bpp);
    Uint32 ppr_u = upload_plane(ps->gpu_device, ps->gpu_xfer_u, ps->gpu_xfer_uv_cap,
                 src_frame->data[1], src_frame->linesize[1], cw * bpp, ch, bpp);
    Uint32 ppr_v = upload_plane(ps->gpu_device, ps->gpu_xfer_v, ps->gpu_xfer_uv_cap,
                 src_frame->data[2], src_frame->linesize[2], cw * bpp, ch, bpp);

#ifdef DSVP_PROFILE
    /* upload column = CPU-side prep since entry (sws convert, DV RPU,
     * transfer-buffer memcpy) minus the separately-counted peak scan. */
    double t_before_gpu = get_time_sec();
    ps->prof_upload_ms = (t_before_gpu - t_enter) * 1000.0 - ps->prof_peak_ms;
#endif

    /* ── GPU command buffer ── */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) {
        log_msg("ERROR: SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return;
    }

    /* ── Copy pass: transfer buffers → GPU textures ──
     * pixels_per_row comes from upload_plane: equals the source stride
     * when the stride-padded single-memcpy path was taken, so the GPU
     * skips the row padding during the copy. */
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src_info;
        SDL_GPUTextureRegion dst_region;

        /* ppr == 0 means the transfer-buffer map failed for that
         * plane — skip its copy; the previous texture content holds
         * for one frame instead of uploading undefined bytes. */

        /* Y plane */
        if (ppr_y) {
            SDL_zero(src_info);
            SDL_zero(dst_region);
            src_info.transfer_buffer = ps->gpu_xfer_y;
            src_info.pixels_per_row  = ppr_y;
            src_info.rows_per_layer  = h;
            dst_region.texture = ps->gpu_tex_y;
            dst_region.w = w;
            dst_region.h = h;
            dst_region.d = 1;
            SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);
        }

        /* U plane */
        if (ppr_u) {
            SDL_zero(src_info);
            SDL_zero(dst_region);
            src_info.transfer_buffer = ps->gpu_xfer_u;
            src_info.pixels_per_row  = ppr_u;
            src_info.rows_per_layer  = ch;
            dst_region.texture = ps->gpu_tex_u;
            dst_region.w = cw;
            dst_region.h = ch;
            dst_region.d = 1;
            SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);
        }

        /* V plane */
        if (ppr_v) {
            SDL_zero(src_info);
            SDL_zero(dst_region);
            src_info.transfer_buffer = ps->gpu_xfer_v;
            src_info.pixels_per_row  = ppr_v;
            src_info.rows_per_layer  = ch;
            dst_region.texture = ps->gpu_tex_v;
            dst_region.w = cw;
            dst_region.h = ch;
            dst_region.d = 1;
            SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);
        }
    }
    SDL_EndGPUCopyPass(copy);

    /* ── Overlay copy pass (if dirty) ── */
    gpu_overlay_copy_cmd(cmd, ps);

    /* ── Acquire swapchain texture ── */
    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ps->window,
            &swapchain_tex, &sc_w, &sc_h)) {
        log_msg("ERROR: Swapchain acquire failed: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    /* Cache physical pixel dimensions for DPI-correct overlay sizing */
    ps->sc_w = (int)sc_w;
    ps->sc_h = (int)sc_h;

#ifdef DSVP_PROFILE
    /* vsync column = command-buffer acquire + copy-pass recording +
     * swapchain acquire (the VSync/backpressure wait lives here). */
    double t_after_vsync = get_time_sec();
    ps->prof_vsync_ms = (t_after_vsync - t_before_gpu) * 1000.0;
#endif

    /* ── Render shaded frame into the cache, then blit + overlay ──
     * Fallback: if the cache texture or blit pipeline is unavailable,
     * shade directly into the swapchain (the pre-cache behavior). */
    int use_cache = (ps->gpu_pipeline_blit != NULL
                     && gpu_cache_ensure(ps, (int)sc_w, (int)sc_h) == 0);

    if (use_cache)
        gpu_render_to_cache(cmd, ps);

    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture    = swapchain_tex;
    color_target.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    color_target.load_op    = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op   = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        if (use_cache)
            gpu_blit_cache(pass, ps, sc_w, sc_h);
        else
            gpu_draw_video_quad(pass, cmd, ps, sc_w, sc_h);

        /* ── Overlay quad (alpha-blended over video) ── */
        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);

#ifdef DSVP_PROFILE
    {
        double t_exit = get_time_sec();
        ps->prof_render_ms  = (t_exit - t_after_vsync) * 1000.0;
        ps->prof_display_ms = (t_exit - t_enter) * 1000.0;

        /* Spike log: flag individual frames that blow the budget.
         * Total includes the normal VSync wait (~14ms at 60Hz), so
         * only genuine anomalies trip the 20ms line. */
        if (ps->prof_peak_ms > 3.0 || ps->prof_upload_ms > 8.0
                || ps->prof_display_ms > 20.0) {
            log_msg("PROF SPIKE: upload=%.1f peak=%.1f vsync=%.1f "
                    "render=%.1f total=%.1fms (frame %d)",
                    ps->prof_upload_ms, ps->prof_peak_ms,
                    ps->prof_vsync_ms, ps->prof_render_ms,
                    ps->prof_display_ms, ps->diag_frames_displayed);
        }

        /* Running stats (reset by main.c's 10s DIAG report) */
        ps->prof_n++;
        ps->prof_sum_upload += ps->prof_upload_ms;
        ps->prof_sum_peak   += ps->prof_peak_ms;
        ps->prof_sum_vsync  += ps->prof_vsync_ms;
        ps->prof_sum_render += ps->prof_render_ms;
        ps->prof_sum_total  += ps->prof_display_ms;
        if (ps->prof_upload_ms  > ps->prof_max_upload)
            ps->prof_max_upload = ps->prof_upload_ms;
        if (ps->prof_peak_ms    > ps->prof_max_peak)
            ps->prof_max_peak   = ps->prof_peak_ms;
        if (ps->prof_vsync_ms   > ps->prof_max_vsync)
            ps->prof_max_vsync  = ps->prof_vsync_ms;
        if (ps->prof_render_ms  > ps->prof_max_render)
            ps->prof_max_render = ps->prof_render_ms;
        if (ps->prof_display_ms > ps->prof_max_total)
            ps->prof_max_total  = ps->prof_display_ms;
    }
#endif

    ps->video_ready = 1;
}


/* Re-draw the last frame without uploading new data.
 * Called from main.c on ticks where no new frame was decoded
 * (GPU double-buffering requires explicit re-blit each frame).
 * Also used for paused state rendering.
 *
 * With the shaded-frame cache, steady-state reblits are a single
 * 1-fetch blit. The 48-tap YUV shader only re-runs when the cache is
 * stale: HDR uniform change (H/T/G keys clear cache_valid), swapchain
 * resize, or letterbox geometry change. The YUV planes stay resident
 * on the GPU, so re-rendering needs no new upload. */
void video_reblit(PlayerState *ps) {
    if (!ps->gpu_tex_y) return;

#ifdef DSVP_PROFILE
    /* Reblit is nominally a 1-fetch blit; if its acquire blocks for
     * multiple vsyncs the swapchain queue is backed up by earlier
     * work — that split (vsync vs total) is the reason it's timed. */
    double t_rb_enter = get_time_sec();
#endif

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) return;

    /* ── Overlay copy pass (if dirty — e.g. first reblit after overlay update) ── */
    gpu_overlay_copy_cmd(cmd, ps);

    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ps->window,
            &swapchain_tex, &sc_w, &sc_h)) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    /* Cache physical pixel dimensions for DPI-correct overlay sizing */
    ps->sc_w = (int)sc_w;
    ps->sc_h = (int)sc_h;

#ifdef DSVP_PROFILE
    double t_rb_vsync = get_time_sec();
#endif

    int use_cache = (ps->gpu_pipeline_blit != NULL
                     && gpu_cache_ensure(ps, (int)sc_w, (int)sc_h) == 0);

    if (use_cache) {
        /* Re-render only when stale (gpu_cache_ensure already cleared
         * cache_valid if the swapchain was resized). */
        player_update_display_rect(ps);
        if (!ps->cache_valid
                || ps->cache_rect.x != ps->display_rect.x
                || ps->cache_rect.y != ps->display_rect.y
                || ps->cache_rect.w != ps->display_rect.w
                || ps->cache_rect.h != ps->display_rect.h) {
            gpu_render_to_cache(cmd, ps);
        }
    }

    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture    = swapchain_tex;
    color_target.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    color_target.load_op    = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op   = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        if (use_cache)
            gpu_blit_cache(pass, ps, sc_w, sc_h);
        else
            gpu_draw_video_quad(pass, cmd, ps, sc_w, sc_h);

        /* ── Overlay quad (alpha-blended over video) ── */
        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);

#ifdef DSVP_PROFILE
    {
        double t_rb_exit  = get_time_sec();
        double rb_vsync   = (t_rb_vsync - t_rb_enter) * 1000.0;
        double rb_total   = (t_rb_exit  - t_rb_enter) * 1000.0;
        ps->prof_rb_n++;
        ps->prof_sum_rb_vsync += rb_vsync;
        ps->prof_sum_rb_total += rb_total;
        if (rb_vsync > ps->prof_max_rb_vsync)
            ps->prof_max_rb_vsync = rb_vsync;
        if (rb_total > ps->prof_max_rb_total)
            ps->prof_max_rb_total = rb_total;
    }
#endif
}


/* ═══════════════════════════════════════════════════════════════════
 * Seeking
 * ═══════════════════════════════════════════════════════════════════ */

/* Seek by `incr` seconds relative to current position. */
/* Absolute seek. backward=1 asks the demuxer for the keyframe AT OR
 * BEFORE pos (resume, chapter starts: never overshoot the moment);
 * backward=0 takes the next keyframe (the default flag choice for a
 * forward step). Same request slot as player_seek. */
void player_seek_abs(PlayerState *ps, double pos_sec, int backward) {
    if (!ps->playing) return;
    if (pos_sec < 0.0) pos_sec = 0.0;
    ps->seek_target  = (int64_t)(pos_sec * AV_TIME_BASE);
    ps->seek_flags   = backward ? AVSEEK_FLAG_BACKWARD : 0;
    ps->seek_request = 1;
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;
}

/* Index of the chapter containing pos, -1 when the file has none. */
int player_chapter_index(const PlayerState *ps, double pos_sec) {
    if (ps->nb_chapters <= 0) return -1;
    int idx = 0;
    for (int i = 0; i < ps->nb_chapters; i++)
        if (ps->chapter_start[i] <= pos_sec + 1e-3) idx = i;
    return idx;
}

void player_seek(PlayerState *ps, double incr) {
    if (!ps->playing) return;
    ps->chapter_nav_idx = -1;   /* a free seek ends the chapter pre-roll rule */

    /* Audio-only files never update video_clock — seek relative to
     * the audio playback position instead. */
    double base = (ps->video_stream_idx >= 0)
        ? ps->video_clock : ps->audio_clock_sync;
    double pos = base + incr;
    if (pos < 0.0) pos = 0.0;

    ps->seek_target  = (int64_t)(pos * AV_TIME_BASE);
    ps->seek_flags   = (incr < 0) ? AVSEEK_FLAG_BACKWARD : 0;
    ps->seek_request = 1;

    /* Reset video timing after seek */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;
}


/* ═══════════════════════════════════════════════════════════════════
 * Media Info / Debug
 * ═══════════════════════════════════════════════════════════════════ */

/* Bounded append for the info/debug string builders.
 *
 * HEAP-OVERFLOW FIX: snprintf returns the WOULD-HAVE-written length,
 * so the naive `off += snprintf(buf+off, sz-off, ...)` pattern lets
 * `off` exceed `sz` once any field (long filename, metadata tag, ...)
 * truncates. The next call then computes a negative size — which
 * converts to a huge size_t — with buf+off pointing past the buffer.
 * This macro clamps `off` to sz-1, so every subsequent call safely
 * degenerates to a 0/1-byte write. Output truncates; memory survives.
 *
 * Expects locals: char *buf; int sz; int off; */
#define INFO_APPEND(...)                                                    \
    do {                                                                    \
        if (off < sz - 1) {                                                 \
            int _n = snprintf(buf + off, (size_t)(sz - off), __VA_ARGS__);  \
            if (_n > 0) off += _n;                                          \
            if (off > sz - 1) off = sz - 1;                                 \
        }                                                                   \
    } while (0)

void player_build_media_info(PlayerState *ps) {
    if (!ps->fmt_ctx) return;

    char *buf = ps->media_info;
    int   sz  = sizeof(ps->media_info);
    int   off = 0;

    INFO_APPEND("=== MEDIA INFO ===\n");
    INFO_APPEND("File: %s\n", ps->filepath);
    INFO_APPEND("Format: %s (%s)\n",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name);

    double duration = (ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    int hrs = (int)duration / 3600;
    int min = ((int)duration % 3600) / 60;
    int sec = (int)duration % 60;
    INFO_APPEND("Duration: %02d:%02d:%02d\n", hrs, min, sec);
    if (ps->nb_chapters > 0) {
        INFO_APPEND("Chapters: %d (PgUp/PgDn)\n", ps->nb_chapters);
        for (int ci = 0; ci < ps->nb_chapters && ci < 24; ci++) {
            int cs = (int)ps->chapter_start[ci];
            INFO_APPEND("  %2d  %d:%02d:%02d  %s\n", ci + 1,
                        cs / 3600, (cs / 60) % 60, cs % 60, ps->chapter_title[ci]);
        }
        if (ps->nb_chapters > 24) INFO_APPEND("  … %d more\n", ps->nb_chapters - 24);
    }

    if (ps->fmt_ctx->bit_rate > 0) {
        INFO_APPEND("Bitrate: %"PRId64" kb/s\n",
            ps->fmt_ctx->bit_rate / 1000);
    }

    /* Video stream info */
    if (ps->video_stream_idx >= 0) {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        AVCodecParameters *par = vs->codecpar;
        INFO_APPEND("\n--- Video ---\n");
        INFO_APPEND("Codec: %s\n",
            avcodec_get_name(par->codec_id));
        INFO_APPEND("Resolution: %dx%d\n",
            par->width, par->height);
        INFO_APPEND("Pixel Format: %s\n",
            av_get_pix_fmt_name(par->format));

        if (vs->avg_frame_rate.den > 0) {
            INFO_APPEND("Frame Rate: %.3f fps\n",
                av_q2d(vs->avg_frame_rate));
        }
        if (vs->r_frame_rate.den > 0) {
            INFO_APPEND("Real Frame Rate: %.3f fps\n",
                av_q2d(vs->r_frame_rate));
        }
        if (par->bit_rate > 0) {
            INFO_APPEND("Video Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }

        /* Color info — show tagged values, or infer with "(assumed)" */
        {
            int is_hd = (par->height >= 720);

            if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                INFO_APPEND("Color Space: %s\n",
                    av_color_space_name(par->color_space));
            } else {
                INFO_APPEND("Color Space: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            if (par->color_range != AVCOL_RANGE_UNSPECIFIED) {
                INFO_APPEND("Color Range: %s\n",
                    av_color_range_name(par->color_range));
            } else {
                INFO_APPEND("Color Range: tv (assumed)\n");
            }

            if (par->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                INFO_APPEND("Color Primaries: %s\n",
                    av_color_primaries_name(par->color_primaries));
            } else {
                INFO_APPEND("Color Primaries: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            if (par->color_trc != AVCOL_TRC_UNSPECIFIED) {
                INFO_APPEND("Color TRC: %s\n",
                    av_color_transfer_name(par->color_trc));
            } else {
                INFO_APPEND("Color TRC: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            /* HDR info from uniforms (already detected at open time) */
            if (ps->gpu_uniforms.is_hdr > 0.0f) {
                INFO_APPEND(
                    "HDR: Yes (peak %.0f nits, %s gamut)\n",
                    ps->gpu_uniforms.hdr_peak_nits,
                    ps->gpu_uniforms.hdr_gamut > 0.5f ? "BT.2020" : "BT.709");
            }
        }
    }

    /* Audio stream info */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        AVCodecParameters *par = as->codecpar;
        INFO_APPEND("\n--- Audio ---\n");
        INFO_APPEND("Codec: %s\n",
            avcodec_get_name(par->codec_id));
        INFO_APPEND("Sample Rate: %d Hz\n",
            par->sample_rate);
        INFO_APPEND("Channels: %d\n",
            par->ch_layout.nb_channels);

        char ch_layout_str[128];
        av_channel_layout_describe(&par->ch_layout, ch_layout_str, sizeof(ch_layout_str));
        INFO_APPEND("Channel Layout: %s\n", ch_layout_str);

        INFO_APPEND("Sample Format: %s\n",
            av_get_sample_fmt_name(par->format));
        if (par->bit_rate > 0) {
            INFO_APPEND("Audio Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }
    }

    /* Metadata — unbounded user-controlled content; this is exactly
     * where the old pattern overflowed on tag-heavy files. */
    AVDictionaryEntry *tag = NULL;
    int first = 1;
    while ((tag = av_dict_get(ps->fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (first) {
            INFO_APPEND("\n--- Metadata ---\n");
            first = 0;
        }
        INFO_APPEND("%s: %s\n", tag->key, tag->value);
    }
}

void player_build_debug_info(PlayerState *ps) {
    if (!ps->playing) return;

    char *buf = ps->debug_info;
    int   sz  = sizeof(ps->debug_info);
    int   off = 0;

    INFO_APPEND("=== DEBUG ===\n");
    INFO_APPEND("Build:       %s\n", DSVP_GIT_COMMIT);
    INFO_APPEND("Output:      %dx%d (%s)\n",
        ps->sc_w, ps->sc_h,
        ps->fullscreen ? "borderless" : "windowed");

    /* Output = the physical-pixel area the scaler actually fills
     * (display_rect is in logical window coords; convert to
     * swapchain pixels, matching the viewport math in video_display). */
    int out_w = ps->display_rect.w;
    int out_h = ps->display_rect.h;
    if (ps->win_w > 0 && ps->sc_w > 0)
        out_w = (int)((double)ps->display_rect.w * ps->sc_w / ps->win_w + 0.5);
    if (ps->win_h > 0 && ps->sc_h > 0)
        out_h = (int)((double)ps->display_rect.h * ps->sc_h / ps->win_h + 0.5);

    /* Real-time FPS + scaler resolution (video streams only) */
    if (ps->video_stream_idx >= 0) {
        if (ps->paused)
            INFO_APPEND("FPS:         paused\n");
        else
            INFO_APPEND("FPS:         %.2f (render %.0f)\n",
                ps->fps_content, ps->fps_render);
        INFO_APPEND("Resolution:  %dx%d -> %dx%d\n",
            ps->vid_w, ps->vid_h, out_w, out_h);
    }

    INFO_APPEND("Renderer: SDL_GPU\n");
    INFO_APPEND("Video Queue: %d pkts (%d KB)\n",
        ps->video_pq.nb_packets, ps->video_pq.size / 1024);
    INFO_APPEND("Audio Queue: %d pkts (%d KB)\n",
        ps->audio_pq.nb_packets, ps->audio_pq.size / 1024);
    INFO_APPEND("Volume:      %.0f%%\n", ps->volume * 100.0);

    if (ps->video_codec_ctx) {
        const char *pfn = av_get_pix_fmt_name(ps->video_codec_ctx->pix_fmt);
        INFO_APPEND("Decode: software %s %s, %d threads\n",
            avcodec_get_name(ps->video_codec_ctx->codec_id),
            pfn ? pfn : "?", ps->video_codec_ctx->thread_count);

        int is_yuv420p = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P);
        int is_10bit = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE);
        int is_full_range = (ps->fmt_ctx &&
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar->color_range == AVCOL_RANGE_JPEG);

        if (is_10bit && !ps->sws_ctx) {
            INFO_APPEND(
                "SWS: bypassed (10-bit passthrough, %s->full in shader)\n",
                is_full_range ? "full" : "limited");
        } else if (is_yuv420p && !ps->sws_ctx) {
            INFO_APPEND(
                "SWS: bypassed (8-bit passthrough, %s->full in shader)\n",
                is_full_range ? "full" : "limited");
        } else {
            INFO_APPEND(
                "SWS: format convert%s (SWS_LANCZOS + ED dither)\n",
                ps->sws_out_10bit ? " to 10-bit" : "");
        }

        /* Report the kernel as it actually runs. x64 has ONE luma
         * variant (Lanczos-2, 4x4 at df<=1) whose footprint dilates on
         * downscale — the shader measures df from screen-space
         * derivatives per axis and caps it at 4; this reproduces that
         * from the same source/viewport sizes (exact under
         * letterboxing, which is where the old static line was
         * silent about the extra taps). */
        {
            double dfx = 1.0, dfy = 1.0;
            if (out_w > 0) dfx = (double)ps->vid_w / out_w;
            if (out_h > 0) dfy = (double)ps->vid_h / out_h;
            if (dfx < 1.0) dfx = 1.0;
            if (dfy < 1.0) dfy = 1.0;
            int capped = (dfx > 4.0 || dfy > 4.0);
            if (dfx > 4.0) dfx = 4.0;
            if (dfy > 4.0) dfy = 4.0;
            if (dfx > 1.0 || dfy > 1.0)
                INFO_APPEND("Sampler: Lanczos-2 luma dilated x%.2f/%.2f%s, "
                            "Catmull-Rom chroma\n",
                    dfx, dfy, capped ? " (cap 4)" : "");
            else
                INFO_APPEND("Sampler: Lanczos-2 luma 4x4, Catmull-Rom chroma\n");
            INFO_APPEND("Dither:  blue noise 64x64\n");
        }

        {
            static const char *chroma_names[] = {
                "unspecified", "left", "center", "top-left",
                "top", "bottom-left", "bottom"
            };
            int cl = ps->chroma_location;
            const char *cn = (cl >= 0 && cl <= 6) ? chroma_names[cl] : "unknown";
            INFO_APPEND("Chroma Siting: %s\n", cn);
        }
    }

    /* Audio track info */
    if (ps->aud_count > 1) {
        INFO_APPEND("Audio Track: %s (%d/%d)\n",
            ps->aud_stream_names[ps->aud_selection],
            ps->aud_selection + 1, ps->aud_count);
    }

    /* Subtitle info */
    if (ps->sub_count > 0) {
        if (ps->sub_selection == 0) {
            INFO_APPEND("Subtitles: off (%d available)\n",
                ps->sub_count);
        } else {
            INFO_APPEND("Subtitles: %s\n",
                ps->sub_stream_names[ps->sub_selection - 1]);
        }
    } else {
        INFO_APPEND("Subtitles: none found\n");
    }

    double duration = (ps->fmt_ctx && ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    /* Audio-only files never update video_clock */
    double pos = (ps->video_stream_idx >= 0)
        ? ps->video_clock : ps->audio_clock_sync;
    INFO_APPEND("Position:    %.1f / %.1f s\n", pos, duration);

    /* HDR — the live tone-map contract: the same numbers the engage
     * log and the periodic "HDR peak:" line print, so a screenshot
     * of the panel and a grep of the log never disagree. */
    if (ps->gpu_uniforms.is_hdr > 0.0f) {
        const GPUUniforms *u = &ps->gpu_uniforms;
        INFO_APPEND("\n--- HDR ---\n");
        /* On DV the "static" peak is the RPU's source_max_pq, rewritten
         * per frame — label it for what it is. */
        INFO_APPEND("Signal:  %s, %s, %s %.0f nits\n",
            u->is_dovi > 0.0f ? "Dolby Vision (reshape active)"
          : u->is_hlg  > 0.0f ? "HLG" : "PQ (HDR10)",
            u->hdr_gamut > 0.5f ? "BT.2020" : "BT.709",
            u->is_dovi > 0.0f ? "RPU max" : "static peak",
            ps->hdr_static_peak);
        INFO_APPEND("Mode:    %s [Z]\n",
            ps->hdr_out_active ? "PASSTHROUGH (display tone-maps)"
                               : "tone-map to SDR");
        if (!ps->hdr_out_active) {
            /* Which peak drives the curve THIS frame (priority order
             * of the writers in video_display): DV L1 authored scene
             * peak outranks the histogram, which outranks static. */
            /* m-S2-d: on DV the histogram never runs — the bypass in
             * hdr_compute_scene_peak copies the RPU static peak into
             * hdr_smoothed_peak, so "histogram" was the wrong writer. */
            const char *psrc = ps->dovi_l1_peak_nits > 0.0f ? "DV L1 authored scene"
                             : u->is_dovi > 0.5f              ? "RPU source_max_pq"
                             : ps->hdr_smoothed_peak > 0.0f ? "histogram p99.875 smoothed"
                                                            : "static metadata";
            float peak   = u->hdr_peak_nits;
            float target = u->hdr_target_nits;
            /* m-S2-e: debug mode 1 runs the shader at target+100. */
            int dbg1 = u->hdr_debug > 0.5f && u->hdr_debug < 1.5f;
            float eff_target = dbg1 ? target + 100.0f : target;
            INFO_APPEND("Peak:    %.0f nits (%s)\n", peak, psrc);
            if (dbg1)
                INFO_APPEND("Target:  %.0f nits [T] (+100 debug = %.0f)  midtone %.2f [G]\n",
                    target, eff_target, u->hdr_midtone_gain);
            else
                INFO_APPEND("Target:  %.0f nits [T]  midtone %.2f [G]\n",
                    target, u->hdr_midtone_gain);
            if (peak > 0.0f && target > 0.0f) {
                /* PQ-domain, identical to the log's formula. KS near 0
                 * here = the linear-domain bug is back. */
                double maxLum = pq_oetf_cpu(eff_target) / pq_oetf_cpu(peak);
                if (maxLum > 1.0) maxLum = 1.0;
                double ks = 1.5 * maxLum - 0.5;
                if (ks < 0.0) ks = 0.0; else if (ks > 0.999) ks = 0.999;
                INFO_APPEND("EETF:    BT.2390 PQ-domain  KS=%.3f  maxLum=%.4f\n",
                    ks, maxLum);
            }
            if (u->out_black_nits > 0.0f)
                INFO_APPEND("Black:   lift to %.3f nits (%s)\n",
                    u->out_black_nits, ps->black_src ? ps->black_src : "?");
            else if (u->out_black_nits == 0.0f)
                INFO_APPEND("Black:   lift auto target/2000\n");
            else
                INFO_APPEND("Black:   lift off\n");
            {
                static const char *dbg_names[] = {
                    "off", "1 (target+100 compare)",
                    "2 (PQ bypass, raw)", "3 (luminance viz)"
                };
                int dm = (int)u->hdr_debug;
                /* m-S2-j: modes 2 and 3 live in the HDR10/HLG branch
                 * of the shader; the DV branch consults hdr_debug only
                 * for mode 1's +100. On DV the panel must not claim a
                 * bypass or a luminance view the picture is not showing. */
                INFO_APPEND("Debug:   %s [H]%s\n",
                    (dm >= 0 && dm <= 3) ? dbg_names[dm] : "?",
                    (u->is_dovi > 0.5f && dm >= 2) ? "  — n/a on DV (no effect)" : "");
            }
            if (u->out_gamma > 0.0f)
                INFO_APPEND("Transfer: gamma %.1f out\n", u->out_gamma);
            else
                INFO_APPEND("Transfer: sRGB piecewise out\n");
        } else {
            INFO_APPEND("Dials:   T/G/H inactive — the display owns the curve\n");
        }
#ifdef _WIN32
        if (g_winhdr.engaged)
            INFO_APPEND("Display: Windows HDR engaged by DSVP (restores on exit)\n");
        else if (ps->hdr_out_active)
            INFO_APPEND("Display: Windows HDR already on (user's setting)\n");
        else
            INFO_APPEND("Display: untouched\n");
#else
        if (g_linhdr.engaged) {
            int t = hdr_sdr_target();
            if (g_linhdr.prior_sdrbr > 0 && t > 0 && g_linhdr.prior_sdrbr != t)
                INFO_APPEND("Display: KWin HDR held on %s, sdr-brightness %d "
                            "(restores %d)\n",
                    g_linhdr.output, t, g_linhdr.prior_sdrbr);
            else
                INFO_APPEND("Display: KWin HDR held on %s, sdr-brightness untouched\n",
                    g_linhdr.output);
        } else if (ps->hdr_out_active) {
            INFO_APPEND("Display: HDR already on %s (user's setting)\n",
                g_linhdr.output[0] ? g_linhdr.output : "output");
        } else {
            INFO_APPEND("Display: untouched\n");
        }
#endif
    }

    /* Audio status */
    INFO_APPEND("\n--- Audio ---\n");
    if (ps->audio_codec_ctx) {
        const char *acodec = avcodec_get_name(ps->audio_codec_ctx->codec_id);
        const char *afmt   = av_get_sample_fmt_name(ps->audio_codec_ctx->sample_fmt);
        int src_rate = ps->audio_codec_ctx->sample_rate;
        int src_ch   = ps->audio_codec_ctx->ch_layout.nb_channels;

        char layout_desc[64] = {0};
        av_channel_layout_describe(&ps->audio_codec_ctx->ch_layout,
                                   layout_desc, sizeof(layout_desc));

        INFO_APPEND("Source:  %s %s %dHz %dch (%s)\n",
            acodec, afmt ? afmt : "?", src_rate, src_ch, layout_desc);

        /* Determine output format name from SDL spec */
        const char *out_fmt = (ps->audio_spec.format == SDL_AUDIO_F32) ? "F32" :
                              (ps->audio_spec.format == SDL_AUDIO_S16) ? "S16" : "???";
        int out_rate = ps->audio_spec.freq;
        int out_ch   = ps->audio_spec.channels;
        INFO_APPEND("Output:  %s %dHz %dch (stereo)\n",
            out_fmt, out_rate, out_ch);

        int resampling = (src_rate != out_rate);
        int downmixing = (src_ch != out_ch);

        if (resampling && downmixing)
            INFO_APPEND(
                "Pipeline: resample %d->%dHz + downmix %dch->%dch + %s\n",
                src_rate, out_rate, src_ch, out_ch, out_fmt);
        else if (resampling)
            INFO_APPEND(
                "Pipeline: resample %d->%dHz + %s\n", src_rate, out_rate, out_fmt);
        else if (downmixing)
            INFO_APPEND(
                "Pipeline: downmix %dch->%dch + %s\n", src_ch, out_ch, out_fmt);
        else if (afmt && strcmp(afmt, "flt") == 0 &&
                 ps->audio_spec.format == SDL_AUDIO_F32)
            INFO_APPEND("Pipeline: direct (no conversion)\n");
        else
            INFO_APPEND(
                "Pipeline: format convert %s->%s\n", afmt ? afmt : "?", out_fmt);
    } else {
        INFO_APPEND("No audio\n");
    }

    /* Lossless availability — scan catalog for TrueHD/DTS-HD MA tracks
     * that are present in the container but not currently active. These
     * are only useful over bitstream passthrough; PCM decode of them is
     * the same lossless output as decoded FLAC from any other source. */
    if (ps->fmt_ctx && ps->aud_count > 0) {
        int has_truehd  = 0;
        int has_dtshdma = 0;
        for (int i = 0; i < ps->aud_count; i++) {
            int sidx = ps->aud_stream_indices[i];
            if (sidx < 0 || sidx >= (int)ps->fmt_ctx->nb_streams) continue;
            enum AVCodecID cid = ps->fmt_ctx->streams[sidx]->codecpar->codec_id;
            if (cid == AV_CODEC_ID_TRUEHD)  has_truehd  = 1;
            if (cid == AV_CODEC_ID_DTS)     has_dtshdma = 1;  /* DTS-HD MA shares codec_id with DTS core */
        }
        if (has_truehd || has_dtshdma) {
            const char *names = has_truehd && has_dtshdma ? "TrueHD + DTS-HD"
                              : has_truehd                ? "TrueHD"
                                                          : "DTS-HD";
            INFO_APPEND(
                "Lossless: %s available (bitstream only)\n", names);
        }
    }

    /* Bitstream sink capabilities (if probed). m-S7-d: the Windows stub
     * used to set probed=1 and this block then read "probed, supports
     * nothing" — say what is true instead. */
    if (ps->bitstream_caps.platform_unsupported) {
        INFO_APPEND("\n--- Bitstream Sink ---\nnot probed on this platform\n");
    } else if (ps->bitstream_caps.probed) {
        INFO_APPEND("\n--- Bitstream Sink ---\n");
        INFO_APPEND(
            "AC3=%d EAC3=%d TrueHD=%d DTS=%d DTS-HD=%d HBR=%d ch=%d\n",
            ps->bitstream_caps.support_ac3,
            ps->bitstream_caps.support_eac3,
            ps->bitstream_caps.support_truehd,
            ps->bitstream_caps.support_dts,
            ps->bitstream_caps.support_dtshd,
            ps->bitstream_caps.hbr_capable,
            ps->bitstream_caps.max_channels);
        INFO_APPEND(
            "Mode: %s  Active: %s\n",
            ps->audio_mode == AUDIO_MODE_PCM ? "PCM" :
            ps->audio_mode == AUDIO_MODE_AUTO ? "Auto" : "Passthrough",
            ps->bitstream_active ? "YES" : "no");
    }

    /* Playback diagnostics */
    INFO_APPEND("\n--- Diagnostics ---\n");
    INFO_APPEND("Decoded:     %d\n", ps->diag_frames_decoded);
    INFO_APPEND("Displayed:   %d\n", ps->diag_frames_displayed);
    /* Same formula as the close-out summary (dropped/decoded) — the
     * panel and the log must never disagree about the same number. */
    INFO_APPEND("Dropped:     %d (%.2f%%)\n", ps->diag_frames_dropped,
        ps->diag_frames_decoded > 0
            ? 100.0 * ps->diag_frames_dropped / ps->diag_frames_decoded
            : 0.0);
    INFO_APPEND("Multi-ticks: %d\n", ps->diag_multi_decodes);
    INFO_APPEND("Stall snaps: %d\n", ps->diag_timer_snaps);
    INFO_APPEND("Peak drift:  %.1f ms\n",
        ps->diag_max_av_drift * 1000.0);
    INFO_APPEND("A/V bias:    %.1f ms\n",
        ps->av_bias * 1000.0);
}
