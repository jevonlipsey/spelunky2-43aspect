/* host test for the ui_2d algorithm. alg.inc is generated from mod_43aspect.c
 * by tools/gen_alg.sh, so this runs the shipped code. numbers come from the
 * pin8 device log (run10_pin8.log).
 *   tools/gen_alg.sh && cc -O2 -Wall tools/test_alg.c -o /tmp/test_alg -lm && /tmp/test_alg */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "alg.inc"

#define SY 0.75f
static uint8_t buf[0x18 * 4096];
static int nv = 0;
static int last_pins = 0, last_scaled = 0, last_vpq = 4;
static uint8_t rec[0x18 * 8192];
static int recn = 0;
static int checking = 0, fails = 0;

#define CHECK(c, ...) do { if (checking && !(c)) { fails++; printf("    FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define SAY(...) do { if (checking) { printf("    " __VA_ARGS__); printf("\n"); } } while (0)

static int near(float a, float b, float t) { return fabsf(a - b) <= t; }

static void reset_chain(void) {
    memset(g_ui_2d_chx0, 0, sizeof g_ui_2d_chx0);
    memset(g_ui_2d_chx1, 0, sizeof g_ui_2d_chx1);
    memset(g_ui_2d_chcy, 0, sizeof g_ui_2d_chcy);
    memset(g_ui_2d_chanchor, 0, sizeof g_ui_2d_chanchor);
    memset(g_ui_2d_chused, 0, sizeof g_ui_2d_chused);
    g_ui_2d_chnext = 0;
}

static void vert(float x, float y) {
    uint8_t* v = buf + (size_t)nv * 0x18;
    memset(v, 0, 0x18);
    memcpy(v, &x, 4);
    memcpy(v + 4, &y, 4);
    *(uint32_t*)(v + 0x14) = 0xffffffffu;
    nv++;
}
/* tri: 6 verts (two triangles); else 4 indexed verts */
static void quad(int tri, float x0, float x1, float y0, float y1) {
    if (tri) { vert(x0, y1); vert(x1, y1); vert(x0, y0); vert(x1, y1); vert(x1, y0); vert(x0, y0); }
    else     { vert(x0, y1); vert(x1, y1); vert(x0, y0); vert(x1, y0); }
}
static void quadc(int tri, float x0, float x1, float cy, float h) { quad(tri, x0, x1, cy - h * 0.5f, cy + h * 0.5f); }

/* same vpq rule as ui_2d_scale_batch */
static void set_vpq(int n, int nidx) {
    g_ui_2d_vpq = 4;
    if (nidx > 0 && (nidx % 6) == 0) {
        int v = n * 6 / nidx;
        if (v >= 3 && v <= 6) g_ui_2d_vpq = v;
    }
}
/* nidx: 0 = derive (tri list idx = n, indexed idx = 1.5n) */
static int flush(int tri, int nidx) {
    if (!nidx) nidx = tri ? nv : nv / 4 * 6;
    set_vpq(nv, nidx);
    last_vpq = g_ui_2d_vpq;
    last_pins = ui_2d_batch_pins(buf, nv);
    last_scaled = ui_2d_apply_runs(buf, nv, SY, 1.0f);
    memcpy(rec + recn, buf, (size_t)nv * 0x18);
    recn += nv * 0x18;
    return last_scaled;
}
static float qcy(int i) {
    float s = 0; int k;
    for (k = 0; k < last_vpq; k++) s += ui_2d_ldf(buf + (size_t)(i * last_vpq + k) * 0x18 + 4);
    return s / last_vpq;
}
static float anchor_of(float y, float ny) { return (ny - y * SY) / (1.0f - SY); } /* ny = a + (y-a)*sy */

