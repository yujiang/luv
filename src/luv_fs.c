#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "luv_core.h"
#include "luv_object.h"
#include "luv_fs.h"

/* lifted from luvit */
static int luv_string_to_flags(lua_State* L, const char* str) {
  if (strcmp(str, "r") == 0)
    return O_RDONLY;
  if (strcmp(str, "r+") == 0)
    return O_RDWR;
  if (strcmp(str, "w") == 0)
    return O_CREAT | O_TRUNC | O_WRONLY;
  if (strcmp(str, "w+") == 0)
    return O_CREAT | O_TRUNC | O_RDWR;
  if (strcmp(str, "a") == 0)
    return O_APPEND | O_CREAT | O_WRONLY;
  if (strcmp(str, "a+") == 0)
    return O_APPEND | O_CREAT | O_RDWR;
  return luaL_error(L, "Unknown file open flag: '%s'", str);
}

/* lifted from luvit */
static void luv_push_stats_table(lua_State* L, struct stat* s) {                                
  lua_newtable(L);
  lua_pushinteger(L, s->st_dev);                                                         
  lua_setfield(L, -2, "dev");
  lua_pushinteger(L, s->st_ino);                                                         
  lua_setfield(L, -2, "ino");
  lua_pushinteger(L, s->st_mode);                                                        
  lua_setfield(L, -2, "mode");
  lua_pushinteger(L, s->st_nlink);                                                       
  lua_setfield(L, -2, "nlink");
  lua_pushinteger(L, s->st_uid);                                                         
  lua_setfield(L, -2, "uid");
  lua_pushinteger(L, s->st_gid);                                                         
  lua_setfield(L, -2, "gid");
  lua_pushinteger(L, s->st_rdev);                                                        
  lua_setfield(L, -2, "rdev");
  lua_pushinteger(L, s->st_size);                                                        
  lua_setfield(L, -2, "size");                                                           
#ifdef __POSIX__
  lua_pushinteger(L, s->st_blksize);                                                     
  lua_setfield(L, -2, "blksize");
  lua_pushinteger(L, s->st_blocks);                                                      
  lua_setfield(L, -2, "blocks");                                                         
#endif
  lua_pushinteger(L, s->st_atime);                                                       
  lua_setfield(L, -2, "atime");
  lua_pushinteger(L, s->st_mtime);                                                       
  lua_setfield(L, -2, "mtime");
  lua_pushinteger(L, s->st_ctime);                                                       
  lua_setfield(L, -2, "ctime");                                                          
#ifndef _WIN32
  lua_pushboolean(L, S_ISREG(s->st_mode));                                               
  lua_setfield(L, -2, "is_file");
  lua_pushboolean(L, S_ISDIR(s->st_mode));                                               
  lua_setfield(L, -2, "is_directory");
  lua_pushboolean(L, S_ISCHR(s->st_mode));
  lua_setfield(L, -2, "is_character_device");                                            
  lua_pushboolean(L, S_ISBLK(s->st_mode));                                               
  lua_setfield(L, -2, "is_block_device");
  lua_pushboolean(L, S_ISFIFO(s->st_mode));                                              
  lua_setfield(L, -2, "is_fifo");
  lua_pushboolean(L, S_ISLNK(s->st_mode));
  lua_setfield(L, -2, "is_symbolic_link");
  lua_pushboolean(L, S_ISSOCK(s->st_mode));
  lua_setfield(L, -2, "is_socket");
#endif
}

/* the rest of this file is mostly stolen from luvit - key difference is that
** we don't run Lua callbacks, but instead suspend and resume Lua threads */

static void luv_fs_result(lua_State* L, uv_fs_t* req) {
  if (req->result == -1) {
    lua_pushnil(L);
    lua_pushinteger(L, (uv_err_code)req->errorno);
  }
  else {
    switch (req->fs_type) {
      case UV_FS_RENAME:
      case UV_FS_UNLINK:
      case UV_FS_RMDIR:
      case UV_FS_MKDIR:
      case UV_FS_FSYNC:
      case UV_FS_FTRUNCATE:
      case UV_FS_FDATASYNC:
      case UV_FS_LINK:
      case UV_FS_SYMLINK:
      case UV_FS_CHMOD:
      case UV_FS_FCHMOD:
      case UV_FS_CHOWN:
      case UV_FS_FCHOWN:
      case UV_FS_UTIME:
      case UV_FS_FUTIME:
      case UV_FS_CLOSE:
        lua_pushinteger(L, req->result);
        break;

      case UV_FS_OPEN:
        {
          luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T);
          self->h.file = req->result;
        }
        break;

      case UV_FS_READ:
        lua_pushinteger(L, req->result);
        lua_pushlstring(L, req->data, req->result);
        free(req->data);
        req->data = NULL;
        break;

      case UV_FS_WRITE:
        lua_pushinteger(L, req->result);
        break;

      case UV_FS_READLINK:
        lua_pushstring(L, (char*)req->ptr);
        break;

      case UV_FS_READDIR:
        {
          int i;
          char* namep = (char*)req->ptr;
          int   count = req->result;
          lua_newtable(L);
          for (i = 1; i <= count; i++) {
            lua_pushstring(L, namep);
            lua_rawseti(L, -2, i);
            namep += strlen(namep) + 1; /* +1 for '\0' */
          }
        }
        break;

      case UV_FS_STAT:
      case UV_FS_LSTAT:
      case UV_FS_FSTAT:
        luv_push_stats_table(L, (struct stat*)req->ptr);
        break;

      default:
        luaL_error(L, "Unhandled fs_type");
    }
  }
  uv_fs_req_cleanup(req);
}

