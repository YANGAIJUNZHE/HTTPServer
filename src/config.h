#ifndef CONFIG_H
#define CONFIG_H
int config_load(const char *path);
int get_cfg_str(const char *key,char *s);
int get_cfg_int(const char *key,int *num);
#endif