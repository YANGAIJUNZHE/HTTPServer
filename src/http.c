#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include "config.h"
#include "file.h"

void unimplemented(int client){
    char buf[1024];
    sprintf(buf,"HTTP/1.0 501 Method Not Implemented\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,SERVER_STRING);
    send(client,buf,strlen(buf),0);
    sprintf(buf,"Content-Type: text/html\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"</TITLE></HEAD>\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"<BODY><P>HTTP request method not supported.\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"</BODY></HTML>\r\n");
    send(client,buf,strlen(buf),0);
}

void accept_request(int client){
    get_cfg_str("PATH",&path);
    char buff[1024],method[255],url[255],path[512],*queryString=NULL;
    int numchars;
    size_t i,j;
    numchars=get_line(client,buff,sizeof(buff));
    i=0;j=0;
    while(!isspace(buff[j])&&(i<sizeof(method)-1)){
        method[i]=buff[j];
        i++;j++;
    }
    method[i]='\0';
    if(strcasecmp(method,"GET")&&strcasecmp(method,"POST")){
        unimplemented(client);
        return;
    }
    i=0;
    while(isspace(buff[j])&&(j<sizeof(buff)))
        j++;
    while(!isspace(buff[j])&&(i<sizeof(url)-1)&&(j<sizeof(buff))){
        url[i]=buff[j];
        i++;
        j++;
    }
    url[i]='\0';
    sprintf(path,"%s",url);
    serve_file(client,path);
}