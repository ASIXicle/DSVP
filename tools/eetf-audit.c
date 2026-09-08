/* DSVP x64 EETF audit harness — numeric receipts + fix qualification.
 * Build: gcc -O2 -o eetf-audit eetf-audit.c -lm
 *
 * Lineage: DSVP-deck tools/eetf-audit.c (2026-08-25), adapted to the
 * x64 shader verbatim and extended with the FIX sections. The finding
 * (deck, convicted numerically 2026-08-25; x64 shares the shader
 * lineage): bt2390_eetf is fed LINEAR luminance (E = lin/peak) while
 * the knee uses the PQ-domain formula ks = 1.5*maxLum - 0.5. In PQ
 * domain maxLum = PQ(target)/PQ(peak) is ~0.58-0.9 and the knee lands
 * ~0.65 (everything below ~90 nits passes through untouched); in
 * linear domain maxLum = target/peak is 0.02-0.34, ks clamps to ~0,
 * and the ENTIRE range is splined. On x64 this is the only HDR path
 * on an SDR display, so every HDR file renders through it.
 *
 * REVIEW 2026-09 (M16/M17/M18/M21): the harness now (1) exits non-zero
 * on any FAIL, (2) carries ONE verbatim transcription of the shader's
 * current tone map — clamps, lift and the Y>0 guard included — in
 * shader_tonemap_f32(), and audits THAT against the spec (section S),
 * (3) sweeps peaks at, below and above every target so the clamps are
 * crossed, (4) sweeps black in {off, 0.01 (x64 fixed), target/2000
 * (deck auto)} and prints f(0) next to f(PQ code 1) so the coded-black
 * step is visible, and (5) names the pre-ec97dfb linear-domain model
 * for what it is: a historical receipt, not what ships.
 *
 * Sections:
 *   A: PRE-ec97dfb linear-domain math (historical receipt of the
 *      2026-08-26 conviction) vs BT.2390 as specified, gray ramp.
 *   S: shader@HEAD verbatim transcription vs spec (the live audit).
 *   B: monotonicity / pre-clip overshoot / clip band of shipped curve.
 *   C: FIX variant (a) — direct PQ-domain math in float32 (shader
 *      precision model) vs double-precision spec.
 *   D: FIX variant (b) — per-(peak,target) gain LUT: 1024x1 R16,
 *      sqrt-domain index, sqrt(v) stored and squared in-shader,
 *      bilinear — vs double-precision spec.
 *   E: fix property checks: monotonic, super-peak -> SDR white,
 *      black -> black, knee continuity.
 * Error metric for C/D: worst deviation across the ramp, reported as
 * percent of output and as 8-bit output codes AFTER gamma-2.2 encode
 * (the x64 default output transfer). Acceptance bar: fix variant
 * error <= 0.5 of an 8-bit code across peaks {600,1000,2000,4000,
 * 10000} x targets {203,300,400}.
 *
 * RESULTS BANKED (2026-08-26, persmem run, gcc -O2 — actual output):
 *   A: x64 shipped, target 203: peak=600 mids 100-203 nits render
 *      -15..-19% dark; peak=1000 mids -7..-13% dark then clip band
 *      [342..1000] nits flat; peak=2000: +8..+23% overdriven low-mids
 *      then [255..2000] flat; peak=4000: +11..+44% then [226..4000]
 *      flat; peak=10000: up to +76% then [212..10000] flat. The
 *      "dark mids at modest peaks, overdriven-then-flat at high
 *      peaks" phenotype — no highlight detail above ~1/16 of peak.
 *   B: UNLIKE the deck numbers, the shipped x64 curve is MONOTONIC —
 *      the x64 defensive output clamp (Yt <= maxLum) removes the
 *      deck's brighter-in=darker-out descent and replaces it with the
 *      hard flat ceiling. Pre-clamp overshoot it hides: 1.12x @1000,
 *      1.77x @2000, 3.20x @4000, 7.57x @10000 (x target).
 *   C (variant a, float32 direct): worst 0.007 eight-bit codes
 *      (0.008%, ~34 nits) across all peaks x targets — passes the
 *      0.5-code bar with ~70x margin.
 *   D (variant b, 1024 R16 sqrt/sqrt LUT): worst 0.003 eight-bit
 *      codes (percent error peaks 0.14% only at sub-nit blacks where
 *      code density is ~0) — passes; numerically the BEST variant,
 *      contrary to the pre-run guess that quantization would cost.
 *   E: fixed curve monotonic at all peaks; f(0)=0; f(peak)=1.0;
 *      super-peak (2x peak) clamps to exactly 1.0; knee value-
 *      continuous to 2e-7.
 *   VERDICT: both variants numerically qualified with huge margins.
 *   (a) is the smaller diff (shader-string only, no new bindings,
 *   ~4 extra scalar pow per tone-mapped pixel); (b) is numerically
 *   best and cheapest per-pixel but needs texture+sampler infra x64
 *   does not have. Recommendation: ship (a) first, gate on the
 *   dellbian windowed multi_ticks A/B (standing Intel shader-growth
 *   gate); (b) stays qualified here as the fallback if (a) fails it.
 */
