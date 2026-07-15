#include"config.h"
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<ctype.h>
#include<limits.h>
#include<errno.h>

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "./www"//默认的web文档的根目录

//static规定作用域只在此文件内部
static char *trim(char *s){
    //从尾部开始去掉空格
    char *end=s+strlen(s)-1;
    while(end>=s&&isspace((unsigned char)*end)){//unsigned char保证能用isspace函数
        *end='\0';
        end--;
    }
    //从头开始去掉空格
    while(isspace((unsigned char)*s)){
        s++;
    }

    return s;
}

int config_load(const char *path,config_t *cfg){
    FILE *fp;
    char line[512];
    int lineno=0;

    cfg->port=DEFAULT_PORT;
    strncpy(cfg->root_dir,DEFAULT_ROOT,sizeof(cfg->root_dir)-1);
    cfg->root_dir[sizeof(cfg->root_dir)-1]='\0';

    fp=fopen(path,"r");
    if(fp==NULL){
        fprintf(stderr,"无法打开配置文件%s:%s,使用默认值\n",path,strerror(errno));
        return -1;
    }

    while(fgets(line,sizeof(line),fp)!=NULL){
        char *key,*value,*eq;
        ++lineno;
        key=trim(line);
        if(key[0]=='\0'||key[0]=='#'){
            continue;
        }

        eq=strchr(key,'=');
        if(eq==NULL){
            fprintf(stderr,"第%d行格式错误:%s\n",lineno,key);
            continue;
        }

        *eq='\0';
        value=trim(eq+1);
        key=trim(key);

        if(strcmp(key,"PORT")==0){
            long p=strtol(value,NULL,10);
            if(p<1024||p>65535){
                fprintf(stderr,"PORT值无效%s,使用默认值%d\n",value,DEFAULT_PORT);
                cfg->port=DEFAULT_PORT;
            }
            else{
                    cfg->port=(uint16_t)p;
                }
        }
        else if(strcmp(key,"ROOT_DIR")==0){
            char resolved[PATH_MAX];
            if(realpath(value,resolved)==NULL){
                fprintf(stderr,"ROOT_DIR无效%s:%s,使用默认%s\n",value,strerror(errno),DEFAULT_ROOT);
                strncpy(cfg->root_dir,DEFAULT_ROOT,sizeof(cfg->root_dir)-1);

            }
            else{
                strncpy(cfg->root_dir,resolved,sizeof(cfg->root_dir)-1);
                cfg->root_dir[sizeof(cfg->root_dir)-1]='\0';
            }
        }
        else{
            fprintf(stderr,"第%d行未知键:%s\n",lineno,key);
        }
    }
    fclose(fp);
    return 0;
}


