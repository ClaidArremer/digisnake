/* SPDX-License-Identifier: GPL-2.0-or-later */
/* digisnake: a Cartesian step sequencer for the Digitone mk1 (OS 1.43 and
 * 1.44), after MDD Snake (itself after Make Noise's Rene).
 *
 * Eight steps sit in a 4 x 2 grid that matches the encoders: A-D are the top
 * row, E-H the bottom. A path (the "snake") walks the grid in one of several
 * shapes, and each sequencer step of the track moves it on (or every DIV
 * steps). Where it lands decides the step:
 *
 *   page 1  NOTES   each cell's note, -24..+24 semitones from the track's
 *                   NOTE, quantised to SCALE in the keyboard's ROOT
 *   page 2  GATES   each cell fires or rests
 *   page 3  VELOC   each cell's velocity, 1-127
 *   page 4  PATH    ON, SHAPE, DIR, LEN, DIV, SCALE, RANGE, SHIFT
 *                   (DIV: 1/8, 1/4, 1/2, 1X, 2X; 2X runs the track itself at
 *                   twice its speed, in per-track SCALE mode)
 *
 * With ON set, the track plays the snake in place of its own trigs (lock
 * trigs still play). Everything is per track (T1-T4) and saved with the
 * project.
 *
 * The page is TRIG's next page: press TRIG again from the TRIG page. It is
 * not in core-dn1's Mod Menu (hold a track key), which stays free for
 * machines and other mods. On the page:
 *   knobs A-H        the cells of the page shown (PATH: its eight settings)
 *   LEFT / RIGHT     the previous / next page
 *   UP / DOWN        NOTES: all notes up/down a semitone (FUNC: an octave);
 *                    GATES: rotate; VELOC: all velocities up/down
 *   trig keys 1-8    GATES page: toggle that cell
 *   hold TRIG + YES  randomise the page shown (PATH: all but SNAKE and SCALE)
 *   hold TRIG + NO   back to how it was when TRIG went down (a print too)
 *   hold TRIG + PLAY clear the page shown: NOTES 0, GATES on, VELOC 100,
 *                    PATH's defaults (SNAKE as it was)
 *   hold TRIG + REC / STOP  copy the page shown / paste it (another track)
 *   hold TRIG + FUNC + REC  PRINT: the snake into the track's own trigs,
 *                    notes and velocities, then SNAKE off
 *   FUNC + YES       randomise NOTES, GATES and VELOC together
 *   FUNC + NO        back to before the last run of FUNC + YES
 *   FUNC + knob      jump: NOTES -24/-12/0/+12/+24, VELOC 1/32/64/96/127,
 *                    PATH's RANGE 1/12/24
 *   TRIG on its own  on to TRIG's next page, as stock
 *
 * On OS 1.44 (its port's cflags) the stock drop-down message names what a
 * randomise, reset, undo or jump did, and every change asks the OS to refresh
 * its power-up copy of the project, so the snake survives a power cycle.
 *
 * The sequencer side uses seq-events (step-fire events): ev_gen_trig decides
 * whether a step fires, ev_gen_notes its note and ev_fire_vel its velocity.
 * The UI uses core-dn1's mod page and project data. No firmware
 * address is named here, so one source builds for 1.43 and 1.44.
 */
#include "digitone-mk1/core3.h"
#include "digitone-mk1/seq.h"

/* OS 1.44 only (mod.json ports.1.44.cflags): the stock drop-down message --
 * the message manager singleton (SNK_POPUP_GET) and its printf (SNK_POPUP_PRINTF),
 * as the OS shows "Paste Pattern". Both return pointers in %d0: declared as
 * returning integers, so any toolchain reads %d0. */
#if defined(SNK_POPUP_GET) && defined(SNK_POPUP_PRINTF)
#define POPUP(...) do { uint32_t m_ = ((uint32_t (*)(void))SNK_POPUP_GET)(); \
                        if (m_) ((void (*)(uint32_t, const char *, ...))SNK_POPUP_PRINTF)(m_, __VA_ARGS__); \
                   } while (0)
#else
#define POPUP(...) do { } while (0)
#endif

#define TRACKS    8             /* 0-3 the synth tracks, 4-7 the MIDI tracks */
#define CELLS     8
#define NOTE_MAX  24

/* ---- the project's data (128 bytes) ---------------------------------- */
enum { P_ON, P_SHAPE, P_DIR, P_LEN, P_DIV, P_SCALE, P_RANGE, P_SHIFT, NSET };

struct snake {
    signed char note[CELLS];    /* semitones from NOTE */
    unsigned char vel[CELLS];   /* 1-127 */
    unsigned char set[NSET];    /* the PATH page */
    unsigned char gate;         /* bit c: cell c fires */
    unsigned char pad[7];
};                              /* 32 bytes */

struct snake digisnake_data[TRACKS];

/* ---- shapes: the order the path visits the cells ----------------------
 * Cells:  0 1 2 3   (knobs A B C D)
 *         4 5 6 7   (knobs E F G H) */
#define SHAPES 7
static const unsigned char shapes[SHAPES][CELLS] = {
    {0, 1, 2, 3, 4, 5, 6, 7},   /* ROWS   left to right, top then bottom */
    {0, 1, 2, 3, 7, 6, 5, 4},   /* SNAKE  along the top, back along the bottom */
    {0, 4, 1, 5, 2, 6, 3, 7},   /* COLS   down each column */
    {0, 4, 5, 1, 2, 6, 7, 3},   /* ZIGZAG down, across, up, across */
    {0, 5, 2, 7, 4, 1, 6, 3},   /* DIAG   diagonal hops */
    {0, 3, 7, 4, 1, 2, 6, 5},   /* CORNER the corners, then the middle */
    {0, 6, 1, 7, 2, 4, 3, 5},   /* HOP    two across and down */
};
static const char *const shape_names[SHAPES] = {
    "ROWS", "SNAKE", "COLS", "ZIGZAG", "DIAG", "CORNER", "HOP"
};

enum { D_FWD, D_BWD, D_FB, D_BF, D_RND, DIRS };
static const char *const dir_names[DIRS] = { "FWD", "BWD", "F/B", "B/F", "RND" };