/* ---- T1 rope tear ---- */
static void t1(void) {
    static const float seg[] = {-0.407f, -0.492f, -0.535f, -0.620f, -0.662f, -0.748f, -0.790f,
                                -0.876f, -0.919f, -0.983f, -1.025f, -1.110f};
    float out[12], ys[] = {0.80f, 0.60f, 0.40f, 0.20f, 0.0f, -0.20f};
    int i;
    reset_chain();
    /* seed: n 8 flush, rope top at 0.998 plus a segment of another column */
    nv = 0;
    quadc(0, 0.397f, 0.420f, 0.998f, 0.040f);
    quadc(0, 0.297f, 0.314f, 0.600f, 0.040f);
    flush(0, 0);
    for (i = 0; i < 6; i++) { nv = 0; quadc(0, 0.397f, 0.420f, ys[i], 0.040f); flush(0, 0); }
    for (i = 0; i < 12; i++) {
        float want;
        nv = 0;
        quadc(0, 0.397f, 0.420f, seg[i], 0.040f);
        flush(0, 0);
        out[i] = qcy(0);
        want = 0.998f + (seg[i] - 0.998f) * SY;
        CHECK(near(out[i], want, 0.001f), "seg %.3f -> %.4f, want %.4f (anchor 0.998)", seg[i], out[i], want);
        if (i == 7) {
            float old = -1.0f + (seg[i] + 1.0f) * SY;
            SAY("-0.876: batch_pins=%d (old 'else if (!pins)' would anchor -1.000)", last_pins);
            SAY("-0.876 -> old behaviour %.4f, fixed %.4f", old, out[i]);
            CHECK(last_pins == 1, "-0.876 batch should pin so the old rule would have fired");
            CHECK(near(old, -0.907f, 0.001f), "old expectation %.4f != -0.907", old);
            CHECK(near(out[i], -0.4075f, 0.001f), "fixed %.4f != -0.4075", out[i]);
        }
    }
    for (i = 1; i < 12; i++) {
        float g_in = seg[i - 1] - seg[i], g_out = out[i - 1] - out[i];
        CHECK(out[i] < out[i - 1], "not monotonic at %d", i);
        CHECK(near(g_out, g_in * SY, 0.0005f), "gap %d: %.4f want %.4f", i, g_out, g_in * SY);
    }
}

/* ---- T2 pinned-top column ---- */
static void t2(void) {
    static const float s[] = {0.975f, 0.880f, 0.858f, 0.764f, 0.680f, 0.590f, 0.500f, 0.400f, 0.300f, 0.200f, 0.105f};
    int i;
    reset_chain();
    for (i = 0; i < 11; i++) {
        float want = 1.0f + (s[i] - 1.0f) * SY;
        nv = 0;
        quadc(0, -1.034f, -1.017f, s[i], 0.040f);
        flush(0, 0);
        CHECK(near(qcy(0), want, 0.001f), "col seg %.3f -> %.4f want %.4f (anchor +1)", s[i], qcy(0), want);
    }
}

/* ---- T3 hud ---- */
static void t3(void) {
    static const int tri[] = {0, 1, 1, 1};
    static const int nq[] = {1, 1, 3, 5};   /* n = 4/6/18/30 */
    int c, i, pass;
    reset_chain();
    for (c = 0; c < 4; c++) {
        float lo, hi;
        nv = 0;
        for (i = 0; i < nq[c]; i++) quad(tri[c], 0.45f + 0.05f * i, 0.49f + 0.05f * i, 0.86f, 0.94f);
        flush(tri[c], 0);
        ui_2d_group_ry(buf, nv, 0, &lo, &hi);
        CHECK(last_pins == 1, "hud n %d not pinned", nv);
        CHECK(near(lo, 0.895f, 0.001f) && near(hi, 0.955f, 0.001f), "hud n %d y %.4f..%.4f want 0.895..0.955", nv, lo, hi);
    }
    reset_chain();
    for (pass = 0; pass < 2; pass++) {   /* fresh chain, then previous-frame self-match */
        float lo, hi;
        nv = 0;
        quad(0, 0.884f, 0.925f, -0.936f, -0.864f);
        flush(0, 0);
        ui_2d_group_ry(buf, nv, 0, &lo, &hi);
        CHECK(near(lo, -0.952f, 0.001f) && near(hi, -0.898f, 0.001f), "bottom hud pass %d y %.4f..%.4f want -0.952..-0.898", pass, lo, hi);
    }
}

/* ---- T4 hud digits ---- */
static void t4(void) {
    int pass;
    reset_chain();
    for (pass = 0; pass < 2; pass++) {
        nv = 0;
        quadc(0, 0.951f, 0.968f, 0.90f, 0.04f);
        quadc(0, 0.968f, 0.985f, 0.90f, 0.04f);
        flush(0, 0);
        CHECK(near(qcy(0), 0.925f, 0.001f) && near(qcy(1), 0.925f, 0.001f), "digit pass %d -> %.4f %.4f want 0.925 (pinned top)", pass, qcy(0), qcy(1));
    }
    /* a digit that is not at the edge keeps its own centre, and a lone one still does the next frame */
    for (pass = 0; pass < 2; pass++) {
        nv = 0;
        quadc(0, 0.951f, 0.968f, 0.50f, 0.04f);
        flush(0, 0);
        CHECK(near(qcy(0), 0.50f, 0.001f), "mid digit pass %d moved to %.4f", pass, qcy(0));
    }
}

