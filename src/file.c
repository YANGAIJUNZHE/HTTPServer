#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

void headers(int client,const char *filename){
    char buf[1024];
    (void) filename;
    strcpy(buf,"HTTP/1.0 200 OK\r\n");
    send(client,buf,strlen(buf),0);
    strcpy(buf,"from: strange space");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"Content-Type: text/html\r\n");
    send(client,buf,strlen(buf),0);
    strcpy(buf,"\r\n");
    send(client,buf,strlen(buf),0);
}

void not_found(int client){
    char buf[1024];
    sprintf(buf,"HTTP/1.0 404 NOT FOUND\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"from: strange space");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"Content-Type: text/html\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"<BODY><P>The server could not fulfill\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"your request because the resource specified\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"is unavailable or nonexistent.\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"</BODY></HTML>\r\n");
    send(client,buf,strlen(buf),0);
}

void cat(int client,FILE *resource){
    char buf[1024];
    while(fgets(buf,sizeof(buf),resource)!=NULL)
        send(client,buf,strlen(buf),0);
}

void serve_file(int client,const char*filename){
    FILE *resource=NULL;
    char buf[1024];
    int numchars=1;
    buf[0]='A';
    buf[1]='\0';
    while((numchars>0)&&strcmp("\n",buf))
        numchars=get_line(client,buf,sizeof(buf));
    resource=fopen(filename,"r");
    if(resource==NULL)
        not_found(client);
    else{
        headers(client,filename);
        cat(client,resource);
        fclose(resource);
    }
}