#include <stdio.h>
#include <math.h>

/* ── reference PQ, double precision ── */
static const double m1 = 0.1593017578125, m2 = 78.84375;
static const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;

static double pq_eotf(double pq){            /* code [0,1] -> nits */
    double Np = pow(fmax(pq,0.0), 1.0/m2);
    double num = fmax(Np - c1, 0.0), den = c2 - c3*Np;
    return 10000.0 * pow(fmax(num/den,0.0), 1.0/m1);
}
static double pq_oetf(double nits){          /* nits -> code [0,1] */
    double Np = pow(fmax(nits,0.0)/10000.0, m1);
    return pow((c1 + c2*Np)/(1.0 + c3*Np), m2);
}

/* ── float32 PQ, shader precision model (variant a) ── */
static float pq_eotf_f(float pq){
    float Np = powf(fmaxf(pq,0.0f), (float)(1.0/m2));
    float num = fmaxf(Np - (float)c1, 0.0f), den = (float)c2 - (float)c3*Np;
    return 10000.0f * powf(fmaxf(num/den,0.0f), (float)(1.0/m1));
}
static float pq_oetf_f(float nits){
    float Np = powf(fmaxf(nits,0.0f)/10000.0f, (float)m1);
    return powf(((float)c1 + (float)c2*Np)/(1.0f + (float)c3*Np), (float)m2);
}

/* ── Hermite spline, shared shape (matches shader bt2390_eetf) ── */
static double hermite(double e, double ks, double maxLum){
    if (e <= ks) return e;
    e = fmin(e, 1.0);
    double t = (e - ks)/(1.0 - ks), t2=t*t, t3=t2*t;
    return (2*t3 - 3*t2 + 1)*ks + (t3 - 2*t2 + t)*(1 - ks) + (-2*t3 + 3*t2)*maxLum;
}
static float hermite_f(float e, float ks, float maxLum){
    if (e <= ks) return e;
    e = fminf(e, 1.0f);
    float t = (e - ks)/(1.0f - ks), t2=t*t, t3=t2*t;
    return (2*t3 - 3*t2 + 1)*ks + (t3 - 2*t2 + t)*(1 - ks) + (-2*t3 + 3*t2)*maxLum;
}

/* ── A-input: the PRE-ec97dfb linear-domain math (HISTORICAL RECEIPT
 * of the 2026-08-26 conviction — this is NOT what ships; the shader
 * has run in PQ domain since ec97dfb). Kept so the phenotype numbers
 * banked above stay reproducible. Review M21: the old name
 * "x64_shipped" was a label describing what the code used to do. */
static double x64_pre_ec97dfb_linear(double nits, double peak, double target){
    double E = nits/peak;
    double maxLum = target/peak;
    double ks = fmax(1.5*maxLum - 0.5, 0.0);
    ks = fmin(ks, 0.999);
    double Y_in = fmin(E, 1.0);
    double Yt = hermite(Y_in, ks, maxLum);
    Yt = fmin(fmax(Yt, 0.0), maxLum);        /* x64 defensive output clamp */
    double v = Yt / fmax(maxLum, 0.001);
    return fmin(fmax(v,0.0),1.0);
}

