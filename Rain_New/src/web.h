#ifndef RAIN_WEB_H
#define RAIN_WEB_H
#include "config.h"
int web_run(const cfg_t *cfg);              /* blocks until stop is requested */
void web_thread(void *cfg_ptr);             /* thread entry for web_run */
extern const unsigned char web_index_html[];
extern const unsigned int web_index_html_len;
#endif
