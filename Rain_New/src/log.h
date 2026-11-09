#ifndef RAIN_LOG_H
#define RAIN_LOG_H
#include <stddef.h>
void log_init(const char *dir, int console, int debug);
void log_msg(int lvl, const char *fmt, ...);   /* 0 err, 1 warn, 2 info, 3 debug */
/* Last log lines kept in memory for the GUI. Returns 1 and copies the next unread line, 0 if none. */
int log_ring_next(unsigned long *cursor, char *out, size_t outsz);
#define LOGE(...) log_msg(0, __VA_ARGS__)
#define LOGW(...) log_msg(1, __VA_ARGS__)
#define LOGI(...) log_msg(2, __VA_ARGS__)
#define LOGD(...) log_msg(3, __VA_ARGS__)
#endif