/* ── spec: BT.2390 EETF on PQ-normalized signal (double) ── */
static double spec_tonemap(double nits, double peak, double target){
    double maxE = pq_oetf(peak);
    double e = fmin(pq_oetf(nits)/maxE, 1.0);
    double maxLum = fmin(pq_oetf(target)/maxE, 1.0);          /* shader: min(.., 1.0) */
    double ks = fmin(fmax(1.5*maxLum - 0.5, 0.0), 0.999);      /* shader: clamp(.., 0, 0.999) */
    double eo = hermite(e, ks, maxLum);
    eo = fmin(fmax(eo, 0.0), maxLum);        /* keep the defensive clamp */
    double out_nits = pq_eotf(eo*maxE);
    double v = out_nits/target;
    return fmin(fmax(v,0.0),1.0);
}

/* ── FIX variant (a): same math in float32, uniforms precomputed on
 * CPU in double then narrowed (as the C side would set them) ── */
static double fix_direct_f32(double nits, double peak, double target){
    float maxE   = (float)pq_oetf(peak);            /* CPU-side uniform */
    float maxLum = fminf((float)(pq_oetf(target)/pq_oetf(peak)), 1.0f);
    float ks     = fminf(fmaxf(1.5f*maxLum - 0.5f, 0.0f), 0.999f);
    float e = fminf(pq_oetf_f((float)nits)/maxE, 1.0f);
    float eo = hermite_f(e, ks, maxLum);
    eo = fminf(fmaxf(eo, 0.0f), maxLum);
    float out_nits = pq_eotf_f(eo*maxE);
    float v = out_nits/(float)target;
    return fmin(fmax((double)v,0.0),1.0);
}

/* ── FIX variant (b): gain LUT. Build tab[i] = sqrt(v_spec(Y)) with
 * Y = (i/1023)^2 (sqrt-domain samples), quantize R16; shader samples
 * at u = sqrt(Y_in) with the half-texel mapping + bilinear, squares. */
static unsigned short lut_tab[1024];
static void lut_build(double peak, double target){
    for (int i=0;i<1024;i++){
        double u = (double)i/1023.0;
        double Y = u*u;                       /* E-normalized luma */
        double v = spec_tonemap(Y*peak, peak, target);
        double s = sqrt(fmin(fmax(v,0.0),1.0));
        lut_tab[i] = (unsigned short)lround(s*65535.0);
    }
}
static double fix_lut(double nits, double peak, double target){
    (void)target;
    double Y_in = fmin(nits/peak, 1.0);
    double s = sqrt(Y_in);
    double u = s*(1023.0/1024.0) + 0.5/1024.0;
    double tex = u*1024.0 - 0.5;
    int i0 = (int)floor(tex); double fr = tex - i0;
    if (i0 < 0){ i0=0; fr=0; } if (i0 > 1022){ i0=1022; fr=1; }
    double g = ((1-fr)*lut_tab[i0] + fr*lut_tab[i0+1])/65535.0;
    double v = g*g;
    return fmin(fmax(v,0.0),1.0);
}

/* gamma-2.2 8-bit output code (x64 default output transfer) */
static double code8(double v){
    return pow(fmin(fmax(v,0.0),1.0), 1.0/2.2) * 255.0;
}

/* ═══ BT.2390 black-level lift (deck 4614882 port, review PQUALITY;
 * Holden: "reference means lift"). Mirrors the shader's bt2390_lift:
 * e3 = e2 + minL*(1-e2)^4 with (1-e2)^4 by squaring, minL =
 * PQ(black)/PQ(peak), applied AFTER the spline's [0,maxLum] clamp
 * and BEFORE denormalization. Returns nits. ═══ */