/* Scales: a bit per semitone above the root. */
#define SCALES 12
static const unsigned short scale_mask[SCALES] = {
    0xfff,  /* CHR  */
    0xab5,  /* MAJ  */
    0x5ad,  /* MIN  */
    0x6ad,  /* DOR  */
    0x5ab,  /* PHR  */
    0xad5,  /* LYD  */
    0x6b5,  /* MIX  */
    0x9ad,  /* HMIN */
    0x295,  /* PENT */
    0x4a9,  /* mPNT */
    0x4e9,  /* BLUE */
    0x555,  /* WHOL */
};
static const char *const scale_names[SCALES] = {
    "CHR", "MAJ", "MIN", "DOR", "PHR", "LYD", "MIX", "HMIN", "PENT", "mPNT", "BLUE", "WHOL"
};

static const unsigned char set_min[NSET] = { 0, 0, 0, 1, 0, 0, 1, 0 };   /* RANGE from 1 (1.2) */
static const unsigned char set_max[NSET] = { 1, SHAPES - 1, DIRS - 1, CELLS, 8, SCALES - 1, NOTE_MAX, CELLS - 1 };
static const char *const set_labels[NSET] = { "SNAKE", "SHAPE", "DIR", "LEN", "DIV", "SCALE", "RANGE", "SHIFT" };

/* DIV: sequencer steps a snake step (1 = 1X, 2 = 1/2, 4 = 1/4, 8 = 1/8), or 0:
 * 2X, the track itself at twice its speed (per-track SCALE mode) and the snake
 * a step every step. The knob walks div_list; values from before 0.9 (3, 5,
 * 6, 7) still play as they are and snap to the list when turned. */
#define DIVS 5
static const unsigned char div_list[DIVS] = { 8, 4, 2, 1, 0 };
static const char *div_name(int v)
{
    switch (v) {
    case 0: return "2X";
    case 1: return "1X";
    case 2: return "1/2";
    case 4: return "1/4";
    case 8: return "1/8";
    default: return "1/?";
    }
}
static int div_turn(int v, long d)
{
    int i, best = 3;
    for (i = 0; i < DIVS; i++)                  /* the nearest on the list */
        if (div_list[i] == v)
            best = i;
    if (best == 3 && v != 1) {
        for (i = 0; i < DIVS - 1; i++)
            if (div_list[i] >= v)
                best = i;
    }
    best += (int)d;
    if (best < 0) best = 0;
    if (best > DIVS - 1) best = DIVS - 1;
    return div_list[best];
}

static void track_defaults(struct snake *s)
{
    static const signed char demo[CELLS] = { 0, 3, 7, 10, 12, 10, 7, 3 };
    int c;
    for (c = 0; c < CELLS; c++) {
        s->note[c] = demo[c];
        s->vel[c] = 100;
    }
    s->gate = 0xff;
    s->set[P_ON] = 0;
    s->set[P_SHAPE] = 1;        /* SNAKE */
    s->set[P_DIR] = D_FWD;
    s->set[P_LEN] = CELLS;
    s->set[P_DIV] = 1;
    s->set[P_SCALE] = 0;
    s->set[P_RANGE] = 12;
    s->set[P_SHIFT] = 0;
    for (c = 0; c < 7; c++)
        s->pad[c] = 0;
}

static void check_track(struct snake *s)
{
    int c;
    for (c = 0; c < CELLS; c++) {
        if (s->note[c] > NOTE_MAX)
            s->note[c] = NOTE_MAX;
        else if (s->note[c] < -NOTE_MAX)
            s->note[c] = -NOTE_MAX;
        if (s->vel[c] < 1)
            s->vel[c] = 1;
        else if (s->vel[c] > 127)
            s->vel[c] = 127;
    }
    for (c = 0; c < NSET; c++)
        if (s->set[c] < set_min[c] || s->set[c] > set_max[c])
            s->set[c] = c == P_DIV ? 1                   /* DIV: 1X, never 2X */
                      : c == P_RANGE ? 12                /* RANGE 0 (before 1.2): the default */
                      : set_min[c];
    if (s->set[P_LEN] < 1)
        s->set[P_LEN] = CELLS;
}

static unsigned char data_ready;
static volatile unsigned char reset_req[TRACKS];
/* Starting from STOP (1.2). The sequencer prepares step 1 while stopped (a fire
 * with the transport at 0): that fire starts the path (prep), and the first fire
 * after PLAY is then step 2, one on. Without a prepare (rewind, set while
 * stopped), the first fire after PLAY starts the path. Before 1.2 the start was
 * also asked for while stopped and taken by step 2, so step 1 and step 2 both
 * played the path's first cell: the snake ran a step late. */
static volatile unsigned char prep[TRACKS], rewind[TRACKS];

/* DIRECT JUMP (1.4 or later), when the build has it: its count of pattern starts
 * (START ON BAR: a pattern launched from its first step on a bar line). When it
 * moves, every snake starts its path again with the pattern, so the whole groove
 * relaunches together. A weak import (mod.json): without DIRECT JUMP it reads 0
 * and never moves. */
extern volatile uint32_t dj_starts;
static uint32_t dj_seen[TRACKS];

/* ---- surviving a power cycle (OS 1.44) ------------------------------------
 * Core saves digisnake_data inside the project each time the OS serializes it.
 * What a power cycle keeps is the OS's power-up copy (its "mirror"), which it
 * refreshes with the job Project::updateMirror when the project changes -- a
 * snake edit is not a change the OS sees. So after an edit (and 1.5 s of
 * quiet) we make the same request the project's own change notice makes:
 * fw_project's virtual slot 17 with its mirror buffer (+32), unless the
 * project has its notices suspended (+36). The OS runs the job in the
 * background. SNK_MIRROR is set by the 1.44 port only. */
#define SAVE_TICKS 90                   /* UI frames of quiet before the request */
static unsigned short save_wait;        /* frames left; 0 = nothing pending */

static void changed(void)
{
    save_wait = SAVE_TICKS;
}

static void mirror_request(void)
{
#ifdef SNK_MIRROR
    uint8_t *p = fw_project;
    uint32_t buf;
    if (!p)
        return;
    buf = *(volatile uint32_t *)(p + 32);
    if (!buf || *(volatile uint8_t *)(p + 36))
        return;
    ((void (*)(void *, uint32_t, uint32_t))(*(uint32_t **)p)[17])(p, buf, 0);
#endif
}

/* ---- reload, the Elektron way -----------------------------------------------
 * TRIG + NO puts the track's snake back as it was when TRIG went down, however
 * many randomises and clears came since; pressing it again changes nothing.
 * FUNC + NO puts it back as it was before the first FUNC + YES of a run of
 * them (any other edit ends the run). */
