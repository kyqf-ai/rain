#ifndef RAIN_UI_LOGIC_H
#define RAIN_UI_LOGIC_H
#include "collector.h"

/* Pure presentation logic for the tray program: no Windows calls, fully testable. All text is UTF-8. */
typedef enum { LV_GRAY = 0, LV_GREEN, LV_YELLOW, LV_RED } level_t;

typedef struct {
    level_t level;
    char status[64];        /* headline: "在线" ... */
    char detail[200];       /* extra explanation, may be empty */
    char device[160];
    char contact[64];
    char today[200];
    char power[160];
    char clock[80];
    char tip[120];          /* tray tooltip */
    int  balloon;           /* 0 none, 1 info, 2 warning */
    char balloon_title[96];
    char balloon_text[200];
} ui_view_t;

typedef struct {
    int down_notified;      /* a "communication lost" balloon is outstanding */
    int batt_warned;
    int64_t batt_warn_ms;
} ui_state_t;

void ui_state_init(ui_state_t *st);
/* port: configured serial port (shown in the device line); poll_sec: collector interval */
void ui_compute(ui_state_t *st, const col_status_t *cs, const char *port, int poll_sec, int64_t now_ms, ui_view_t *v);
#endif