static double spec_lift(double nits, double peak, double target, double black){
    double maxE = pq_oetf(peak);
    double maxLum = fmin(pq_oetf(target)/maxE, 1.0);
    double ks = fmin(fmax(1.5*maxLum - 0.5, 0.0), 0.999);
    double e = fmin(pq_oetf(nits)/maxE, 1.0);
    double eo = fmin(fmax(hermite(e, ks, maxLum), 0.0), maxLum);
    double minL = pq_oetf(black)/maxE;
    double u = 1.0 - eo; u *= u; u *= u;
    eo += minL*u;
    return pq_eotf(eo*maxE);
}

/* ═══ shader_tonemap_f32: VERBATIM transcription of the HDR10/HLG
 * tone-map branch in src/player.c (the `else` block after the debug
 * modes: maxE, maxLum, ks, e, e2 = bt2390_lift(bt2390_eetf(e)),
 * Yt_nits, rgb_tm with the Y > 0.0 guard), float32, gray pixel, then
 * encode is left to the caller. Any edit to those shader lines must be
 * mirrored HERE — section S is the audit that catches drift. Inputs:
 * nits (display-linear luminance of the source pixel), peak, target,
 * black: <0 lift off, 0 auto (target/2000), >0 fixed nits. Returns
 * display-linear output normalised to target (1.0 = target). ═══ */
static float shader_bt2390_eetf(float e, float ks, float maxLum){
    if (e <= ks) return e;
    float t = (e - ks) / (1.0f - ks);
    float t2 = t * t;
    float t3 = t2 * t;
    float y = (2.0f*t3 - 3.0f*t2 + 1.0f) * ks
            + (t3 - 2.0f*t2 + t) * (1.0f - ks)
            + (-2.0f*t3 + 3.0f*t2) * maxLum;
    return fminf(fmaxf(y, 0.0f), maxLum);
}
static float shader_bt2390_lift(float e2, float target, float maxE, float black){
    if (black < 0.0f) return e2;
    float b = (black > 0.0f) ? black : target / 2000.0f;
    float minL = pq_oetf_f(b / 10000.0f * 10000.0f) / maxE;   /* pq_oetf1(b/10000) */
    float u = 1.0f - e2;
    u = u * u; u = u * u;
    return e2 + minL * u;
}
static double shader_tonemap_f32(double nits_d, double peak_d, double target_d, double black_d){
    float peak = (float)peak_d, target = (float)target_d, black = (float)black_d;
    float maxE   = pq_oetf_f(peak);                              /* pq_oetf1(hdr_peak_nits/10000) */
    float maxLum = fminf(pq_oetf_f(target) / maxE, 1.0f);
    float ks     = fminf(fmaxf(1.5f * maxLum - 0.5f, 0.0f), 0.999f);
    float Y      = (float)nits_d;                                /* dot(lin, lc) for gray = nits */
    float e      = fminf(pq_oetf_f(Y) / maxE, 1.0f);
    float e2     = shader_bt2390_lift(shader_bt2390_eetf(e, ks, maxLum), target, maxE, black);
    float Yt_nits = pq_eotf_f(e2 * maxE);
    /* rgb_tm = (Y > 0.0) ? (lin/target)*(Yt_nits/Y) : 0 — for gray lin==Y */
    float v = (Y > 0.0f) ? Yt_nits / target : 0.0f;
    return (double)v;
}
/* spec with lift, normalised to target, same guard semantics stated explicitly */
static double spec_lift_norm(double nits, double peak, double target, double black){
    if (black < 0.0) return spec_tonemap(nits, peak, target);
    double b = (black > 0.0) ? black : target/2000.0;
    return fmin(fmax(spec_lift(nits, peak, target, b)/target, 0.0), 1.0);
}
static double code8_mid(double v, double midtone){   /* 8-bit code after the default midtone gain 1.3 */
    v = fmin(fmax(v,0.0),1.0);
    return pow(pow(v, 1.0/midtone), 1.0/2.2) * 255.0;
}

static int g_fails = 0;
/* A function, not a macro with a side effect: two FAILMARKs in one
 * printf argument list would be a sequence-point hazard. */
