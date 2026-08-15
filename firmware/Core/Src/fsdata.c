/*
 * fsdata.c
 *
 *  Created on: Apr 6, 2026
 *      Author: vovchik
 */

#include "lwip/apps/fs.h"
#include "lwip/mem.h"
#include "ff.h"
const struct fsdata_file *FS_ROOT=NULL;

/* Per-connection state for a custom (FatFs-backed) file.
 * Allocated from the lwIP heap (mem_malloc/mem_free) in fs_open_custom
 * and stashed in struct fs_file::pextension, so concurrent/interleaved
 * connections each get their own FIL instead of stomping on a shared one. */
struct fs_custom_state {
  FIL fil;
};

int fs_open_custom(struct fs_file *file, const char *name){
  struct fs_custom_state *state=(struct fs_custom_state*)mem_malloc(sizeof(struct fs_custom_state));
  if(state==NULL)
    return 0;

  FRESULT open_result=f_open(&state->fil,name+1,FA_READ);
  if(open_result!=FR_OK){
    mem_free(state);
    return 0;
  }

  file->pextension=state;
  file->index=0;
  file->len=(int)f_size(&state->fil);
  return 1;
}
void fs_close_custom(struct fs_file *file){
  struct fs_custom_state *state=(struct fs_custom_state*)file->pextension;
  if(state==NULL)
    return;
  f_close(&state->fil);
  mem_free(state);
  file->pextension=NULL;
}

//u8_t fs_wait_read_custom(struct fs_file *file, fs_wait_cb callback_fn, void *callback_arg){
//
//}
//u8_t fs_canread_custom(struct fs_file *file){
//
//}

int fs_read_custom(struct fs_file *file, char *buffer, int count){
  struct fs_custom_state *state=(struct fs_custom_state*)file->pextension;
  UINT bytes_read;
  FRESULT read_result;
  if(state==NULL)
    return FS_READ_EOF;

  read_result=f_read(&state->fil,buffer,count,&bytes_read);
  if(read_result!=FR_OK)
    return FS_READ_EOF;
  if(bytes_read==0)
    return FS_READ_EOF;

  file->index+=bytes_read;
  return (int)bytes_read;
}