/* ---- T5 bbox vs mean ---- */
static void row(int tri, float cy, int nl, float h, const float* jit, int nj, float x0) {
    int i;
    for (i = 0; i < nl; i++) quadc(tri, x0 + 0.025f * i, x0 + 0.025f * i + 0.02f, cy + (i < nj ? jit[i] : 0.0f), h);
}
static void cluster_stats(int ci_expect, const float* base, float tolbase, const char* name) {
    int ng = (nv + g_ui_2d_vpq - 1) / g_ui_2d_vpq, g = 0, ci = 0;
    while (g < ng) {
        float mean, tmin = 9e9f, tmax = -9e9f, bbc, px;
        int g1 = ui_2d_cluster(buf, nv, g, &mean), j;
        for (j = g * g_ui_2d_vpq; j < g1 * g_ui_2d_vpq && j < nv; j++) {
            float y = ui_2d_ldf(buf + (size_t)j * 0x18 + 4);
            if (y < tmin) tmin = y;
            if (y > tmax) tmax = y;
        }
        bbc = (tmin + tmax) * 0.5f;
        px = fabsf(bbc - mean) * 0.25f * 480.0f;
        CHECK(ci < ci_expect && near(mean, base[ci], tolbase), "%s cluster %d mean %.4f want %.3f", name, ci, mean, ci < ci_expect ? base[ci] : 0);
        CHECK(px < 1.0f, "%s row %d |bbox-mean| = %.2f px", name, ci, px);
        SAY("%s row %d: mean %.4f bbox %.4f -> %.2f px", name, ci, mean, bbc, px);
        ci++; g = g1;
    }
    CHECK(ci == ci_expect, "%s: %d clusters, want %d", name, ci, ci_expect);
}
static void t5(void) {
    static const float pb[] = {0.350f, 0.213f, 0.071f, -0.069f, -0.206f, -0.344f};
    static const float pl[] = {4, 9, 9, 9, 9, 9};
    static const float j0[] = {0.004f, 0.006f, -0.001f, -0.009f};
    static const float j1[] = {0.001f, -0.004f, 0.003f, 0.005f, -0.004f, -0.004f};
    static const float cb[] = {0.141f, 0.006f, -0.140f};
    static const float cl[] = {9, 7, 5};
    static const float j2[] = {0.004f, 0.005f, -0.002f, -0.002f, -0.002f, 0.002f, -0.003f, -0.002f, -0.002f};
    int r;
    reset_chain();
    nv = 0;
    for (r = 0; r < 6; r++) row(1, pb[r], (int)pl[r], 0.069f, r == 0 ? j0 : (r == 1 ? j1 : 0), r == 0 ? 4 : (r == 1 ? 6 : 0), -0.12f);
    set_vpq(nv, nv);
    cluster_stats(6, pb, 0.01f, "pause");
    nv = 0;
    for (r = 0; r < 3; r++) row(1, cb[r], (int)cl[r], 0.055f, r == 0 ? j2 : 0, r == 0 ? 9 : 0, -0.10f);
    set_vpq(nv, nv);
    cluster_stats(3, cb, 0.01f, "confirm");
    /* shape 222: 16 glyphs at 0.775 (0.733..0.817) + degenerate space at 0.844 */
    nv = 0;
    {
        int i;
        for (i = 0; i < 17; i++) {
            if (i == 6) quad(1, -0.338f + 0.04f * i, -0.318f + 0.04f * i, 0.844f, 0.844f);
            else quadc(1, -0.338f + 0.04f * i, -0.318f + 0.04f * i, 0.775f, 0.084f);
        }
        set_vpq(nv, nv);
        {
            float mean, tmin = 9e9f, tmax = -9e9f; int j;
            ui_2d_cluster(buf, nv, 0, &mean);
            for (j = 0; j < nv; j++) { float y = ui_2d_ldf(buf + (size_t)j * 0x18 + 4); if (y < tmin) tmin = y; if (y > tmax) tmax = y; }
            SAY("shape 222: glyph 0.775, mean %.4f, bbox %.4f", mean, (tmin + tmax) * 0.5f);
            CHECK(fabsf(mean - 0.775f) < fabsf((tmin + tmax) * 0.5f - 0.775f), "mean not closer to glyph centre than bbox");
        }
    }
}

