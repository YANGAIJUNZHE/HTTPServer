#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define MAXLEN 1024
#define PATH "../config.ini"

void trim(char *s){
    char tmp[MAXLEN];
    char *p=s;
    while(*p&&isspace((unsigned char)*p))   p++;
    strcpy(tmp,p);
    strcpy(s,tmp);
}

int get_cfg_str(const char *key,char *s){
    FILE *fp=fopen(PATH,"r");
    if(!fp){
        printf("Fail to open config file!\n");
        exit(1);
    }
    char line[MAXLEN];
    while(fgets(line,sizeof(line),fp)){
        trim(line);
        if(!line[0]||line[0]=='#')  continue;
        char *md=strchr(line,'=');
        if(!md){
            printf("Invalid configuration!");
            exit(1);
        }
        char k[MAXLEN],v[MAXLEN];
        strncpy(k,line,md-line);
        k[md-line]='\0';
        strcpy(v,md+1);
        trim(k);
        trim(v);
        if(strcmp(k,key)==0){
            strcpy(s,v);
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    printf("Required config not found!\n");
    exit(1);
}

int get_cfg_int(const char *key,int *num){
    char buf[MAXLEN];
    int res=get_cfg_str(key,buf);
    if(res!=0)  return res;
    char *p;
    int n=(int)strtol(buf,&p,10);
    if(*p!='\0'||errno==ERANGE){
        printf("Invalid configuration!");
        exit(1);
    }
    *num=n;
    return 0;
}