static void luv_fs_cb(uv_fs_t* req) {
  luv_fiber_t* fiber = container_of(req, luv_fiber_t, req);
  luv_fs_result(fiber->L, req);
  luv__state_resume((luv_state_t*)fiber);
}

#define LUV_FS_CALL(sched, func, misc, ...) \
  do { \
    luv_state_t* curr = luv__sched_current(sched); \
    lua_State*   L    = curr->L; \
    uv_fs_t*     req; \
    uv_fs_cb     cb; \
    if (luv__state_is_main(curr)) { \
      /* synchronous in main */ \
      req = &sched->req.fs; \
      cb  = NULL; \
    } \
    else { \
      req = &curr->req.fs; \
      cb  = luv_fs_cb; \
    } \
    req->data = misc; \
    \
    if (uv_fs_##func(sched->loop, req, __VA_ARGS__, cb) < 0) { \
      uv_err_t err = uv_last_error(sched->loop); \
      lua_settop(L, 0); \
      lua_pushnil(L); \
      lua_pushstring(L, uv_strerror(err)); \
    } \
    if (luv__state_is_main(curr)) { \
      luv_fs_result(L, req); \
      return lua_gettop(L); \
    } \
    else { \
      luv__state_suspend(curr); \
      return lua_yield(L, lua_gettop(L)); \
    } \
  } while(0)

static int luv_fs_open(lua_State* L) {
  luv_sched_t*  sched = luaL_checkudata(L, lua_upvalueindex(1), LUV_SCHED_T);
  const char*   path  = luaL_checkstring(L, 1);
  luv_object_t* self;

  int flags = luv_string_to_flags(L, luaL_checkstring(L, 2));
  int mode  = strtoul(luaL_checkstring(L, 3), NULL, 8);

  lua_settop(L, 0);

  self = lua_newuserdata(L, sizeof(luv_object_t));
  luaL_getmetatable(L, LUV_FILE_T);
  lua_setmetatable(L, -2);
  luv__object_init(sched, self);

  self->h.file = -1; /* invalid file handle */
  LUV_FS_CALL(sched, open, NULL, path, flags, mode);
}

static int luv_fs_unlink(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, unlink, NULL, path);
}

static int luv_fs_mkdir(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  int mode = strtoul(luaL_checkstring(L, 2), NULL, 8);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, mkdir, NULL, path, mode);
}

static int luv_fs_rmdir(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, rmdir, NULL, path);
}

static int luv_fs_readdir(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, readdir, NULL, path, 0);
}

static int luv_fs_stat(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, stat, NULL, path);
}

static int luv_fs_rename(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* old_path = luaL_checkstring(L, 1);
  const char* new_path = luaL_checkstring(L, 2);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, rename, NULL, old_path, new_path);
}

static int luv_fs_sendfile(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  luv_object_t* o_file = luaL_checkudata(L, 1, LUV_FILE_T); 
  luv_object_t* i_file = luaL_checkudata(L, 2, LUV_FILE_T); 
  off_t  ofs = luaL_checkint(L, 3); 
  size_t len = luaL_checkint(L, 4); 
  lua_settop(L, 2);
  LUV_FS_CALL(sched, sendfile, NULL, o_file->h.file, i_file->h.file, ofs, len);
}

static int luv_fs_chmod(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1); 
  int mode = strtoul(luaL_checkstring(L, 2), NULL, 8); 
  lua_settop(L, 0);
  LUV_FS_CALL(sched, chmod, NULL, path, mode);
}

static int luv_fs_utime(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1); 
  double atime = luaL_checknumber(L, 2); 
  double mtime = luaL_checknumber(L, 3); 
  lua_settop(L, 0);
  LUV_FS_CALL(sched, utime, NULL, path, atime, mtime);
}

static int luv_fs_lstat(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, lstat, NULL, path);
}

static int luv_fs_link(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* src_path = luaL_checkstring(L, 1);
  const char* dst_path = luaL_checkstring(L, 2);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, link, NULL, src_path, dst_path);
}

