#ifndef CONFIG_H
#define CONFIG_H

#include<stdint.h>

typedef struct{
    uint16_t port;
    char root_dir[256];
}config_t;

int config_load(const char* path,config_t *cfg);

#endif