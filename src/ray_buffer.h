#ifndef _RAY_BUF_H_
#define _RAY_BUF_H_

#include "ray_common.h"

typedef struct ray_buffer_t {
  size_t   size;
  uint8_t* head;
  uint8_t* base;
} ray_buffer_t;

ray_buffer_t* rayL_buffer_new(size_t size);

void rayL_buffer_need (ray_buffer_t* buf, size_t len);
void rayL_buffer_init (ray_buffer_t* buf, uint8_t* data, size_t len);
void rayL_buffer_free (ray_buffer_t* buf);
void rayL_buffer_init_data (ray_buffer_t* buf, uint8_t* data, size_t len);

void rayL_buffer_put (ray_buffer_t* buf, uint8_t val);
void rayL_buffer_write (ray_buffer_t* buf, uint8_t* data, size_t len);
void rayL_buffer_write_uleb128 (ray_buffer_t* buf, uint32_t val);

uint8_t  rayL_buffer_get (ray_buffer_t* buf);
uint8_t  rayL_buffer_peek (ray_buffer_t* buf);
uint8_t* rayL_buffer_read (ray_buffer_t* buf, size_t len);
uint32_t rayL_buffer_read_uleb128 (ray_buffer_t* buf);

int rayL_writer(lua_State* L, const char* str, size_t len, void* buf);

#endif /* _RAY_BUF_H_ */