/* ---- T6 shape 79 straddle ---- */
static void t6(void) {
    int k, i, bars = g_ui_2d_bars;
    float orig[24], maxd = 0, mind = 9;
    reset_chain();
    nv = 0;
    for (k = 0; k < 6; k++) {
        float c = 0.279f - 0.140f * k;
        for (i = 0; i < 4; i++) {
            float cy = c + ((i & 1) ? 0.0715f : -0.0715f);
            orig[k * 4 + i] = cy;
            quadc(0, (i & 1) ? 0.0f : -0.905f, (i & 1) ? 0.905f : 0.0f, cy, 0.24f);
        }
    }
    flush(0, 144);
    CHECK(last_vpq == 4, "vpq %d", last_vpq);
    for (i = 0; i < 24; i++) {
        float d = fabsf(qcy(i) - orig[i]);
        if (d > maxd) maxd = d;
        if (d < mind) mind = d;
    }
    SAY("bars=%d: displacement min %.4f max %.4f ndc (%.1f px at 480)", bars, mind, maxd, maxd * 0.5f * 960.0f);
    if (!bars) {
        CHECK(mind > 0.016f && maxd < 0.020f, "pin8 displacement should be ~0.018, got %.4f..%.4f", mind, maxd);
    } else {
        CHECK(maxd < 0.0005f, "bars=1 still moves a quad by %.4f", maxd);
        for (k = 0; k < 6; k++) {   /* low/high pair of a level still overlap after scaling 0.75 about own centre */
            float top_lo = qcy(k * 4) + 0.12f * SY;   /* half of 0.24 * 0.75 */
            float bot_hi = qcy(k * 4 + 1) - 0.12f * SY;
            CHECK(top_lo > bot_hi, "level %d no longer overlaps (%.4f vs %.4f)", k, top_lo, bot_hi);
        }
    }
}

/* ---- T7 regressions ---- */
static void t7(void) {
    int sv, f, r, i, saved = g_ui_2d_fill;
    float ex[6] = {0.36f, 0.22f, 0.08f, -0.06f, -0.20f, -0.34f};
    reset_chain();
    for (f = 1; f >= 0; f--) {
        g_ui_2d_fill = f;
        nv = 0; quad(0, -0.2f, 0.2f, 99.0f, 103.0f);
        sv = flush(0, 0);
        CHECK(sv == 0, "world batch scaled %d verts (fill %d)", sv, f);
    }
    g_ui_2d_fill = 1;
    nv = 0; quad(0, -1.0f, 0.067f, -1.0f, 0.896f);
    sv = flush(0, 0);
    CHECK(sv == 0, "cover quad scaled %d", sv);
    for (f = 1; f >= 0; f--) {
        g_ui_2d_fill = f;
        nv = 0; quad(0, -0.5f, 0.5f, -1.2f, 0.5f);
        sv = flush(0, 0);
        CHECK(sv == (f ? 4 : 2), "overshoot column scaled %d/4 (fill %d)", sv, f);
    }
    g_ui_2d_fill = saved;
    /* 6 x 9 triangle-list text block */
    reset_chain();
    nv = 0;
    for (r = 0; r < 6; r++) row(1, ex[r], 9, 0.069f, 0, 0, -0.12f);
    flush(1, 0);
    CHECK(last_vpq == 6, "vpq %d", last_vpq);
    for (r = 0; r < 6; r++) {
        float s = 0;
        for (i = 0; i < 9; i++) s += qcy(r * 9 + i);
        CHECK(near(s / 9.0f, ex[r], 0.002f), "row %d centre %.4f want %.2f", r, s / 9.0f, ex[r]);
    }
    /* four-letter title + first item */
    nv = 0;
    row(1, 0.354f, 4, 0.069f, 0, 0, -0.06f);
    row(1, 0.214f, 9, 0.069f, 0, 0, -0.12f);
    set_vpq(nv, nv);
    {
        int ng = (nv + 5) / 6, g = 0, cnt = 0; float m[4];
        while (g < ng && cnt < 4) g = ui_2d_cluster(buf, nv, g, &m[cnt++]);
        CHECK(cnt == 2 && near(m[0], 0.354f, 0.002f) && near(m[1], 0.214f, 0.002f), "title split: %d clusters %.3f %.3f", cnt, m[0], m[1]);
    }
    /* two ropes in one flush keep their own anchor, either order */
    for (r = 0; r < 2; r++) {
        reset_chain();
        nv = 0; quadc(0, 0.397f, 0.420f, 0.998f, 0.04f); quadc(0, -0.5f, -0.2f, 0.2f, 0.1f); flush(0, 0);
        nv = 0; quadc(0, -1.034f, -1.017f, 0.60f, 0.04f); quadc(0, -0.5f, -0.2f, -0.4f, 0.1f); flush(0, 0);
        nv = 0;
        if (r == 0) { quadc(0, 0.397f, 0.420f, 0.78f, 0.04f); quadc(0, -1.034f, -1.017f, 0.45f, 0.04f); }
        else        { quadc(0, -1.034f, -1.017f, 0.45f, 0.04f); quadc(0, 0.397f, 0.420f, 0.78f, 0.04f); }
        flush(0, 0);
        {
            float a = qcy(r == 0 ? 0 : 1), b = qcy(r == 0 ? 1 : 0);
            CHECK(near(a, 0.998f + (0.78f - 0.998f) * SY, 0.001f), "order %d rope A %.4f", r, a);
            CHECK(near(b, 0.60f + (0.45f - 0.60f) * SY, 0.001f), "order %d rope B %.4f", r, b);
        }
    }
}

