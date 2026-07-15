#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#define MAXLEN 1024
#define PATH "/home/lld/code_train/config.ini"

void trim(char *s){
    char *p=s;
    while(*p&&isspace((unsigned char)*p))   p++;
    if(p!=s){
        char tmp[MAXLEN];
        strcpy(tmp,p);
        strcpy(s,tmp);
    }
    char *end=s+strlen(s)-1;
    while(end>=s&&isspace((unsigned char)*end)){
        *end='\0';
        end--;
    }
}

int get_cfg_str(const char *key,char *s){
    FILE *fp=fopen(PATH,"r");
    if(!fp){
        fprintf(stderr,"Fail to open config file!\n");
        return -1;
    }
    char line[MAXLEN];
    while(fgets(line,sizeof(line),fp)){
        trim(line);
        if(!line[0]||line[0]=='#')  continue;
        char *md=strchr(line,'=');
        if(!md){
            fprintf(stderr,"Invalid configuration!\n");
            fclose(fp);
            return -1;
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
    fprintf(stderr,"Required config not found!\n");
    return -1;
}

int get_cfg_int(const char *key,int *num){
    char buf[MAXLEN];
    int res=get_cfg_str(key,buf);
    if(res!=0)  return res;
    char *p;
    errno=0;
    int n=(int)strtol(buf,&p,10);
    if(errno==ERANGE||*p!='\0'){
        fprintf(stderr,"Invalid configuration!\n");
        return -1;
    }
    *num=n;
    return 0;
}