static struct snake hold_copy[TRACKS], run_copy[TRACKS];
static unsigned char hold_valid[TRACKS], run_valid[TRACKS], run_active[TRACKS];

static void loaded(int t0, int found)
{
    int t;
    for (t = t0; t < t0 + 4; t++) {
        if (found)
            check_track(&digisnake_data[t]);
        else
            track_defaults(&digisnake_data[t]);
        reset_req[t] = 1;
        hold_valid[t] = run_valid[t] = run_active[t] = 0;
    }
    data_ready = 1;
}
void digisnake_loaded(int found) { loaded(0, found); }      /* T1-T4 */
void digisnake_loaded2(int found) { loaded(4, found); }     /* the MIDI tracks */

/* Two blocks, so projects saved with 0.1-0.3 (SNK1, the synth tracks) load
 * as they were; SNK2 holds the MIDI tracks. */
const struct core_projdata digisnake_projdata = {
    0x534e4b31u,                /* "SNK1" */
    4 * sizeof(struct snake), &digisnake_data[0], digisnake_loaded
};
const struct core_projdata digisnake_projdata2 = {
    0x534e4b32u,                /* "SNK2" */
    4 * sizeof(struct snake), &digisnake_data[4], digisnake_loaded2
};

/* ---- playback (interrupt level, in the sequencer's step fire) ---------- */
struct play {
    unsigned char k;            /* the path step, 0..len-1 */
    unsigned char count;        /* sequencer steps since the last move */
    signed char pp;             /* ping-pong's way */
    unsigned char cell;         /* the cell the step plays */
    unsigned char moved;        /* this step is a snake step (DIV: the others rest) */
};
static struct play play[TRACKS];
volatile unsigned char digisnake_cell[TRACKS];      /* for the screen */
static unsigned long irq_rand = 0x2545f491UL;

static unsigned long xorshift(unsigned long *s)
{
    unsigned long x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x2545f491UL;
    return *s;
}

static int track_on(int32_t track)
{
    return data_ready && (uint32_t)track < TRACKS && digisnake_data[track].set[P_ON];
}

/* The next path step from k: the direction's rule over len steps. */
static void step_on(struct play *p, const struct snake *s, int len, unsigned long *rnd)
{
    int k = p->k;
    switch (s->set[P_DIR]) {
    case D_BWD:
        k = k > 0 ? k - 1 : len - 1;
        break;
    case D_FB:
    case D_BF:
        if (len < 2) {
            k = 0;
            break;
        }
        if (p->pp >= 0) {
            if (k + 1 >= len) { p->pp = -1; k = len - 2; } else k++;
        } else {
            if (k <= 0) { p->pp = 1; k = 1; } else k--;
        }
        break;
    case D_RND:
        k = (int)(xorshift(rnd) % (unsigned long)len);
        break;
    default:
        k = k + 1 < len ? k + 1 : 0;
        break;
    }
    if (k >= len)
        k = 0;
    p->k = (unsigned char)k;
}

static void start(struct play *p, const struct snake *s, int len)
{
    p->count = 0;
    p->moved = 1;
    if (s->set[P_DIR] == D_BWD || s->set[P_DIR] == D_BF) {
        p->k = (unsigned char)(len - 1);
        p->pp = -1;
    } else {
        p->k = 0;
        p->pp = 1;
    }
}

static int cell_of(const struct snake *s, int k, int len)
{
    int i = (k + s->set[P_SHIFT]) % len;
    return shapes[s->set[P_SHAPE] % SHAPES][i];
}

/* One sequencer step on: a snake step every DIV steps, a rest between. */
static void advance(struct play *p, const struct snake *s, int len)
{
    if (++p->count >= s->set[P_DIV]) {
        p->count = 0;
        p->moved = 1;
        step_on(p, s, len, &irq_rand);
    } else {
        p->moved = 0;                       /* DIV 1/2 .. 1/8: between snake steps */
    }
}

/* Does the step fire. Both reads of a step (first 1, then 0) must answer the
 * same: the first moves the path, the second reuses its cell. */
void digisnake_trig(int32_t track, int32_t step, int32_t first, uint32_t *trig)
{
    const struct snake *s;
    struct play *p;
    int len;
    (void)step;
    if (!track_on(track))
        return;
    s = &digisnake_data[track];
    p = &play[track];
    len = s->set[P_LEN];
    if (len < 1 || len > CELLS)
        len = CELLS;
    if (first) {
        if (fw_transport == 0) {            /* stopped: prepare the first step */
            prep[track] = 1;
            rewind[track] = reset_req[track] = 0;
            dj_seen[track] = dj_starts;
            start(p, s, len);
        } else if (prep[track] || rewind[track]) {
            /* the first fire after PLAY: step 1 is the path's start (step 2
             * when step 1 was prepared while stopped, or before SNAKE was on) */
            int n;
            prep[track] = rewind[track] = reset_req[track] = 0;
            dj_seen[track] = dj_starts;
            start(p, s, len);
            for (n = 0; n < step && n < SQ_STEPS; n++)
                advance(p, s, len);
        } else if (reset_req[track] || dj_starts != dj_seen[track]) {
            reset_req[track] = 0;
            dj_seen[track] = dj_starts;     /* a pattern start: from the path's start */
            start(p, s, len);
        } else {
            advance(p, s, len);
        }
        if (p->k >= len)
            p->k = 0;
        p->cell = (unsigned char)cell_of(s, p->k, len);
        digisnake_cell[track] = p->cell;
    }
    /* a snake step fires its cell's gate; between them (DIV 1/2 .. 1/8) the
     * track rests, so 1/2 plays eighths, 1/4 quarters, 1/8 halves */
    if (p->moved && ((s->gate >> p->cell) & 1))
        *trig |= SQ_TRIG;
    else
        *trig &= ~SQ_TRIG;
}

static int quantise(int note, int root, int scale)
{
    unsigned short m;
    int i, deg;
    if (scale <= 0 || scale >= SCALES)
        return note;
    m = scale_mask[scale];
    for (i = 0; i < 12; i++) {              /* down to the nearest scale tone */
        deg = (note - root - i) % 12;
        if (deg < 0)
            deg += 12;
        if ((m >> deg) & 1)
            return note - i;
    }
    return note;
}