/* ---- T9 rope strip (run 10: shape 272 was cut between every segment) ---- */
static int count_clusters(void) {
    int ng = (nv + g_ui_2d_vpq - 1) / g_ui_2d_vpq, g = 0, c = 0;
    while (g < ng) { float m; g = ui_2d_cluster(buf, nv, g, &m); c++; }
    return c;
}
static void rope(int nq, float pitch, float hw, float h) {
    int k;
    nv = 0;
    for (k = 0; k < nq; k++) quadc(0, -hw, hw, (k - (nq - 1) / 2) * pitch, h);
}
static void contiguous(const char* name, int nq) {
    int k;
    for (k = 0; k + 1 < nq; k++) {
        float a0, a1, b0, b1;
        ui_2d_group_ry(buf, nv, k, &a0, &a1);
        ui_2d_group_ry(buf, nv, k + 1, &b0, &b1);
        CHECK(fabsf(a1 - b0) <= 1e-4f, "%s: seg %d top %.4f vs next bottom %.4f", name, k, a1, b0);
    }
}
static void t9(void) {
    uint8_t orig[0x18 * 64];
    int sv;
    reset_chain();
    /* shape 272: 9 quads, x +-0.063, 0.2226 tall, pitch 0.2226, +-1.0017 */
    rope(9, 0.2226f, 0.063f, 0.2226f);
    memcpy(orig, buf, (size_t)nv * 0x18);
    set_vpq(nv, nv / 4 * 6);
    CHECK(count_clusters() == 1, "shape 272: %d clusters, want 1", count_clusters());
    sv = flush(0, 0);
    CHECK(sv == 0, "shape 272: %d verts scaled, want 0 (fill exemption)", sv);
    CHECK(!memcmp(orig, buf, (size_t)nv * 0x18), "shape 272 output differs from input");
    contiguous("shape 272", 9);
    /* shape 263: pitch 0.195, x +-0.055, span +-0.877 */
    reset_chain();
    rope(9, 0.195f, 0.055f, 0.195f);
    set_vpq(nv, nv / 4 * 6);
    CHECK(count_clusters() == 1, "shape 263: %d clusters, want 1", count_clusters());
    {
        float m; ui_2d_cluster(buf, nv, 0, &m);
        CHECK(near(m, 0.0f, 1e-4f), "shape 263 anchor %.4f want 0", m);
    }
    /* mid-zoom the rope belongs to the unscaled scene: kept whole (pin10b
     * scaled it about 0, and it popped to full height when the zoom ended) */
    memcpy(orig, buf, (size_t)nv * 0x18);
    sv = flush(0, 0);
    CHECK(sv == 0, "shape 263: %d verts scaled, want 0 (scene rope)", sv);
    CHECK(!memcmp(orig, buf, (size_t)nv * 0x18), "shape 263 output differs from input");
    contiguous("shape 263", 9);
    /* same rope with ctr=1 is the same rope */
    g_ui_2d_center = 1; reset_chain();
    rope(9, 0.195f, 0.055f, 0.195f);
    memcpy(orig, buf, (size_t)nv * 0x18);
    flush(0, 0);
    CHECK(!memcmp(orig, buf, (size_t)nv * 0x18), "shape 263 ctr1 output differs from input");
    g_ui_2d_center = 0;
}

