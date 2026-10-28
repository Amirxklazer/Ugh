// RumbleNX - Joy-Con HD rumble studio for Nintendo Switch (C++ / SDL2 / libnx)
//
//  SONG    : touch step-sequencer, two tracks (A -> left Joy-Con, B -> right Joy-Con),
//            notes are played as tones through the HD rumble actuators.
//  ACTIONS : soundboard of vibration actions (buzz, crawl, heartbeat, purr, gag...)
//            and pose triggers: put a Joy-Con upright / on its side / laying flat and
//            it runs the action you assigned to that pose.
//
// Detach the Joy-Con (they must be paired as a Dual set), put them on a table, use the touch screen.

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

static const int SW = 1280, SH = 720;
static const char* SAVE_DIR = "/switch/RumbleNX";

// ------------------------------------------------------------------ helpers
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float frand() { return (float)rand() / (float)RAND_MAX; }
static float midiHz(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

// ------------------------------------------------------------------ rendering / text
static SDL_Window*   win = nullptr;
static SDL_Renderer* ren = nullptr;
static TTF_Font*     fonts[3] = {nullptr, nullptr, nullptr};
static const int     FONT_PT[3] = {20, 26, 36};

static const SDL_Color C_BG   = {16, 14, 30, 255};
static const SDL_Color C_PNL  = {32, 28, 56, 255};
static const SDL_Color C_BTN  = {58, 52, 98, 255};
static const SDL_Color C_HOT  = {88, 80, 140, 255};
static const SDL_Color C_ON   = {255, 110, 170, 255};
static const SDL_Color C_TXT  = {240, 238, 255, 255};
static const SDL_Color C_DIM  = {150, 145, 188, 255};
static const SDL_Color C_DARK = {20, 16, 36, 255};
static const SDL_Color C_ACC  = {110, 220, 255, 255};
static const SDL_Color C_RED  = {220, 70, 80, 255};

struct CachedText { SDL_Texture* tex; int w, h; };
static std::unordered_map<std::string, CachedText> tcache;

static void clearTextCache() {
    for (auto& p : tcache) SDL_DestroyTexture(p.second.tex);
    tcache.clear();
}

static void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

static void strokeRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderDrawRect(ren, &r);
}

// align: 0 left, 1 center, 2 right (relative to x). y is the top of the text.
static void text(const std::string& s, int x, int y, int sz, SDL_Color c, int align = 0) {
    if (s.empty() || !fonts[sz]) return;
    std::string key = std::to_string(sz) + ":" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                      std::to_string(c.b) + ":" + s;
    auto it = tcache.find(key);
    if (it == tcache.end()) {
        SDL_Surface* sf = TTF_RenderUTF8_Blended(fonts[sz], s.c_str(), c);
        if (!sf) return;
        SDL_Texture* t = SDL_CreateTextureFromSurface(ren, sf);
        CachedText ct = {t, sf->w, sf->h};
        SDL_FreeSurface(sf);
        if (!t) return;
        it = tcache.emplace(key, ct).first;
    }
    int dx = x;
    if (align == 1) dx = x - it->second.w / 2;
    else if (align == 2) dx = x - it->second.w;
    SDL_Rect dst = {dx, y, it->second.w, it->second.h};
    SDL_RenderCopy(ren, it->second.tex, nullptr, &dst);
}

// ------------------------------------------------------------------ input (touch + pad)
static PadState pad;
static bool tapped = false;      // a finger went down this frame
static int  tapX = 0, tapY = 0;
static bool held = false;        // a finger is down
static int  heldX = 0, heldY = 0;