void digisnake_notes(int32_t track, int32_t step, int32_t generated, int8_t notes[SQ_NOTES])
{
    const struct snake *s;
    struct sq_track info;
    int base, root = 0, n;
    (void)step;
    (void)generated;
    if (!track_on(track))
        return;
    s = &digisnake_data[track];
    if (sq_track_info(track, &info, sizeof info) == 0) {
        base = info.note;
        if (info.root != SQ_ROOT_NONE)
            root = info.root % 12;
    } else {
        base = notes[0] >= 0 ? notes[0] : 60;
    }
    n = quantise(base + s->note[play[track].cell], root, s->set[P_SCALE]);
    if (n < 0)
        n = 0;
    else if (n > 127)
        n = 127;
    notes[0] = (int8_t)n;
}

void digisnake_vel(int32_t track, int32_t step, int32_t *vel)
{
    (void)step;
    if (!track_on(track))
        return;
    *vel = digisnake_data[track].vel[play[track].cell];
}

/* ---- 2X: the track's own speed --------------------------------------------
 * The pattern (fw_pattern, 976 bytes a track) keeps a speed per track at
 * +0x3c7, used when its SCALE mode is per track (pattern +0x16111 == 1): an
 * index into the stock speeds, ticks a step 3, 4, 6, 8, 12, 24, 48 (2X, 3/2X,
 * 1X, 3/4X, 1/2X, 1/4X, 1/8X). DIV 2X halves the track's step: 1X -> 2X,
 * 1/2X -> 1X and so on; switching 2X or SNAKE off, or the pattern changing,
 * puts it back. pad[0] keeps the speed it had + 1, so a power cycle with 2X on
 * still knows what to put back. */
#define PAT_MODE   0x16111
#define TR_SPEED   0x3c7
static uint8_t *mod_pat[TRACKS];            /* the pattern we sped up, 0 none */
static signed char mod_orig[TRACKS];        /* its track's speed before */

static int per_track_mode(void)
{
    uint8_t *pat = fw_pattern;
    return pat && pat[PAT_MODE] == 1;
}

static int speed_half(int i)                /* the index of half the ticks */
{
    static const signed char h[7] = { 0, 1, 0, 1, 2, 4, 5 };
    return i >= 0 && i < 7 ? h[i] : i;
}

static void speed_sync(void)
{
    uint8_t *pat = fw_pattern;
    int t, pt = pat && pat[PAT_MODE] == 1;
    for (t = 0; t < TRACKS; t++) {
        struct snake *s = &digisnake_data[t];
        int want = pt && s->set[P_ON] && s->set[P_DIV] == 0;
        if (mod_pat[t] && (mod_pat[t] != pat || !want)) {       /* put it back */
            uint8_t *tr = FW_TRACK(mod_pat[t], t);
            if ((signed char)tr[TR_SPEED] == speed_half(mod_orig[t]))
                tr[TR_SPEED] = (uint8_t)mod_orig[t];
            mod_pat[t] = 0;
            s->pad[0] = 0;
            changed();
        }
        if (want && !mod_pat[t]) {
            uint8_t *tr = FW_TRACK(pat, t);
            int o = (signed char)tr[TR_SPEED], h;
            if (s->pad[0] && o == speed_half(s->pad[0] - 1)) {  /* ours, from before */
                mod_pat[t] = pat;
                mod_orig[t] = (signed char)(s->pad[0] - 1);
            } else if ((h = speed_half(o)) != o) {
                mod_orig[t] = (signed char)o;
                tr[TR_SPEED] = (uint8_t)h;
                mod_pat[t] = pat;
                s->pad[0] = (unsigned char)(o + 1);
                changed();
            }
        }
    }
}

/* ---- PRINT (TRIG + FUNC + REC): the snake into the track's steps ------------
 * Plays the snake through the track's length, as it would from the start, and
 * writes each step as a stock trig: the trig bit (lock trigs kept), its note
 * (the step's first note; no chord) and its velocity (the step's own VEL),
 * then turns SNAKE off. A 2X speed stays. TRIG + NO, TRIG still held, puts the
 * track's steps and the snake back. */
static uint8_t print_bak[976];
static signed char print_t = -1;            /* the track print_bak holds, -1 none */

static int print_track(int t)
{
    uint8_t *pat = fw_pattern, *trk;
    struct snake *s = &digisnake_data[t];
    struct sq_track info;
    struct play q;
    unsigned long rnd = 0x9e3779b9UL ^ fw_timeline;
    int len, plen, base, root = 0, step, i, first, playing, pending;
    uint32_t last;
    if (!pat || sq_track_info(t, &info, sizeof info) != 0)
        return -1;
    len = info.len;
    if (len < 1 || len > 64)
        return -1;
    base = info.note;
    if (info.root != SQ_ROOT_NONE)
        root = info.root % 12;
    trk = FW_TRACK(pat, t);
    for (i = 0; i < 976; i++)
        print_bak[i] = trk[i];
    print_t = (signed char)t;
    plen = s->set[P_LEN];
    if (plen < 1 || plen > CELLS)
        plen = CELLS;
    /* Playing: carry on from where the snake is, so step N gets what step N
     * plays next time round (the path seldom sits at its start on step 1).
     * Stopped: from the path's start on step 1, as PLAY starts it. */
    last = sq_clock[t].last;
    playing = fw_transport == 1 && s->set[P_ON] && last < (uint32_t)len;
    pending = !playing || reset_req[t] || prep[t] || rewind[t] || dj_starts != dj_seen[t];
    first = playing ? ((int)last + 1) % len : 0;
    q = play[t];
    for (i = 0; i < len; i++) {
        int cell, n, moved = 1;
        uint16_t w;
        step = (first + i) % len;
        if (i == 0 && pending) {
            start(&q, s, plen);             /* the next step starts the path */
        } else if (++q.count >= s->set[P_DIV]) {
            q.count = 0;
            step_on(&q, s, plen, &rnd);
        } else {
            moved = 0;                      /* DIV: a rest between snake steps */
        }
        if (q.k >= plen)
            q.k = 0;
        cell = cell_of(s, q.k, plen);
        w = FW_STEP(trk, step);
        if (moved && ((s->gate >> cell) & 1))
            w |= FW_STEP_TRIG;
        else
            w &= (uint16_t)~FW_STEP_TRIG;
        FW_STEP(trk, step) = w;
        n = quantise(base + s->note[cell], root, s->set[P_SCALE]);
        if (n < 0) n = 0; else if (n > 127) n = 127;
        trk[0x1c0 + 8 * step] = (uint8_t)n;
        for (n = 1; n < 8; n++)             /* no chord */
            trk[0x1c0 + 8 * step + n] = 0;
        trk[0x80 + step] = s->vel[cell];
    }
    if (mod_pat[t]) {                       /* keep the 2X the print was made at */
        mod_pat[t] = 0;
        s->pad[0] = 0;
    }
    s->set[P_ON] = 0;
    sq_rearm(1u << t);
    return len;
}