static int luv_fs_symlink(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* src_path = luaL_checkstring(L, 1);
  const char* dst_path = luaL_checkstring(L, 2);
  int flags = luv_string_to_flags(L, luaL_checkstring(L, 3));
  lua_settop(L, 0);
  LUV_FS_CALL(sched, symlink, NULL, src_path, dst_path, flags);
}

static int luv_fs_readlink(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, readlink, NULL, path);
}

static int luv_fs_chown(lua_State* L) {
  luv_sched_t* sched = lua_touserdata(L, lua_upvalueindex(1));
  const char* path = luaL_checkstring(L, 1);
  int uid = luaL_checkint(L, 2);
  int gid = luaL_checkint(L, 3);
  lua_settop(L, 0);
  LUV_FS_CALL(sched, chown, NULL, path, uid, gid);
}


/* file instance methods */
static int luv_file_stat(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T);
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, fstat, NULL, self->h.file);
}

static int luv_file_sync(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T); 
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, fsync, NULL, self->h.file);
}

static int luv_file_datasync(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T); 
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, fdatasync, NULL, self->h.file);
}

static int luv_file_truncate(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T); 
  off_t ofs = luaL_checkint(L, 2); 
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, ftruncate, NULL, self->h.file, ofs);
}

static int luv_file_utime(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T); 
  double atime = luaL_checknumber(L, 2);
  double mtime = luaL_checknumber(L, 3);
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, futime, NULL, self->h.file, atime, mtime);
}

static int luv_file_chmod(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T); 
  int mode = strtoul(luaL_checkstring(L, 2), NULL, 8);
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, fchmod, NULL, self->h.file, mode);
}

static int luv_file_chown(lua_State* L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T); 
  int uid = luaL_checkint(L, 2);
  int gid = luaL_checkint(L, 3);
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, fchown, NULL, self->h.file, uid, gid);
}

static int luv_file_read(lua_State *L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T);

  size_t  len = luaL_optint(L, 2, LUV_BUF_SIZE);
  int64_t ofs = luaL_optint(L, 3, -1);
  void*   buf = malloc(len); /* free from ctx->req.fs_req.data in cb */

  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, read, buf, self->h.file, buf, len, ofs); 
}

static int luv_file_write(lua_State *L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T);

  size_t   len;
  void*    buf = (void*)luaL_checklstring(L, 2, &len);
  uint64_t ofs = luaL_optint(L, 3, 0);

  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, write, NULL, self->h.file, buf, len, ofs);
}

static int luv_file_close(lua_State *L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T);
  lua_settop(L, 0);
  LUV_FS_CALL(self->sched, close, NULL, self->h.file);
}

static int luv_file_free(lua_State *L) {
  luv_object_t* self = lua_touserdata(L, 1);
  if (self->data) free(self->data);
  return 0;
}

static int luv_file_tostring(lua_State *L) {
  luv_object_t* self = luaL_checkudata(L, 1, LUV_FILE_T);
  lua_pushfstring(L, "userdata<%s>: %p", LUV_FILE_T, self);
  return 1;
}

static luaL_Reg luv_fs_funcs[] = {
  {"open",      luv_fs_open},
  {"unlink",    luv_fs_unlink},
  {"mkdir",     luv_fs_mkdir},
  {"rmdir",     luv_fs_rmdir},
  {"readdir",   luv_fs_readdir},
  {"stat",      luv_fs_stat},
  {"rename",    luv_fs_rename},
  {"sendfile",  luv_fs_sendfile},
  {"chmod",     luv_fs_chmod},
  {"chown",     luv_fs_chown},
  {"utime",     luv_fs_utime},
  {"lstat",     luv_fs_lstat},
  {"link",      luv_fs_link},
  {"symlink",   luv_fs_symlink},
  {"readlink",  luv_fs_readlink},
  {NULL,        NULL}
};

static luaL_Reg luv_file_meths[] = {
  {"read",      luv_file_read},
  {"write",     luv_file_write},
  {"close",     luv_file_close},
  {"stat",      luv_file_stat},
  {"sync",      luv_file_sync},
  {"utime",     luv_file_utime},
  {"chmod",     luv_file_chmod},
  {"chown",     luv_file_chown},
  {"datasync",  luv_file_datasync},
  {"truncate",  luv_file_truncate},
  {"__gc",      luv_file_free},
  {"__tostring",luv_file_tostring},
  {NULL,        NULL}
};

LUALIB_API int luaopenL_luv_fs(lua_State *L) {
  luaL_newmetatable(L, LUV_FILE_T);
  luaL_register(L, NULL, luv_file_meths);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luv__new_namespace(L, "luv_fs");
  lua_getfield(L, LUA_REGISTRYINDEX, LUV_SCHED_O);
  luaL_checkudata(L, -1, LUV_SCHED_T);
  luaL_openlib(L, NULL, luv_fs_funcs, 1);

  /* luv.fs */
  lua_getfield(L, LUA_REGISTRYINDEX, LUV_REG_KEY);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "fs");

  lua_pop(L, 1);

  return 1;
}

