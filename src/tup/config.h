#ifndef tup_config_h
#define tup_config_h

#include "tupid.h"

int find_tup_dir(void);
int open_tup_top(void);
tupid_t get_sub_dir_dt(void);
const char *get_tup_top(void);
int get_tup_top_len(void);
const char *get_sub_dir(void);
int get_sub_dir_len(void);
int tup_top_fd(void);
int tup_vardict_fd(void);
int tup_vardict_open(void);
void tup_vardict_close(void);
int display_output(int fd, int iserr, const char *name, int display_name);

#endif