static void unprint(int t)
{
    uint8_t *pat = fw_pattern, *trk;
    int i;
    if (print_t != t || !pat)
        return;
    trk = FW_TRACK(pat, t);
    for (i = 0; i < 976; i++)
        trk[i] = print_bak[i];
    print_t = -1;
    sq_rearm(1u << t);
}

/* ---- the page (core-dn1's mod pages): TRIG's next ------------------------
 * Its knobs are empty boxes: digisnake draws the page over the stock one and
 * takes the knobs and keys while it shows. TRIG_PAGE is the index of the
 * stock TRIG page (Trig Parameters) the page follows. */
#ifndef TRIG_PAGE
#error "build with -DTRIG_PAGE=<the TRIG page's index> (mod.json cflags)"
#endif
#ifndef TRIG_KIND
#define TRIG_KIND 9
#endif

struct core_page digisnake_page = {
    28, TRIG_PAGE, "SNK", "Snake",
    { 0, 0, 0, 0, 0, 0, 0, 0 }, TRIG_KIND,
    0, 0
};

/* The MIDI tracks' TRIG key has its own view, whose first page is the MIDI
 * Trig Parameters (page 19, kind 0): a second page follows that one. */
#ifndef MIDI_TRIG_PAGE
#define MIDI_TRIG_PAGE 19
#endif
#ifndef MIDI_TRIG_KIND
#define MIDI_TRIG_KIND 0
#endif
struct core_page digisnake_page_midi = {
    29, MIDI_TRIG_PAGE, "SNK", "Snake",
    { 0, 0, 0, 0, 0, 0, 0, 0 }, MIDI_TRIG_KIND,
    0, 0
};

#define KEY_FUNC  1
#define KEY_REC   10                    /* FUNC + REC is COPY */
#define KEY_PLAY  11                    /* FUNC + PLAY is CLEAR */
#define KEY_STOP  12                    /* FUNC + STOP is PASTE */
#define KEY_YES   13
#define KEY_NO    14
#define KEY_UP    15
#define KEY_DOWN  16
#define KEY_LEFT  17
#define KEY_RIGHT 18
#define KEY_TRIG  20
#define KEY_LFO   25
#define KEY_STEP1 26
#define EV_KEY(ev)   (*(unsigned long *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(unsigned long *)((char *)(ev) + 16))
#define EV_ENC(ev)   (*(unsigned long *)((char *)(ev) + 12))
#define EV_DELTA(ev) (*(long *)((char *)(ev) + 16))
#define F_DOWN    1
#define F_FUNC    2
#define F_REPEAT  8
#define F_UP      0x10

enum { PG_NOTES, PG_GATES, PG_VEL, PG_PATH, PAGES };
static const char *const page_names[PAGES] = { "NOTES", "GATES", "VELOCITY", "PATH" };

unsigned char digisnake_pg;             /* the page shown */
void *digisnake_brain;                  /* the key dispatcher's brain, for ev_draw */
static unsigned char trig_held, trig_used, in_open;
static unsigned char stock_held, stock_used;   /* TRIG pressed on the stock TRIG page */
static unsigned char func_held;         /* FUNC is down (followed on every key event) */
static unsigned char redraw;            /* something on the page changed */
static long enc_acc[8];
#define COUNTS    4                     /* encoder counts a detent */
static unsigned long ui_rand = 0x9e3779b9UL;

static int ui_track(void)
{
    long t = fw_active_track;
    return t >= 0 && t < TRACKS ? (int)t : -1;
}

static struct core_page *page_for(long t)
{
    return t >= 4 ? &digisnake_page_midi : &digisnake_page;
}

static int page_up(void *brain, struct core_page *pg)
{
    return pg->view && core_page_shown(brain, pg);
}

static int shown(void *brain)
{
    return brain && (page_up(brain, &digisnake_page) || page_up(brain, &digisnake_page_midi));
}

/* The stock page just before ours (the TRIG page) on screen: -> our page, or 0. */
static struct core_page *stock_trig_up(void *brain)
{
    static struct core_page probe;
    struct core_page *pg = page_for(fw_active_track);
    if (!brain || !pg->view || pg->pos < 1)
        return 0;
    probe.view = pg->view;
    probe.pos = pg->pos - 1;
    return core_page_shown(brain, &probe) ? pg : 0;
}

static void randomise(struct snake *s, int pg)
{
    int c, r = s->set[P_RANGE];
    if (r < 1 || r > NOTE_MAX)
        r = 12;                         /* never all zeros */
    ui_rand ^= fw_timeline;
    for (c = 0; c < CELLS; c++) {
        unsigned long x = xorshift(&ui_rand);
        if (pg == PG_NOTES)
            s->note[c] = (signed char)(r ? (long)(x % (unsigned long)(2 * r + 1)) - r : 0);
        else if (pg == PG_VEL)
            s->vel[c] = (unsigned char)(40 + x % 88);
        else if (pg == PG_GATES) {
            if ((x >> 16) % 100 < 65)
                s->gate |= (unsigned char)(1u << c);
            else
                s->gate &= (unsigned char)~(1u << c);
        }
    }
    if (pg == PG_GATES && !s->gate)                 /* never all rests */
        s->gate = (unsigned char)(1u << (xorshift(&ui_rand) % CELLS));
    if (pg == PG_PATH) {                            /* everything but SNAKE and SCALE */
        static const unsigned char divs[8] = { 1, 1, 1, 1, 1, 2, 2, 4 };   /* mostly 1X */
        s->set[P_SHAPE] = (unsigned char)(xorshift(&ui_rand) % SHAPES);
        s->set[P_DIR] = (unsigned char)(xorshift(&ui_rand) % DIRS);
        s->set[P_LEN] = (unsigned char)(3 + xorshift(&ui_rand) % (CELLS - 2));   /* 3-8 */
        s->set[P_DIV] = divs[xorshift(&ui_rand) & 7];
        s->set[P_RANGE] = (unsigned char)(5 + xorshift(&ui_rand) % (NOTE_MAX - 4));   /* 5-24 */
        s->set[P_SHIFT] = (unsigned char)(xorshift(&ui_rand) % CELLS);
    }
}