/* ---- T12 keep rules + fog (run 12) ---- */
static int kept(void) {
    uint8_t orig[0x18 * 64];
    memcpy(orig, buf, (size_t)nv * 0x18);
    flush(0, 0);
    return !memcmp(orig, buf, (size_t)nv * 0x18);
}
static void t12(void) {
    static const float zoom[] = {0.620f, 0.723f, 0.775f, 0.880f, 0.950f};
    float m0, m1;
    int i;
    reset_chain();
    /* main menu tunnel: x +-0.533, y +-0.948, inside the +-1 cave fill */
    nv = 0; quad(0, -0.533f, 0.533f, -0.948f, 0.948f);
    { int kk = kept(); CHECK(kk, "tunnel +-0.948 scaled, want kept"); }
    /* camp / menu zoom: frame-shaped squares at every size, ctr on or off */
    for (i = 0; i < 5; i++) {
        nv = 0; quad(0, -zoom[i], zoom[i], -zoom[i], zoom[i]);
        { int kk = kept(); CHECK(kk, "zoom square +-%.3f scaled, want kept", zoom[i]); }
        g_ui_2d_center = 1;
        nv = 0; quad(0, -zoom[i], zoom[i], -zoom[i], zoom[i]);
        { int kk = kept(); CHECK(kk, "zoom square +-%.3f ctr1 scaled, want kept", zoom[i]); }
        g_ui_2d_center = 0;
    }
    /* off-centre zoom (zooming into options): still frame-shaped */
    nv = 0; quad(0, -0.2f, 0.6f, -0.5f, 0.3f);
    { int kk = kept(); CHECK(kk, "off-centre zoom square scaled, want kept"); }
    /* mid-zoom rope at +-0.662 (shape 243) */
    nv = 0; rope(9, 0.147f, 0.041f, 0.147f);
    { int kk = kept(); CHECK(kk, "rope +-0.662 scaled, want kept"); }
    /* things that must still scale: pause board, main menu arch (1.7),
     * banner frame, 1.76 backdrop, a 3-quad narrow column, small squares */
    nv = 0; quad(0, -0.267f, 0.267f, -0.474f, 0.474f);
    { int kk = kept(); CHECK(!kk, "pause board +-0.474 kept, want scaled"); }
    nv = 0; quad(0, -0.478f, 0.0f, -0.85f, 0.85f); quad(0, 0.0f, 0.478f, -0.85f, 0.85f);
    { int kk = kept(); CHECK(!kk, "arch +-0.85 kept, want scaled"); }
    nv = 0; quad(0, -0.733f, 0.733f, 0.55f, 1.074f);
    { int kk = kept(); CHECK(!kk, "banner frame kept, want scaled"); }
    nv = 0; quad(0, -0.971f, 0.971f, -0.88f, 0.88f);
    { int kk = kept(); CHECK(!kk, "+-0.971 x +-0.88 kept, want scaled"); }
    nv = 0; rope(3, 0.2f, 0.041f, 0.2f);
    { int kk = kept(); CHECK(!kk, "3-tile column kept, want scaled"); }
    nv = 0; quad(0, -0.2f, 0.2f, -0.2f, 0.2f);
    { int kk = kept(); CHECK(!kk, "0.4 square kept, want scaled (below UI_2D_FRAME_MIN)"); }
    /* fog: wider than the screen, x centre drifting 0.013 <-> 0.021 (run 12).
     * with ctr=1 both frames must use the same anchor (own mean, ~-1) */
    g_ui_2d_center = 1;
    for (i = 0; i < 2; i++) {
        /* the logged fog quad centres (pin8 shape 192) */
        static const float fc[12] = {-1.000f, -1.019f, -0.985f, -1.005f, -0.977f, -1.007f,
                                     -1.006f, -0.992f, -1.025f, -1.023f, -1.028f, -0.987f};
        float dx = i ? 0.021f : 0.013f, a;
        int k;
        reset_chain(); nv = 0;
        for (k = 0; k < 16; k++) {
            float x0 = -1.367f + dx - 0.013f + 0.17f * k;
            quadc(0, x0, x0 + 0.2f, fc[k % 12], 0.9f);
        }
        {
            float yin = ui_2d_ldf(buf + 4);
            flush(0, 0);
            a = anchor_of(yin, ui_2d_ldf(buf + 4));
        }
        if (i == 0) m0 = a; else m1 = a;
    }
    g_ui_2d_center = 0;
    SAY("fog anchor at xc 0.013: %.4f, at 0.021: %.4f", m0, m1);
    CHECK(near(m0, m1, 0.002f), "fog anchor flips %.4f -> %.4f", m0, m1);
    CHECK(m0 < -0.9f, "fog anchor %.4f, want its own mean ~-1", m0);
}

