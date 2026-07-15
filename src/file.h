#ifndef FILE_H
#define FILE_H
void headers(int client,const char *filename);
void not_found(int client);
void cat(int client,FILE *resource);
void serve_file(int client,const char*filename);
#endif