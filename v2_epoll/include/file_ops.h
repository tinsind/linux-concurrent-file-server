#ifndef __FILE_OPS_H__
#define __FILE_OPS_H__

int handle_list(int cfd);
int handle_get(int cfd, const char *filename);
int handle_put(int cfd, const char *filename);


#endif // __FILE_OPS_H__