/* ---- T10 strip guard ---- */
static void t10(void) {
    reset_chain();
    /* touching, centres 0.25 apart, x boxes differ by 0.01: still two elements */
    nv = 0; quadc(0, -0.05f, 0.05f, 0.0f, 0.25f); quadc(0, -0.04f, 0.06f, 0.25f, 0.25f);
    set_vpq(nv, 6);
    CHECK(count_clusters() == 2, "differing x box: %d clusters, want 2", count_clusters());
    /* same x box but a 0.02 gap: two elements */
    nv = 0; quadc(0, -0.05f, 0.05f, 0.0f, 0.23f); quadc(0, -0.05f, 0.05f, 0.25f, 0.23f);
    set_vpq(nv, 6);
    CHECK(count_clusters() == 2, "gapped strip: %d clusters, want 2", count_clusters());
    /* control: same x box, touching: one element */
    nv = 0; quadc(0, -0.05f, 0.05f, 0.0f, 0.25f); quadc(0, -0.05f, 0.05f, 0.25f, 0.25f);
    set_vpq(nv, 6);
    CHECK(count_clusters() == 1, "touching strip: %d clusters, want 1", count_clusters());
    /* pause text rows still six clusters, camp hud (3x3 glyph digits, T3 shapes) still one */
    nv = 0;
    { int r; float ex[6] = {0.36f, 0.22f, 0.08f, -0.06f, -0.20f, -0.34f};
      for (r = 0; r < 6; r++) row(1, ex[r], 9, 0.069f, 0, 0, -0.12f); }
    set_vpq(nv, nv);
    CHECK(count_clusters() == 6, "pause rows: %d clusters, want 6", count_clusters());
    nv = 0;
    { int i; for (i = 0; i < 5; i++) quad(1, 0.45f + 0.05f * i, 0.49f + 0.05f * i, 0.86f, 0.94f); }
    set_vpq(nv, nv);
    CHECK(count_clusters() == 1, "hud run: %d clusters, want 1", count_clusters());
}

/* ---- T11 centre toggle ---- */
static void snap_run(void (*bld)(void), int tri, int ctr, uint8_t* out, int* nout, int* pins) {
    g_ui_2d_center = ctr;
    reset_chain();
    bld();
    flush(tri, 0);
    *pins = last_pins;
    memcpy(out, buf, (size_t)nv * 0x18);
    *nout = nv;
    g_ui_2d_center = 0;
}
static void b_pause(void) { int r; float ex[6] = {0.36f, 0.22f, 0.08f, -0.06f, -0.20f, -0.34f};
    nv = 0; for (r = 0; r < 6; r++) row(1, ex[r], 9, 0.069f, 0, 0, -0.12f); }
static void b_confirm(void) { int r, i; /* shape 532: n 480, x +-0.388, y 0.107..0.390, one cluster */
    nv = 0; for (r = 0; r < 4; r++) for (i = 0; i < 20; i++)
        quadc(1, -0.388f + 0.0388f * i, -0.388f + 0.0388f * i + 0.036f, 0.137f + 0.074f * r, 0.060f); }
static void b_yes(void) { int i; nv = 0; for (i = 0; i < 3; i++) quadc(1, -0.041f + 0.0273f * i, -0.041f + 0.0273f * i + 0.0273f, -0.34f, 0.06f); }
static void b_no(void) { int i; nv = 0; for (i = 0; i < 2; i++) quadc(1, -0.031f + 0.031f * i, -0.031f + 0.031f * i + 0.031f, -0.373f, 0.065f); }
static void b_camp(void) { int i; nv = 0; for (i = 0; i < 8; i++) quadc(1, -0.51f + 0.035f * i, -0.51f + 0.035f * i + 0.03f, 0.2f, 0.07f); }
static void b_hud(void) { int i; nv = 0; for (i = 0; i < 5; i++) quad(1, 0.45f + 0.05f * i, 0.49f + 0.05f * i, 0.86f, 0.94f); }
static void b_fill(void) { nv = 0; quad(0, -1.0f, 1.0f, -1.0f, 1.0f); }