static const char *FAILMARK(int cond){ if (!cond) g_fails++; return cond ? "" : "  ** FAIL"; }

int main(void){
    /* Peaks BELOW, AT and ABOVE every target in the T ladder, so the
     * maxLum/ks clamps are actually crossed (review M17/S6-9). */
    double peaks[]   = {100, 203, 300, 400, 600, 1000, 2000, 4000, 10000};
    const int NP = 9;
    double targets[] = {203, 300, 400};

    printf("== A: x64 PRE-ec97dfb linear-domain (HISTORICAL RECEIPT, not shipped) vs spec (PQ-domain) ==\n");
    printf("target=203; display-linear out (1.0 = target), ratio old/spec\n");
    for (int p=0;p<NP;p++){
        double peak=peaks[p], target=203;
        double ml=target/peak, ks=fmin(fmax(1.5*ml-0.5,0.0),0.999);
        printf("\n-- peak=%.0f target=%.0f  maxLum(lin)=%.4f ks(ship)=%.4f "
               "maxLum(pq)=%.4f ks(spec)=%.4f --\n",
               peak,target,ml,ks,
               pq_oetf(target)/pq_oetf(peak),
               1.5*pq_oetf(target)/pq_oetf(peak)-0.5);
        printf("%10s %12s %12s %9s\n","in nits","ship","spec","ratio");
        double probe[]={1,5,10,25,50,100,150,203,300,400,peak*0.5,peak*0.75,peak};
        for (int i=0;i<13;i++){
            double n=probe[i]; if(n>peak) continue;
            double a=x64_pre_ec97dfb_linear(n,peak,target);
            double b=spec_tonemap(n,peak,target);
            printf("%10.1f %12.4f %12.4f %8.2f%%\n",
                   n,a,b, b>0? 100.0*a/b : 0.0);
        }
    }

    printf("\n== B: monotonicity / overshoot / clip band of the PRE-ec97dfb curve (receipt) ==\n");
    for (int p=0;p<NP;p++){
        double peak=peaks[p], target=203;
        double prev=-1, worst=0, worst_at=0; int nonmono=0; double nm_at=0;
        double clip_lo=peak, clip_hi=0;
        for (int i=0;i<=4000;i++){
            double n = peak*i/4000.0;
            /* pre-saturate value, WITHOUT the x64 output clamp — shows
             * the overshoot the clamp was added to hide */
            double E=n/peak, ml=target/peak;
            double ks=fmin(fmax(1.5*ml-0.5,0.0),0.999);
            double Yt=hermite(fmin(E,1.0),ks,ml);
            double v=Yt/fmax(ml,0.001);
            if (v>worst){worst=v;worst_at=n;}
            double vc=x64_pre_ec97dfb_linear(n,peak,target);
            if (vc<prev-1e-12 && !nonmono){nonmono=1;nm_at=n;}
            if (vc>=1.0){ if(n<clip_lo) clip_lo=n; if(n>clip_hi) clip_hi=n; }
            prev=vc;
        }
        printf("peak=%5.0f: pre-clamp max=%.3fx target at %.0f nits; "
               "non-monotonic from %.0f nits: %s; clipped band [%.0f..%.0f]\n",
               peak, worst, worst_at, nm_at, nonmono?"YES":"no",
               clip_lo<=clip_hi?clip_lo:0, clip_hi);
    }

    printf("\n== S: shader@HEAD verbatim transcription (f32, lift OFF) vs spec (double) ==\n");
    printf("the live audit: any shader edit not mirrored in shader_tonemap_f32() shows here\n");
    double worst_s_all=0;
    for (int p=0;p<NP;p++) for (int t=0;t<3;t++){
        double peak=peaks[p], target=targets[t];
        double wc=0,wc_at=0;
        for (int i=0;i<=40000;i++){
            double n=peak*i/40000.0;
            double a=shader_tonemap_f32(n,peak,target,-1.0);
            double b=spec_tonemap(n,peak,target);
            double dc=fabs(code8(a)-code8(b));
            if(dc>wc){wc=dc;wc_at=n;}
        }
        double maxE=pq_oetf(peak);
        double maxLum=fmin(pq_oetf(target)/maxE,1.0), ks=fmin(fmax(1.5*maxLum-0.5,0.0),0.999);
        printf("peak=%5.0f target=%3.0f: maxLum=%.4f ks=%.3f%s worst %.3f codes (at %.1f nits)%s\n",
               peak,target,maxLum,ks, (peak<=target)?" (clamped)":"", wc,wc_at, FAILMARK(wc<=0.5));
        if(wc>worst_s_all)worst_s_all=wc;
    }
    printf("shader vs spec worst overall: %.3f codes — %s the 0.5-code bar\n",
           worst_s_all, worst_s_all<=0.5?"PASSES":"FAILS");

    printf("\n== C: FIX variant (a) float32 direct vs spec (double) ==\n");
    printf("worst error across 0..peak ramp (40001 samples)\n");
    double worst_a_all=0;
    for (int p=0;p<NP;p++) for (int t=0;t<3;t++){
        double peak=peaks[p], target=targets[t];
        double wpct=0,wp_at=0,wc=0,wc_at=0;
        for (int i=0;i<=40000;i++){
            double n=peak*i/40000.0;
            double a=fix_direct_f32(n,peak,target);
            double b=spec_tonemap(n,peak,target);
            double dpct = b>1e-6 ? 100.0*fabs(a-b)/b : 0.0;
            double dc = fabs(code8(a)-code8(b));
            if(dpct>wpct){wpct=dpct;wp_at=n;}
            if(dc>wc){wc=dc;wc_at=n;}
        }
        printf("peak=%5.0f target=%3.0f: worst %.3f%% (at %.1f nits), "
               "%.3f 8-bit codes (at %.1f nits)%s\n",
               peak,target,wpct,wp_at,wc,wc_at, FAILMARK(wc<=0.5));
        if(wc>worst_a_all)worst_a_all=wc;
    }
    printf("variant (a) worst overall: %.3f codes — %s the 0.5-code bar\n",
           worst_a_all, worst_a_all<=0.5?"PASSES":"FAILS");

    printf("\n== D: FIX variant (b) 1024 R16 sqrt/sqrt LUT vs spec ==\n");
    double worst_b_all=0;
    for (int p=0;p<NP;p++) for (int t=0;t<3;t++){
        double peak=peaks[p], target=targets[t];
        lut_build(peak,target);
        double wpct=0,wp_at=0,wc=0,wc_at=0;
        for (int i=0;i<=40000;i++){
            double n=peak*i/40000.0;
            double a=fix_lut(n,peak,target);
            double b=spec_tonemap(n,peak,target);
            double dpct = b>1e-6 ? 100.0*fabs(a-b)/b : 0.0;
            double dc = fabs(code8(a)-code8(b));
            if(dpct>wpct){wpct=dpct;wp_at=n;}
            if(dc>wc){wc=dc;wc_at=n;}
        }
        printf("peak=%5.0f target=%3.0f: worst %.3f%% (at %.1f nits), "
               "%.3f 8-bit codes (at %.1f nits)%s\n",
               peak,target,wpct,wp_at,wc,wc_at, FAILMARK(wc<=0.5));
        if(wc>worst_b_all)worst_b_all=wc;
    }
    printf("variant (b) worst overall: %.3f codes — %s the 0.5-code bar\n",
           worst_b_all, worst_b_all<=0.5?"PASSES":"FAILS");

    printf("\n== E: fixed-curve properties (spec map, double) ==\n");
    for (int p=0;p<NP;p++){
        double peak=peaks[p], target=203;
        double prev=-1; int mono=1;
        for (int i=0;i<=40000;i++){
            double n=peak*i/40000.0;
            double v=spec_tonemap(n,peak,target);
            if (v<prev-1e-12) mono=0;
            prev=v;
        }
        /* knee continuity: value AND slope across e=ks (review m-S6-e:
         * the old check was value-only on a function continuous by
         * construction). Left slope is 1 (identity); right slope from
         * the spline's first derivative at t=0 must also be 1. */
        double maxE=pq_oetf(peak), maxLum=fmin(pq_oetf(target)/maxE,1.0);
        double ks=fmin(fmax(1.5*maxLum-0.5,0.0),0.999);
        double h=1e-6;
        double dval = fabs(hermite(ks+h,ks,maxLum)-hermite(ks-h,ks,maxLum));
        double sl = (hermite(ks,ks,maxLum)-hermite(ks-h,ks,maxLum))/h;
        double sr = (hermite(ks+h,ks,maxLum)-hermite(ks,ks,maxLum))/h;
        int knee_ok = dval < 1e-5 && fabs(sl-sr) < 1e-3;
        printf("peak=%5.0f: monotonic=%s  f(0)=%.6f  f(peak)=%.6f  "
               "f(2*peak clamped)=%.6f  knee value-gap=%.2e slope L/R=%.4f/%.4f%s%s\n",
               peak, mono?"yes":"NO",
               spec_tonemap(0,peak,target),
               spec_tonemap(peak,peak,target),
               spec_tonemap(2*peak,peak,target), dval, sl, sr,
               FAILMARK(mono), FAILMARK(knee_ok));
    }
    printf("\n== H: BT.2390 black-level lift — shader transcription (with the Y>0 guard) vs spec ==\n");
    printf("black in {off, 0.01 (x64 FIXED default), target/2000 (deck auto)}; target 203\n");
    printf("f(0) = shader output at exact coded black (guard -> 0); f(code1) = at PQ 10-bit code 1\n");
    printf("(4.3e-5 nits) — the step between them, in 8-bit codes at gamma 2.2 and with midtone 1.3\n");
    double blacks[] = {-1.0, 0.01, 203.0/2000.0};
    const char *bname[] = {"off", "0.0100 (x64 fixed)", "0.1015 (deck auto)"};
    double code1_nits = pq_eotf(1.0/1023.0);   /* PQ 10-bit code 1, full range */
    for (int b=0;b<3;b++) for (int p=0;p<NP;p++){
        double peak=peaks[p], target=203, black=blacks[b];
        double prev=-1; int mono=1; double w8=0;
        for (int i=1;i<=40000;i++){             /* i=1: the guard makes f(0) exact 0 by design */
            double n=peak*i/40000.0;
            double a=shader_tonemap_f32(n,peak,target,black);
            double bb=spec_lift_norm(n,peak,target,black);
            /* float32 model: powf round-trips through pq_oetf/pq_eotf
             * jitter by a few 1e-5 (normalised) at the shoulder — the
             * same noise the S section reports as ~0.005 codes. The
             * monotonic check is for CURVE descents (percent-level, the
             * deck's 2026-08 finding), so the tolerance is 1e-4 ≈ 0.01
             * of an 8-bit code. */
            if(a<prev-1e-4)mono=0;
            prev=a;
            double d=fabs(code8(a)-code8(bb));
            if(d>w8)w8=d;
        }
        double f0 = shader_tonemap_f32(0.0,peak,target,black);
        double f1 = shader_tonemap_f32(code1_nits,peak,target,black);
        printf("black=%-19s peak=%5.0f: mono=%s f(0)=%.4f nits (code %.1f) f(code1)=%.4f nits "
               "(code %.1f; %.1f w/ midtone 1.3) -> step %.1f codes; f32 vs spec worst %.4f codes%s%s\n",
               bname[b], peak, mono?"yes":"NO",
               f0*target, code8(f0), f1*target, code8(f1), code8_mid(f1,1.3),
               code8(f1)-code8(f0), w8, FAILMARK(mono), FAILMARK(w8<=0.5));
    }
    (void)targets;
    printf("\n%s\n", g_fails ? "EETF AUDIT: FAIL" : "EETF AUDIT: PASS");
    if (g_fails) printf("EETF AUDIT: %d FAIL\n", g_fails);
    return g_fails ? 1 : 0;
}