static void reset_page(struct snake *s, int pg)
{
    struct snake d;
    int c;
    track_defaults(&d);
    for (c = 0; c < CELLS; c++) {
        if (pg == PG_NOTES)
            s->note[c] = 0;             /* a clear: every cell on the track's NOTE */
        else if (pg == PG_VEL)
            s->vel[c] = d.vel[c];
    }
    if (pg == PG_GATES)
        s->gate = 0xff;
    if (pg == PG_PATH) {
        unsigned char on = s->set[P_ON];
        for (c = 0; c < NSET; c++)
            s->set[c] = d.set[c];
        s->set[P_ON] = on;
    }
}

static int clampi(long v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

static void nudge_page(struct snake *s, int pg, int d, int func)
{
    int c;
    if (pg == PG_NOTES) {
        for (c = 0; c < CELLS; c++)
            s->note[c] = (signed char)clampi(s->note[c] + d * (func ? 12 : 1), -NOTE_MAX, NOTE_MAX);
    } else if (pg == PG_VEL) {
        for (c = 0; c < CELLS; c++)
            s->vel[c] = (unsigned char)clampi(s->vel[c] + d * 8, 1, 127);
    } else if (pg == PG_GATES) {
        unsigned g = s->gate;
        g = d > 0 ? ((g << 1) | (g >> 7)) : ((g >> 1) | (g << 7));
        s->gate = (unsigned char)g;
    }
}

static const char *const what_names[PAGES + 1] = { "Notes", "Gates", "Velocity", "Path", "Snake" };

/* COPY / PASTE (TRIG + REC / STOP): one page, from any track to any
 * other (synth or MIDI). A PATH paste keeps the track's own SNAKE on or off. */
static struct snake clip;
static signed char clip_pg = -1;            /* the page copied, -1: nothing yet */

static void paste_page(struct snake *s, const struct snake *from, int pg)
{
    int c;
    if (pg == PG_NOTES)
        for (c = 0; c < CELLS; c++) s->note[c] = from->note[c];
    else if (pg == PG_VEL)
        for (c = 0; c < CELLS; c++) s->vel[c] = from->vel[c];
    else if (pg == PG_GATES)
        s->gate = from->gate;
    else {
        unsigned char on = s->set[P_ON];
        for (c = 0; c < NSET; c++) s->set[c] = from->set[c];
        s->set[P_ON] = on;
    }
}

/* a change made by hand (or with TRIG held): it ends a FUNC + YES run */
static void edited(int t)
{
    run_active[t] = 0;
    changed();
}

static void reload(int t, const struct snake *from, int valid)
{
    if (!valid)
        return;
    digisnake_data[t] = *from;
    reset_req[t] = 1;
    changed();
    POPUP("Snake reloaded");
}

/* FUNC + knob: the next stop in the knob's direction */
static int next_stop(int v, long d, const signed char *stops, int n)
{
    int i;
    if (d > 0) {
        for (i = 0; i < n; i++)
            if (stops[i] > v)
                return stops[i];
        return stops[n - 1];
    }
    for (i = n - 1; i >= 0; i--)
        if (stops[i] < v)
            return stops[i];
    return stops[0];
}
static const signed char note_stops[5] = { -24, -12, 0, 12, 24 };
static const signed char vel_stops[5] = { 1, 32, 64, 96, 127 };
static const signed char range_stops[3] = { 1, 12, 24 };

/* Leave for TRIG's next page, as a press of TRIG would: core_page_open steps
 * the view until it shows the place given. */
static void next_trig_page(void *brain, void *ev)
{
    static struct core_page to;             /* only view and pos are read */
    struct core_page *pg = page_up(brain, &digisnake_page_midi) ? &digisnake_page_midi
                         : page_up(brain, &digisnake_page) ? &digisnake_page
                         : page_for(fw_active_track);
    unsigned char *view = pg->view;
    long n;
    if (!view)
        return;
    to.idx = pg->idx;
    to.view = view;
    n = (*(long *)(view + 128) - *(long *)(view + 124)) / 4;
    if (n < 2)
        return;
    to.pos = (pg->pos + 1) % n;
    in_open = 1;                            /* its presses are the stock's */
    core_page_open(brain, ev, KEY_TRIG, &to);
    in_open = 0;
}

int digisnake_key(void *brain, void *ev)
{
    unsigned long key = EV_KEY(ev), flags = EV_FLAGS(ev);
    int t, down = (flags & F_DOWN) && !(flags & F_REPEAT);
    struct snake *s;
    digisnake_brain = brain;
    /* FUNC, followed always, never taken. Its press is flags 1 (5 for a
     * double press); its release 0x10, or 0 when another key went down
     * meanwhile (FUNC + YES, say). Another key's event with F_FUNC says it is down. */
    if (key == KEY_FUNC) {
        if (!(flags & F_REPEAT))
            func_held = (unsigned char)(flags & F_DOWN);
    } else if (flags & F_FUNC)
        func_held = 1;
    /* TRIG on the stock TRIG page: ours, so the Snake page opens however long
     * TRIG was held (the stock steps a page only on a short press). Another
     * key while it is down leaves it to that key: no page change. */
    if (!in_open && stock_held) {
        if (key == KEY_TRIG) {
            if ((flags & F_UP) || !(flags & F_DOWN)) {
                struct core_page *pg = page_for(fw_active_track);
                stock_held = 0;
                if (!stock_used && pg->view) {
                    in_open = 1;
                    core_page_open(brain, ev, KEY_TRIG, pg);
                    in_open = 0;
                    redraw = 1;
                }
            }
            return 1;
        }
        if (flags & F_DOWN)
            stock_used = 1;
    } else if (!in_open && key == KEY_TRIG && down && !trig_held && stock_trig_up(brain)) {
        stock_held = 1;
        stock_used = 0;
        return 1;
    }
    if (in_open || (!trig_held && !shown(brain)))
        return 0;
    t = ui_track();
    if (t < 0)
        return 0;                           /* no track we know */
    s = &digisnake_data[t];
    redraw = 1;

    if (key == KEY_TRIG) {
        /* its release is 0x10, or 0 when another key went down meanwhile
         * (TRIG + YES): anything without F_DOWN is the release */
        if ((flags & F_UP) || !(flags & F_DOWN)) {
            if (trig_held && !trig_used) {
                trig_held = 0;
                next_trig_page(brain, ev);
            }
            trig_held = 0;
            return 1;
        }
        if (down) {
            trig_held = 1;
            trig_used = 0;
            print_t = -1;
            hold_copy[t] = *s;              /* what TRIG + NO goes back to */
            hold_valid[t] = 1;
        }
        return 1;
    }
    if (key >= KEY_TRIG && key <= KEY_LFO) {    /* another page key: stock */
        trig_held = 0;
        return 0;
    }
    if (!down)                              /* releases and repeats: stock, but */
        return key == KEY_LEFT || key == KEY_RIGHT     /* the arrows' would nudge the tempo */
            || key == KEY_UP || key == KEY_DOWN;

    if (key == KEY_YES && (flags & F_FUNC)) {   /* (with TRIG held too) */
        trig_used = 1;
        if (!run_active[t]) {               /* the first of a run: keep the start */
            run_copy[t] = *s;
            run_valid[t] = run_active[t] = 1;
        }
        randomise(s, PG_NOTES);
        randomise(s, PG_GATES);
        randomise(s, PG_VEL);
        changed();
        POPUP("Snake randomised");
        return 1;
    }
    if (key == KEY_NO && trig_held) {           /* as it was when TRIG went down */
        trig_used = 1;
        unprint(t);                             /* and a print in this hold undone */
        reload(t, &hold_copy[t], hold_valid[t]);
        run_active[t] = 0;
        return 1;
    }
    if (key == KEY_NO && (flags & F_FUNC)) {    /* as before the FUNC + YES run */
        trig_used = 1;
        reload(t, &run_copy[t], run_valid[t]);
        run_active[t] = 0;
        return 1;
    }
    if (key == KEY_YES && trig_held) {
        trig_used = 1;
        randomise(s, digisnake_pg);
        if (digisnake_pg == PG_PATH)
            reset_req[t] = 1;               /* the path changed shape: start it again */
        edited(t);
        POPUP("%s randomised", what_names[digisnake_pg]);
        return 1;
    }
    /* the stock page keys: TRIG + REC copies, + STOP pastes, + PLAY clears
     * (with FUNC too, as in 0.4 .. 0.7) */
    if (key == KEY_REC && trig_held && ((flags & F_FUNC) || func_held)) {    /* PRINT */
        int n;
        trig_used = 1;
        n = print_track(t);
        if (n < 0) {
            POPUP("No pattern to print to");
        } else {
            reset_req[t] = 1;
            edited(t);
            POPUP("Printed %d steps, snake off", n);
        }
        return 1;
    }
    if (key == KEY_REC && trig_held) {                                       /* COPY */
        trig_used = 1;
        clip = *s;
        clip_pg = (signed char)digisnake_pg;
        POPUP("%s copied", what_names[digisnake_pg]);
        return 1;
    }
    if (key == KEY_STOP && trig_held) {                                      /* PASTE */
        trig_used = 1;
        if (clip_pg < 0) {
            POPUP("Nothing to paste");
            return 1;
        }
        digisnake_pg = (unsigned char)clip_pg;     /* show what was pasted */
        paste_page(s, &clip, clip_pg);
        reset_req[t] = 1;
        edited(t);
        POPUP("%s pasted", what_names[clip_pg]);
        return 1;
    }
    if (key == KEY_PLAY && trig_held) {                                      /* CLEAR */
        trig_used = 1;
        reset_page(s, digisnake_pg);
        reset_req[t] = 1;
        edited(t);
        POPUP("%s cleared", what_names[digisnake_pg]);
        return 1;
    }
    if (key == KEY_LEFT) {
        digisnake_pg = (unsigned char)(digisnake_pg ? digisnake_pg - 1 : PAGES - 1);
        return 1;
    }
    if (key == KEY_RIGHT) {
        digisnake_pg = (unsigned char)((digisnake_pg + 1) % PAGES);
        return 1;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        nudge_page(s, digisnake_pg, key == KEY_UP ? 1 : -1, flags & F_FUNC);
        edited(t);
        return 1;
    }
    if (digisnake_pg == PG_GATES && key >= KEY_STEP1 && key < KEY_STEP1 + CELLS) {
        s->gate ^= (unsigned char)(1u << (key - KEY_STEP1));
        edited(t);
        return 1;
    }
    return 0;
}

int digisnake_enc(void *brain, void *ev)
{
    unsigned long e = EV_ENC(ev);
    long d;
    int t, c;
    struct snake *s;
    digisnake_brain = brain;
    if (e < 1 || e > 8 || !shown(brain))
        return 0;
    t = ui_track();
    if (t < 0)
        return 0;
    enc_acc[e - 1] += EV_DELTA(ev);
    d = enc_acc[e - 1] / COUNTS;
    enc_acc[e - 1] -= d * COUNTS;
    if (d == 0)
        return 1;
    s = &digisnake_data[t];
    c = (int)e - 1;
    redraw = 1;
    edited(t);
    switch (digisnake_pg) {
    case PG_NOTES:
        if (func_held) {
            s->note[c] = (signed char)next_stop(s->note[c], d, note_stops, 5);
            POPUP("Note %c=%c%d", 'A' + c, s->note[c] < 0 ? '-' : '+',
                  s->note[c] < 0 ? -s->note[c] : s->note[c]);
        } else {
            s->note[c] = (signed char)clampi(s->note[c] + d, -NOTE_MAX, NOTE_MAX);
        }
        break;
    case PG_GATES:
        if (d > 0)
            s->gate |= (unsigned char)(1u << c);
        else
            s->gate &= (unsigned char)~(1u << c);
        break;
    case PG_VEL:
        if (func_held) {
            s->vel[c] = (unsigned char)next_stop(s->vel[c], d, vel_stops, 5);
            POPUP("Velocity %c=%d", 'A' + c, s->vel[c]);
        } else {
            s->vel[c] = (unsigned char)clampi(s->vel[c] + d, 1, 127);
        }
        break;
    default:
        if (func_held && c == P_RANGE) {
            s->set[c] = (unsigned char)next_stop(s->set[c], d, range_stops, 3);
            POPUP("Range=+-%d", s->set[c]);
        } else if (c == P_DIV) {
            s->set[c] = (unsigned char)div_turn(s->set[c], d);
            if (s->set[c] == 0 && !per_track_mode())
                POPUP("2X needs SCALE per track");
            else
                POPUP("Division=%s", div_name(s->set[c]));
        } else {
            s->set[c] = (unsigned char)clampi(s->set[c] + d, set_min[c], set_max[c]);
        }
        if (c == P_ON || c == P_DIR || c == P_LEN || c == P_SHAPE)
            reset_req[t] = 1;
        break;
    }
    return 1;
}

/* ---- drawing (ev_tick, ev_draw): 128 x 64, y = 0 the bottom row --------- */
typedef void (*rect_fn)(void *bmp, int x0, int y0, int x1, int y1, int colour);
typedef int (*text_fn)(void *bmp, const void *font, int x, int y, int maxlen, const char *fmt, ...);
#define FILLRECT  ((rect_fn)fw_fillrect)
#define FRAMERECT ((rect_fn)fw_framerect)
#define TEXTF     ((text_fn)fw_textf)
#define FONT5     ((const void *)fw_font5)

static const char *const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void digisnake_tick(void *ctrl)
{
    int t;
    if (!data_ready) {                      /* no project load reached us yet */
        for (t = 0; t < TRACKS; t++)
            track_defaults(&digisnake_data[t]);
        data_ready = 1;
    }
    if (fw_transport == 0)
        for (t = 0; t < TRACKS; t++)
            rewind[t] = 1;                  /* PLAY starts every path again */
    if (save_wait && !--save_wait)
        mirror_request();                   /* 1.5 s after the last change */
    speed_sync();
    if (shown(digisnake_brain)) {
        /* Redraw only when something the page shows has changed: an edit, the
         * playhead (the cell) or the transport. It used to redraw every frame. */
        static int last_cell = -1, last_play = -1, last_t = -1;
        int ut = ui_track(), cell = ut >= 0 ? digisnake_cell[ut] : -1;
        int play = (int)fw_transport;
        if (redraw || cell != last_cell || play != last_play || ut != last_t) {
            redraw = 0;
            last_cell = cell;
            last_play = play;
            last_t = ut;
            *((unsigned char *)ctrl + 0x20) = 1;
        }
    }
}

static int path_order(const struct snake *s, int cell)
{
    int len = s->set[P_LEN], k;
    for (k = 0; k < len; k++)
        if (cell_of(s, k, len) == cell)
            return k + 1;
    return 0;                               /* not on the path */
}

void digisnake_draw(void *bmp, void *ctrl)
{
    int t, c, playing;
    const struct snake *s;
    struct sq_track info;
    int base = 60, root = 0;
    (void)ctrl;
    if (!shown(digisnake_brain))
        return;
    t = ui_track();
    /* Rows 53-63 stay the Digitone's: its title bar, and the drop-down
     * messages it draws there, which this draw (after every view's) would
     * otherwise cover. Our own line is at the bottom. */
    FILLRECT(bmp, 0, 0, 127, 52, 0);
    if (t < 0) {
        TEXTF(bmp, FONT5, 1, 30, -1, "SNAKE: NO TRACK");
        return;
    }
    s = &digisnake_data[t];
    if (sq_track_info(t, &info, sizeof info) == 0) {
        base = info.note;
        if (info.root != SQ_ROOT_NONE)
            root = info.root % 12;
    }
    playing = s->set[P_ON] && fw_transport == 1;


    for (c = 0; c < CELLS; c++) {
        int x0 = 32 * (c & 3), y0 = (c < 4) ? 30 : 7;   /* top row: A-D */
        int x1 = x0 + 30, y1 = y0 + 21, order;
        FRAMERECT(bmp, x0, y0, x1, y1, 1);
        if (digisnake_pg == PG_PATH) {
            int v = s->set[c];
            TEXTF(bmp, FONT5, x0 + 3, y0 + 14, -1, "%s", set_labels[c]);
            switch (c) {
            case P_ON:    TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", v ? "ON" : "OFF"); break;
            case P_SHAPE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", shape_names[v % SHAPES]); break;
            case P_DIR:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", dir_names[v % DIRS]); break;
            case P_SCALE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", scale_names[v % SCALES]); break;
            case P_RANGE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "+-%d", v); break;
            case P_DIV:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", div_name(v)); break;
            case P_SHIFT: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%d", v); break;
            default:      TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%d", v); break;
            }
            continue;
        }
        order = path_order(s, c);
        if (order)
            TEXTF(bmp, FONT5, x1 - 6, y1 - 6, -1, "%d", order);   /* the path's order */
        if (playing && digisnake_cell[t] == c) {                  /* the playhead */
            FRAMERECT(bmp, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1);
            FRAMERECT(bmp, x0 + 2, y0 + 2, x1 - 2, y1 - 2, 1);
        }
        if (digisnake_pg == PG_NOTES) {
            int n = quantise(base + s->note[c], root, s->set[P_SCALE]);
            if (n < 0) n = 0;
            if (n > 127) n = 127;
            TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "%s%d", note_names[n % 12], n / 12);  /* 60 is C5, as the Digitone names it */
            TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%c%d", s->note[c] < 0 ? '-' : '+',
                  s->note[c] < 0 ? -s->note[c] : s->note[c]);
        } else if (digisnake_pg == PG_GATES) {
            if ((s->gate >> c) & 1)
                FILLRECT(bmp, x0 + 8, y0 + 5, x1 - 10, y1 - 5, 1);
            else
                FRAMERECT(bmp, x0 + 8, y0 + 5, x1 - 10, y1 - 5, 1);
        } else {
            int h = (s->vel[c] * 14) / 127;
            FILLRECT(bmp, x0 + 3, y0 + 4, x0 + 6, y0 + 4 + h, 1);
            TEXTF(bmp, FONT5, x0 + 10, y0 + 8, -1, "%d", s->vel[c]);
        }
    }
    TEXTF(bmp, FONT5, 1, 0, -1, "%s%d SNAKE %s", t < 4 ? "T" : "MIDI ", (t & 3) + 1,
          s->set[P_ON] ? "ON" : "OFF");
    TEXTF(bmp, FONT5, 72, 0, -1, "%s %d/4", page_names[digisnake_pg], digisnake_pg + 1);
}