static void t11(void) {
    static uint8_t a0[0x18 * 4096], a1[0x18 * 4096], in[0x18 * 4096];
    int n0, n1, p0, p1, k, cnt;
    struct { const char* name; void (*b)(void); int tri; int map; } C[] = {
        {"pause", b_pause, 1, 1}, {"confirm", b_confirm, 1, 1}, {"Yes", b_yes, 1, 1}, {"No", b_no, 1, 1},
        {"camp off-centre", b_camp, 1, 0}, {"hud pinned", b_hud, 1, 0}, {"fill", b_fill, 0, 0},
    };
    for (cnt = 0; cnt < 7; cnt++) {
        float maxe = 0;
        C[cnt].b(); memcpy(in, buf, (size_t)nv * 0x18);
        snap_run(C[cnt].b, C[cnt].tri, 0, a0, &n0, &p0);
        snap_run(C[cnt].b, C[cnt].tri, 1, a1, &n1, &p1);
        if (cnt == 1) { int ng = n1 / 6, g = 0, c = 0; float m; set_vpq(n1, n1);
            memcpy(buf, in, (size_t)nv * 0x18); (void)ng;
            while (g < n1 / 6) { g = ui_2d_cluster(buf, n1, g, &m); c++; }
            CHECK(c == 1, "confirm block is %d clusters, want 1", c);
            nv = n1; }
        if (cnt == 5) CHECK(p0 == 1 && p1 == 1, "hud batch not pinned (%d/%d)", p0, p1);
        if (C[cnt].map) {
            for (k = 0; k < n1; k++) {
                float y0 = ui_2d_ldf(in + (size_t)k * 0x18 + 4), y1 = ui_2d_ldf(a1 + (size_t)k * 0x18 + 4);
                float e = fabsf(y1 - 0.75f * y0);
                if (e > maxe) maxe = e;
            }
            CHECK(maxe < 0.0005f, "%s ctr=1: y -> 0.75*y off by %.5f", C[cnt].name, maxe);
            CHECK(memcmp(a0, a1, (size_t)n0 * 0x18) != 0, "%s ctr=1 same as ctr=0", C[cnt].name);
        } else {
            CHECK(!memcmp(a0, a1, (size_t)n0 * 0x18), "%s: ctr=1 changes output", C[cnt].name);
        }
        SAY("%s: max |y1 - 0.75 y0| %.5f", C[cnt].name, maxe);
    }
}

typedef void (*fn)(void);
int main(void) {
    /* the tests were written against the old initializers; the release build
     * defaults centre on and the dump off, so pin them here. */
    g_ui_2d_center = 0;
    g_do_ui_2d_dump = 1;
    static const struct { const char* name; fn f; int bars_differs; } T[] = {
        {"T1 rope tear", t1, 0}, {"T2 pinned-top column", t2, 0}, {"T3 hud", t3, 0}, {"T4 hud digits", t4, 0},
        {"T5 bbox vs mean", t5, 0}, {"T6 shape 79 straddle (bars=0 documents bug, bars=1 fixes)", 0, 1}, {"T7 regressions", t7, 0},
        {"T9 rope strip", t9, 0}, {"T10 strip guard", t10, 0}, {"T11 centre toggle", t11, 0},
        {"T12 keep rules + fog", t12, 0},
    };
    int i, total = 0;
    static uint8_t recA[sizeof rec];
    int nA;

    g_ui_2d_fill = 1;
    for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
        int f0, ident, differs;
        printf("%s\n", T[i].name);
        /* the real checks, shipped default bars=0 */
        g_ui_2d_bars = 0; recn = 0; fails = 0; checking = 1;
        if (i == 5) { t6(); } else T[i].f();
        f0 = fails; nA = recn; memcpy(recA, rec, (size_t)recn);
        /* T6 additionally checks the bars=1 behaviour */
        if (i == 5) { g_ui_2d_bars = 1; recn = 0; t6(); f0 = fails; differs = recn != nA || memcmp(recA, rec, (size_t)recn) != 0; CHECK(differs, "bars=1 output identical to bars=0"); f0 = fails; }
        g_ui_2d_bars = 0;
        printf("  %s\n", f0 ? "FAIL" : "PASS");
        { uint32_t h = 2166136261u; int q; for (q = 0; q < nA; q++) h = (h ^ recA[q]) * 16777619u; printf("  rec hash %08x (%d bytes)\n", h, nA); }
        total += f0;
        /* T8: same input again with bars forced off must be byte-identical; for everything except T6 so must bars=1 */
        checking = 0;
        g_ui_2d_bars = 0; recn = 0; if (i == 5) t6(); else T[i].f();
        ident = recn == nA && !memcmp(recA, rec, (size_t)nA);
        if (!ident) { total++; printf("  T8 FAIL: bars=0 rerun of %s not byte-identical\n", T[i].name); }
        if (i != 5) {
            g_ui_2d_bars = 1; recn = 0; T[i].f(); g_ui_2d_bars = 0;
            ident = recn == nA && !memcmp(recA, rec, (size_t)nA);
            if (!ident) { total++; printf("  T8 FAIL: bars=1 changes %s output\n", T[i].name); }
        }
    }
    printf("T8 bars off/on identical on all but T6 (bars=0 rerun identical on all): %s\n", total ? "FAIL (see above)" : "PASS");
    printf("%s\n", total ? "SOME TESTS FAILED" : "ALL PASS");
    return total ? 1 : 0;
}
