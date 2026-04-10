/*
 * fsdata.c
 *
 *  Created on: Apr 6, 2026
 *      Author: vovchik
 */

//#include "lwipopts.h"
#include "ff.h"
const struct fsdata_file *FS_ROOT=NULL;

FIL fsfile;
FILINFO finfo;
unsigned char buff[200];
UINT byte_read_ptr=0;
int fs_open_custom(struct fs_file *file, const char *name){
  int lwip_result;
  FRESULT open_result=f_open(&fsfile,name+1,FA_READ);
  file->pextension=&fsfile;
  file->len=f_size(&fsfile);
  //f_stat(name+1, &finfo);
  //file->len=finfo.fsize;
  if(open_result!=FR_OK)
    lwip_result=0;
  else{
    lwip_result=1;
//    UINT byte_read_ptr;
//    f_read(&fsfile,buff,200,&byte_read_ptr);
//    file->len=byte_read_ptr;
  }
  return lwip_result;

}
void fs_close_custom(struct fs_file *file){
  f_close(&fsfile);
  file->pextension=NULL;
}

int fs_read_custom(struct fs_file *file, char *buffer, int count){
  if(byte_read_ptr==f_size(&fsfile))
    return 0;
  FRESULT open_result=f_read(&fsfile,buffer,count,&byte_read_ptr);
  if(open_result!=FR_OK)
    return 0;
  return byte_read_ptr;
}
