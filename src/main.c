#include <stdio.h>
#include "serve.h"
int main(int argc,char *argv[]){
    if(argc!=2||(strcmp(argv[1],"start")!=0&&strcmp(argv[2],"shut")!=0)){
		printf("Usage: %s start/shut \n",argv[0]);
        return 1;
    }
    if(strcmp(argv[1],"start")==0){
        serve();
    }
}