static bool inside(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// immediate-mode button: draws and returns true when tapped
static bool button(int x, int y, int w, int h, const std::string& label, bool on = false, int sz = 0,
                   const SDL_Color* base = nullptr) {
    bool hot = held && inside(x, y, w, h, heldX, heldY);
    SDL_Color bg = on ? C_ON : (hot ? C_HOT : (base ? *base : C_BTN));
    fillRect(x, y, w, h, bg);
    strokeRect(x, y, w, h, on ? C_TXT : C_DIM);
    int th = TTF_FontHeight(fonts[sz]);
    text(label, x + w / 2, y + (h - th) / 2, sz, on ? C_DARK : C_TXT, 1);
    if (tapped && inside(x, y, w, h, tapX, tapY)) { tapped = false; return true; }
    return false;
}

// ------------------------------------------------------------------ HD rumble engine
static HidVibrationDeviceHandle vib[2][2];   // [set: 0 handheld, 1 joy-con dual][side: 0 left, 1 right]
static float lastF[2] = {0, 0}, lastA[2] = {0, 0};
static bool  lastSent[2] = {false, false};

static void rumbleInit() {
    hidInitializeVibrationDevices(vib[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidInitializeVibrationDevices(vib[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidPermitVibration(true);
}

// Plays a single tone on one Joy-Con. The Joy-Con actuator has two bands:
// low  40..640 Hz (resonant ~160), high 80..1280 Hz (resonant ~320). A pure tone uses one band.
static void sendTone(int side, float freq, float amp) {
    bool silent = amp < 0.001f;
    if (silent && !lastSent[side]) return;      // nothing to stop
    HidVibrationValue v;
    v.amp_low = 0.0f;  v.freq_low = 160.0f;
    v.amp_high = 0.0f; v.freq_high = 320.0f;
    if (!silent) {
        amp = clampf(amp, 0.0f, 1.0f);
        if (freq <= 400.0f) { v.freq_low = clampf(freq, 40.0f, 640.0f);   v.amp_low = amp; }
        else                { v.freq_high = clampf(freq, 80.0f, 1280.0f); v.amp_high = amp; }
    }
    hidSendVibrationValue(vib[0][side], &v);
    hidSendVibrationValue(vib[1][side], &v);
    lastSent[side] = !silent;
    lastF[side] = freq; lastA[side] = amp;
}

// ------------------------------------------------------------------ vibration actions
enum { P_BUZZ, P_CRAWL, P_HEART, P_PURR, P_GAG, P_RATTLE, P_SIREN, P_PULSE, P_COUNT };
static const char* PAT_NAME[P_COUNT] = {"BUZZ", "CRAWL", "HEARTBEAT", "PURR", "GAG", "RATTLE", "SIREN", "PULSE"};
static const char* PAT_DESC[P_COUNT] = {
    "steady buzz", "creeps along", "thump-thump", "soft low purr",
    "ugh... (retching)", "harsh noise", "rising and falling", "on / off pulses"};

// t = seconds since the action started. Writes frequency (Hz) and amplitude (0..1).
static void evalPat(int id, float t, float& f, float& a) {
    f = 160.0f; a = 0.0f;
    switch (id) {
    case P_BUZZ:
        f = 160.0f; a = 1.0f;
        break;
    case P_CRAWL: {                       // short bursts that alternate frequency -> makes it creep
        float p = fmodf(t, 0.5f);
        if (p < 0.32f) { f = (((int)(t * 28.0f)) & 1) ? 80.0f : 165.0f; a = 1.0f; }
        break;
    }
    case P_HEART: {
        float p = fmodf(t, 0.95f);
        if (p < 0.09f || (p > 0.20f && p < 0.31f)) { f = 70.0f; a = 1.0f; }
        break;
    }
    case P_PURR:
        f = 55.0f + 8.0f * sinf(t * 12.5f);
        a = 0.55f + 0.25f * sinf(t * 5.0f);
        break;
    case P_GAG: {                         // three convulsions, then a wet rattle, then rest
        float p = fmodf(t, 2.4f);
        for (int i = 0; i < 3; i++) {
            float s = i * 0.30f;
            if (p >= s && p < s + 0.20f) {
                float k = (p - s) / 0.20f;
                f = 70.0f + 60.0f * k;
                a = 0.5f + 0.5f * k;
                return;
            }
        }
        if (p >= 0.85f && p < 1.55f) {
            f = 60.0f + 35.0f * frand();
            a = (((int)(p * 30.0f)) & 1) ? 0.9f : 0.35f;
        }
        break;
    }
    case P_RATTLE:
        f = 500.0f + 700.0f * frand(); a = 0.8f;
        break;
    case P_SIREN: {
        float p = fmodf(t, 1.6f) / 1.6f;
        float tri = p < 0.5f ? p * 2.0f : 2.0f - p * 2.0f;
        f = 100.0f + 320.0f * tri; a = 1.0f;
        break;
    }
    case P_PULSE:
        if (fmodf(t, 0.5f) < 0.25f) { f = 200.0f; a = 1.0f; }
        break;
    }
}

// ------------------------------------------------------------------ song data
static const int MAXS = 128;
struct Song {
    int tempo = 120;                  // quarter notes per minute; one step = a 16th note
    int len = 32;                     // steps
    signed char n[2][MAXS];           // midi note per step, -1 = rest
};
static Song song;
static int  editTrack = 0;            // 0 = A (left Joy-Con), 1 = B (right Joy-Con)
static int  pageIdx = 0;              // 16 steps per page
static int  baseNote = 52;            // lowest visible midi note
static bool playing = false, loopOn = true;
static float playPos = 0.0f;          // in steps
static int  slot = 1;
static std::string statusMsg;
static double statusUntil = 0.0;
static const int ROWS = 15;

static const char* NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static bool isBlack(int m) { int k = m % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
static std::string noteLabel(int m) { return std::string(NOTE_NAME[m % 12]) + std::to_string(m / 12 - 1); }

static void songClear(int track) { for (int i = 0; i < MAXS; i++) song.n[track][i] = -1; }

static void songDefault() {
    song.tempo = 120; song.len = 32;
    songClear(0); songClear(1);
    // a tiny original arpeggio so there is something to hear straight away
    const int a[7][2] = {{0, 60}, {2, 64}, {4, 67}, {6, 72}, {8, 67}, {10, 64}, {12, 60}};
    for (auto& e : a) song.n[0][e[0]] = (signed char)e[1];
    song.n[1][0] = 48; song.n[1][8] = 43;
}

static void setStatus(const std::string& s, double now) { statusMsg = s; statusUntil = now + 3.0; }

static bool songSave(int s) {
    mkdir("/switch", 0777);
    mkdir(SAVE_DIR, 0777);
    char path[128];
    snprintf(path, sizeof(path), "%s/song%d.txt", SAVE_DIR, s);
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "RUMBLENX1\n%d %d\n", song.tempo, song.len);
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < song.len; i++) fprintf(f, "%d ", (int)song.n[t][i]);
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

static bool songLoad(int s) {
    char path[128];
    snprintf(path, sizeof(path), "%s/song%d.txt", SAVE_DIR, s);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char magic[16] = {0};
    int tempo = 0, len = 0;
    if (fscanf(f, "%15s %d %d", magic, &tempo, &len) != 3 || strcmp(magic, "RUMBLENX1") != 0 ||
        len < 16 || len > MAXS || tempo < 40 || tempo > 300) { fclose(f); return false; }
    Song tmp;
    tmp.tempo = tempo; tmp.len = len;
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < MAXS; i++) tmp.n[t][i] = -1;
        for (int i = 0; i < len; i++) {
            int v = -1;
            if (fscanf(f, "%d", &v) != 1) { fclose(f); return false; }
            tmp.n[t][i] = (signed char)v;
        }
    }
    fclose(f);
    song = tmp;
    return true;
}

// ------------------------------------------------------------------ Joy-Con poses (accelerometer)
static const char* POSE_NAME[3] = {"UPRIGHT", "SIDE", "FLAT"};
static HidSixAxisSensorHandle sxDual[2], sxHand[2];
static float g[2][3];                 // low-pass filtered gravity (in G)
static bool  gInit[2] = {false, false};
static bool  imuOk[2] = {false, false};
static float poseVec[2][3][3];
static bool  poseCal[2][3];
static int   pose[2] = {-1, -1};
static int   poseCand[2] = {-1, -1};
static int   poseCnt[2] = {0, 0};
static int   mapAct[2][3] = {{P_GAG, P_BUZZ, P_CRAWL}, {P_GAG, P_BUZZ, P_CRAWL}};
static double patStart[2] = {0, 0};
static bool  armed = false;
static int   boardAct = -1;           // soundboard action currently playing
static int   boardTarget = 2;         // 0 left, 1 right, 2 both
static double boardStart = 0;

static void imuInit() {
    memset(sxDual, 0, sizeof(sxDual));
    memset(sxHand, 0, sizeof(sxHand));
    hidGetSixAxisSensorHandles(sxDual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    if (R_FAILED(hidGetSixAxisSensorHandles(sxHand, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
        hidGetSixAxisSensorHandles(sxHand, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        sxHand[1] = sxHand[0];
    }
    for (int i = 0; i < 2; i++) { hidStartSixAxisSensor(sxDual[i]); hidStartSixAxisSensor(sxHand[i]); }
    memset(poseCal, 0, sizeof(poseCal));
}

static int classify(int s) {
    float n = sqrtf(g[s][0] * g[s][0] + g[s][1] * g[s][1] + g[s][2] * g[s][2]);
    if (n < 0.6f || n > 1.6f) return -1;                 // moving / shaking, not trustworthy
    float u[3] = {g[s][0] / n, g[s][1] / n, g[s][2] / n};
    int best = -1; float bd = 0.80f;
    for (int p = 0; p < 3; p++) {
        float d;
        if (poseCal[s][p]) d = u[0] * poseVec[s][p][0] + u[1] * poseVec[s][p][1] + u[2] * poseVec[s][p][2];
        else               d = fabsf(u[p]);              // default guess: x=long axis, y=width, z=face
        if (d > bd) { bd = d; best = p; }
    }
    return best;
}

static void imuUpdate(double now) {
    u32 st = padGetStyleSet(&pad);
    HidSixAxisSensorHandle* hs = nullptr;
    if (st & HidNpadStyleTag_NpadJoyDual) hs = sxDual;
    else if (st & HidNpadStyleTag_NpadHandheld) hs = sxHand;
    for (int s = 0; s < 2; s++) {
        imuOk[s] = false;
        if (!hs) { pose[s] = -1; continue; }
        HidSixAxisSensorState ss;
        memset(&ss, 0, sizeof(ss));
        if (hidGetSixAxisSensorStates(hs[s], &ss, 1) > 0) {
            imuOk[s] = true;
            float a[3] = {ss.acceleration.x, ss.acceleration.y, ss.acceleration.z};
            for (int k = 0; k < 3; k++) {
                if (!gInit[s]) g[s][k] = a[k];
                else g[s][k] = g[s][k] * 0.85f + a[k] * 0.15f;
            }
            gInit[s] = true;
            int best = classify(s);
            if (best == pose[s]) { poseCnt[s] = 0; poseCand[s] = best; }
            else {
                if (best == poseCand[s]) poseCnt[s]++;
                else { poseCand[s] = best; poseCnt[s] = 1; }
                int need = (best == -1) ? 90 : 12;       // leaving a pose takes longer than entering one
                if (poseCnt[s] >= need) { pose[s] = best; patStart[s] = now; poseCnt[s] = 0; }
            }
        }
    }
}

// ------------------------------------------------------------------ misc state
static int  page = 0;                 // 0 = SONG, 1 = ACTIONS
static bool quitApp = false;
static double previewUntil[2] = {0, 0};
static float  previewHz[2] = {0, 0};

static void stopAll() {
    playing = false; playPos = 0.0f;
    armed = false; boardAct = -1;
    previewUntil[0] = previewUntil[1] = 0;
}

// ------------------------------------------------------------------ output mixer
static void computeOutput(double now, double dt) {
    if (playing) {
        playPos += (float)(dt * song.tempo * 4.0 / 60.0);
        if (playPos >= (float)song.len) {
            if (loopOn) playPos = fmodf(playPos, (float)song.len);
            else { playing = false; playPos = 0.0f; }
        }
    }
    for (int s = 0; s < 2; s++) {
        float F = 160.0f, A = 0.0f;
        if (armed && pose[s] >= 0) evalPat(mapAct[s][pose[s]], (float)(now - patStart[s]), F, A);
        if (boardAct >= 0 && (boardTarget == 2 || boardTarget == s)) evalPat(boardAct, (float)(now - boardStart), F, A);
        if (playing) {
            int idx = (int)playPos;
            float frac = playPos - (float)idx;
            if (idx >= 0 && idx < song.len) {
                int m = song.n[s][idx];
                if (m >= 0 && frac < 0.85f) { F = midiHz(m); A = 0.95f; }
                else { A = 0.0f; }
            }
        }
        if (now < previewUntil[s]) { F = previewHz[s]; A = 0.9f; }
        sendTone(s, F, A);
    }
}

// ------------------------------------------------------------------ UI: header
static void drawHeader() {
    fillRect(0, 0, SW, 62, C_PNL);
    if (button(20, 8, 150, 46, "SONG", page == 0, 1)) page = 0;
    if (button(180, 8, 190, 46, "ACTIONS", page == 1, 1)) page = 1;
    text("RUMBLENX", 560, 14, 2, C_ACC, 0);
    if (button(940, 8, 150, 46, "STOP ALL", false, 0, &C_RED)) stopAll();
    if (button(1100, 8, 150, 46, "EXIT", false, 0)) quitApp = true;
}

// ------------------------------------------------------------------ UI: SONG page
static const int GX = 90, GY = 126, CW = 73, CH = 34;

static void drawSongPage(double now) {
    int y = 72;
    if (button(20, y, 120, 44, playing ? "STOP" : "PLAY", playing, 0)) {
        playing = !playing;
        if (playing) playPos = (float)(pageIdx * 16);
        else playPos = 0.0f;
    }
    if (button(150, y, 50, 44, "-", false, 1)) song.tempo = std::max(40, song.tempo - 5);
    text(std::to_string(song.tempo) + " BPM", 250, y + 12, 0, C_TXT, 1);
    if (button(300, y, 50, 44, "+", false, 1)) song.tempo = std::min(300, song.tempo + 5);

    std::string tl = editTrack == 0 ? "A: LEFT" : "B: RIGHT";
    if (button(360, y, 180, 44, tl, true, 0)) editTrack = 1 - editTrack;

    static const int LENS[] = {16, 32, 48, 64, 96, 128};
    if (button(550, y, 110, 44, "LEN " + std::to_string(song.len), false, 0)) {
        int ni = 0;
        for (int i = 0; i < 6; i++) if (LENS[i] == song.len) ni = (i + 1) % 6;
        song.len = LENS[ni];
        if (pageIdx * 16 >= song.len) pageIdx = (song.len - 1) / 16;
    }
    if (button(670, y, 100, 44, "LOOP", loopOn, 0)) loopOn = !loopOn;
    if (button(780, y, 100, 44, "CLEAR", false, 0)) { songClear(editTrack); setStatus("Cleared track", now); }
    if (button(890, y, 100, 44, "SLOT " + std::to_string(slot), false, 0)) slot = slot % 4 + 1;
    if (button(1000, y, 100, 44, "SAVE", false, 0))
        setStatus(songSave(slot) ? "Saved slot " + std::to_string(slot) : "Save failed", now);
    if (button(1110, y, 100, 44, "LOAD", false, 0)) {
        if (songLoad(slot)) { pageIdx = 0; playPos = 0.0f; setStatus("Loaded slot " + std::to_string(slot), now); }
        else setStatus("Nothing in slot " + std::to_string(slot), now);
    }

    if (playing) pageIdx = std::min((int)playPos / 16, (song.len - 1) / 16);

    // piano-roll grid
    int step0 = pageIdx * 16;
    for (int r = 0; r < ROWS; r++) {
        int m = baseNote + (ROWS - 1 - r);
        int ry = GY + r * CH;
        fillRect(GX, ry, CW * 16, CH, isBlack(m) ? C_DARK : C_PNL);
        std::string lab = noteLabel(m);
        text(lab, GX - 8, ry + 6, 0, (m % 12 == 0) ? C_ACC : C_DIM, 2);
    }
    for (int c = 0; c <= 16; c++) {
        SDL_Color lc = (c % 4 == 0) ? C_DIM : C_BTN;
        fillRect(GX + c * CW, GY, 1, ROWS * CH, lc);
    }
    for (int r = 0; r <= ROWS; r++) fillRect(GX, GY + r * CH, CW * 16, 1, C_BTN);

    for (int c = 0; c < 16; c++) {
        int st = step0 + c;
        if (st >= song.len) {
            fillRect(GX + c * CW, GY, CW, ROWS * CH, C_BG);
            continue;
        }
        for (int t = 0; t < 2; t++) {
            int m = song.n[t][st];
            if (m < baseNote || m >= baseNote + ROWS) continue;
            int r = ROWS - 1 - (m - baseNote);
            SDL_Color col = (t == editTrack) ? C_ON : SDL_Color{80, 130, 160, 255};
            fillRect(GX + c * CW + 2, GY + r * CH + 2, CW - 4, CH - 4, col);
            if (t == editTrack) text(noteLabel(m), GX + c * CW + CW / 2, GY + r * CH + 6, 0, C_DARK, 1);
        }
    }
    if (playing) {
        int c = (int)playPos - step0;
        if (c >= 0 && c < 16) strokeRect(GX + c * CW, GY, CW, ROWS * CH, C_ACC);
    }

    // bottom bar
    int by = 648;
    if (button(90, by, 150, 50, "< PAGE", false, 0) && pageIdx > 0) pageIdx--;
    if (button(250, by, 150, 50, "PAGE >", false, 0) && (pageIdx + 1) * 16 < song.len) pageIdx++;
    if (button(420, by, 150, 50, "LOWER", false, 0)) baseNote = std::max(36, baseNote - 6);
    if (button(580, by, 150, 50, "HIGHER", false, 0)) baseNote = std::min(70, baseNote + 6);
    text("PAGE " + std::to_string(pageIdx + 1) + "/" + std::to_string((song.len + 15) / 16) + "   " +
         noteLabel(baseNote) + " - " + noteLabel(baseNote + ROWS - 1), 750, by + 12, 0, C_DIM, 0);
    if (now < statusUntil) text(statusMsg, 1250, by + 12, 0, C_ACC, 2);

    // grid touch: toggle a note
    if (tapped && inside(GX, GY, CW * 16, ROWS * CH, tapX, tapY)) {
        int c = (tapX - GX) / CW;
        int r = (tapY - GY) / CH;
        int st = step0 + c;
        int m = baseNote + (ROWS - 1 - r);
        if (st < song.len) {
            if (song.n[editTrack][st] == m) song.n[editTrack][st] = -1;
            else {
                song.n[editTrack][st] = (signed char)m;
                if (!playing) { previewHz[editTrack] = midiHz(m); previewUntil[editTrack] = now + 0.18; }
            }
        }
        tapped = false;
    }
}

// ------------------------------------------------------------------ UI: ACTIONS page
static void drawActionsPage(double now) {
    // soundboard
    text("SOUNDBOARD", 30, 76, 1, C_ACC, 0);
    static const char* TN[3] = {"LEFT", "RIGHT", "BOTH"};
    static const int TV[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++)
        if (button(30 + i * 130, 116, 120, 44, TN[i], boardTarget == TV[i], 0)) boardTarget = TV[i];

    for (int i = 0; i < P_COUNT; i++) {
        int col = i % 2, row = i / 2;
        int x = 30 + col * 310, y = 176 + row * 94;
        bool on = (boardAct == i);
        if (button(x, y, 290, 82, PAT_NAME[i], on, 1)) {
            if (on) boardAct = -1;
            else { boardAct = i; boardStart = now; }
        }
        text(PAT_DESC[i], x + 145, y + 54, 0, on ? C_DARK : C_DIM, 1);
    }
    text("Tap an action to start it, tap again to stop.", 30, 560, 0, C_DIM, 0);
    text("Plus / Minus button also stops everything.", 30, 586, 0, C_DIM, 0);

    // pose triggers
    if (button(680, 76, 570, 52, armed ? "ARMED - poses trigger actions" : "DISARMED - tap to arm", armed, 1)) {
        armed = !armed;
        for (int s = 0; s < 2; s++) patStart[s] = now;
    }
    for (int s = 0; s < 2; s++) {
        int px = 680 + s * 295, py = 140;
        fillRect(px, py, 275, 500, C_PNL);
        text(s == 0 ? "LEFT JOY-CON" : "RIGHT JOY-CON", px + 137, py + 8, 0, C_ACC, 1);
        std::string ps;
        if (!imuOk[s]) ps = "no sensor (detach Joy-Con)";
        else ps = pose[s] >= 0 ? std::string("NOW: ") + POSE_NAME[pose[s]] : "NOW: moving...";
        text(ps, px + 137, py + 36, 0, imuOk[s] && pose[s] >= 0 ? C_ON : C_DIM, 1);
        char gb[64];
        snprintf(gb, sizeof(gb), "G %.1f %.1f %.1f", g[s][0], g[s][1], g[s][2]);
        if (imuOk[s]) text(gb, px + 137, py + 62, 0, C_DIM, 1);

        for (int p = 0; p < 3; p++) {
            int ry = py + 96 + p * 132;
            text(POSE_NAME[p], px + 10, ry, 0, pose[s] == p ? C_ON : C_TXT, 0);
            std::string setLabel = poseCal[s][p] ? "SET (done)" : "SET";
            if (button(px + 150, ry - 4, 115, 34, setLabel, false, 0) && imuOk[s]) {
                float n = sqrtf(g[s][0] * g[s][0] + g[s][1] * g[s][1] + g[s][2] * g[s][2]);
                if (n > 0.5f) {
                    for (int k = 0; k < 3; k++) poseVec[s][p][k] = g[s][k] / n;
                    poseCal[s][p] = true;
                }
            }
            if (button(px + 10, ry + 38, 255, 60, PAT_NAME[mapAct[s][p]], pose[s] == p && armed, 1))
                mapAct[s][p] = (mapAct[s][p] + 1) % P_COUNT;
        }
    }
    text("Hold a Joy-Con in a pose, tap SET to teach it.", 680, 650, 0, C_DIM, 0);
    text("Tap an action name to change it.", 680, 676, 0, C_DIM, 0);
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    (void)argc; (void)argv;

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&pad);
    hidInitializeTouchScreen();
    rumbleInit();
    imuInit();
    plInitialize(PlServiceType_User);

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    win = SDL_CreateWindow("RumbleNX", 0, 0, SW, SH, SDL_WINDOW_SHOWN);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, SW, SH);

    PlFontData fd;
    if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
        for (int i = 0; i < 3; i++)
            fonts[i] = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 0, FONT_PT[i]);
    }

    songDefault();
    srand((unsigned)time(nullptr));

    u32 lastTick = SDL_GetTicks();
    u32 prevTouches = 0;

    while (appletMainLoop() && !quitApp) {
        u32 tick = SDL_GetTicks();
        double dt = clampf((float)(tick - lastTick) / 1000.0f, 0.0f, 0.1f);
        lastTick = tick;
        double now = tick / 1000.0;

        // frame input
        padUpdate(&pad);
        u64 kd = padGetButtonsDown(&pad);
        if (kd & (HidNpadButton_Plus | HidNpadButton_Minus)) stopAll();

        HidTouchScreenState ts;
        memset(&ts, 0, sizeof(ts));
        hidGetTouchScreenStates(&ts, 1);
        tapped = false;
        held = ts.count > 0;
        if (held) {
            heldX = (int)ts.touches[0].x; heldY = (int)ts.touches[0].y;
            if (prevTouches == 0) { tapped = true; tapX = heldX; tapY = heldY; }
        }
        prevTouches = ts.count;

        imuUpdate(now);

        if (tcache.size() > 400) clearTextCache();

        SDL_SetRenderDrawColor(ren, C_BG.r, C_BG.g, C_BG.b, 255);
        SDL_RenderClear(ren);

        // header first so STOP/EXIT/tabs win the tap; the page is then decided once per frame
        drawHeader();
        if (page == 0) drawSongPage(now);
        else drawActionsPage(now);

        computeOutput(now, dt);
        SDL_RenderPresent(ren);
    }

    // silence both Joy-Con and tidy up
    for (int s = 0; s < 2; s++) { lastSent[s] = true; sendTone(s, 160.0f, 0.0f); }
    clearTextCache();
    for (int i = 0; i < 3; i++) if (fonts[i]) TTF_CloseFont(fonts[i]);
    TTF_Quit();
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    plExit();
    return 0;
}
