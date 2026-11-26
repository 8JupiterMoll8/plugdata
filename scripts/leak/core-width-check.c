/* core-width-check.c: headless libpd check for the comment-width heap corruption (2026-11-27).
 *
 * A patch line "#X text 10 10 hi, f -3;" reaches canvas_f (g_canvas.c) and stored a negative te_width (a signed
 * short, so "f 40000" wrapped negative too). rtext_formattext (g_rtext.c) then computed a negative maxindex_b and
 * strncpy'd a negative (huge) length: "malloc(): corrupted top size" about 11 s after open.
 *
 * Checks, in order (each failure is counted, the render step comes last because the unfixed core crashes there):
 *   1. a loaded patch never leaves a text object with te_width < 0 ("f -3" -> 0, "f 40000" -> 32767, "f 20" -> 20)
 *   2. rtext_charwidth never reports < 0, even when te_width is forced negative behind canvas_f's back
 *   3. making the canvas visible (rtext_senditup on every object, negative width forced) neither crashes nor corrupts
 *      the heap
 *
 * Build/run: core-width-check.sh [libpd-src.a] [pure-data/src dir]   (exit 0 = all checks pass; argv[1] = scratch dir)
 */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "m_pd.h"
#include "g_canvas.h"
#include "z_libpd.h"

extern int rtext_charwidth(t_rtext *x);

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* the plugdata fork routes sys_vgui here; headless there is no GUI to talk to */
void plugdata_gui_message(const char *fmt, va_list args) { (void)fmt; (void)args; }

static const char *PATCH =
    "#N canvas 0 50 450 300 12;\n"
    "#X text 10 10 hi, f -3;\n"
    "#X text 10 40 wide comment goes on and on, f 40000;\n"
    "#X text 10 70 plain, f 20;\n"
    "#X text 10 100 auto;\n";

static const int EXPECT_WIDTH[] = { 0, SHRT_MAX, 20, 0 };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[PATH_MAX];
    FILE *f;
    t_canvas *cv;
    t_gobj *g;
    int n = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    snprintf(path, sizeof path, "%s/cw.pd", dir);
    if (!(f = fopen(path, "w"))) { perror("fopen"); return 2; }
    fputs(PATCH, f);
    fclose(f);

    libpd_init();
    libpd_init_audio(0, 2, 44100);
    cv = (t_canvas *)libpd_openfile("cw.pd", dir);
    if (!cv) { printf("FAIL: libpd_openfile\n"); return 2; }

    for (g = cv->gl_list; g; g = g->g_next, n++)
    {
        t_text *t = (t_text *)g;
        CHECK(n < 4, "object %d is within the 4 comments of the test patch", n);
        if (n < 4)
            CHECK(t->te_width >= 0 && t->te_width == EXPECT_WIDTH[n],
                "comment %d loaded te_width %d (want %d, never negative)", n, (int)t->te_width, EXPECT_WIDTH[n]);
    }
    CHECK(n == 4, "all 4 comments loaded (%d)", n);

    for (g = cv->gl_list; g; g = g->g_next)
    {
        t_text *t = (t_text *)g;
        t_rtext *rt = glist_getrtext(cv, t, 1);
        short keep = t->te_width;
        t->te_width = -3;
        CHECK(rt && rtext_charwidth(rt) >= 0, "rtext_charwidth with te_width forced to -3 is %d (want >= 0)",
            rt ? rtext_charwidth(rt) : -999);
        t->te_width = keep;
    }

    /* 3: render every comment with a negative width forced. On the unfixed core this smashes the heap. */
    for (g = cv->gl_list; g; g = g->g_next)
        ((t_text *)g)->te_width = -3;
    canvas_vis(cv, 1);
    for (g = cv->gl_list; g; g = g->g_next)     /* the path of the abort: text_getrect -> rtext_getrect -> rtext_senditup */
    {
        int x1, y1, x2, y2;
        gobj_getrect(g, cv, &x1, &y1, &x2, &y2);
    }
    for (n = 0; n < 200; n++) { void *p = malloc(64 + n); if (p) { memset(p, 0xA5, 64 + n); free(p); } }
    CHECK(glist_isvisible(cv), "canvas became visible and every comment was formatted with te_width -3 (no crash)");
    canvas_vis(cv, 0);
    for (g = cv->gl_list; g; g = g->g_next)
        ((t_text *)g)->te_width = 0;
    libpd_closefile(cv);

    printf("%s (%d failure%s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
