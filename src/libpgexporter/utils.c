/*
 * Copyright (C) 2023 Red Hat
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgexporter */
#include <pgexporter.h>
#include <logging.h>
#include <utils.h>

/* system */
#include <dirent.h>
#include <errno.h>
#include <ev.h>
#include <execinfo.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <openssl/pem.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#ifndef EVBACKEND_LINUXAIO
#define EVBACKEND_LINUXAIO 0x00000040U
#endif

#ifndef EVBACKEND_IOURING
#define EVBACKEND_IOURING  0x00000080U
#endif

extern char** environ;
static bool env_changed = false;
static int max_process_title_size = 0;

static int string_compare(const void* a, const void* b);

static bool is_wal_file(char* file);

int32_t
pgexporter_get_request(struct message* msg)
{
   if (msg == NULL || msg->data == NULL || msg->length < 8)
   {
      return -1;
   }

   return pgexporter_read_int32(msg->data + 4);
}

int
pgexporter_extract_username_database(struct message* msg, char** username, char** database, char** appname)
{
   int start, end;
   int counter = 0;
   signed char c;
   char** array = NULL;
   size_t size;
   char* un = NULL;
   char* db = NULL;
   char* an = NULL;

   *username = NULL;
   *database = NULL;
   *appname = NULL;

   /* We know where the parameters start, and we know that the message is zero terminated */
   for (int i = 8; i < msg->length - 1; i++)
   {
      c = pgexporter_read_byte(msg->data + i);
      if (c == 0)
      {
         counter++;
      }
   }

   array = (char**)malloc(sizeof(char*) * counter);

   counter = 0;
   start = 8;
   end = 8;

   for (int i = 8; i < msg->length - 1; i++)
   {
      c = pgexporter_read_byte(msg->data + i);
      end++;
      if (c == 0)
      {
         array[counter] = (char*)malloc(end - start);
         memset(array[counter], 0, end - start);
         memcpy(array[counter], msg->data + start, end - start);

         start = end;
         counter++;
      }
   }

   for (int i = 0; i < counter; i++)
   {
      if (!strcmp(array[i], "user"))
      {
         size = strlen(array[i + 1]) + 1;
         un = malloc(size);
         memset(un, 0, size);
         memcpy(un, array[i + 1], size);

         *username = un;
      }
      else if (!strcmp(array[i], "database"))
      {
         size = strlen(array[i + 1]) + 1;
         db = malloc(size);
         memset(db, 0, size);
         memcpy(db, array[i + 1], size);

         *database = db;
      }
      else if (!strcmp(array[i], "application_name"))
      {
         size = strlen(array[i + 1]) + 1;
         an = malloc(size);
         memset(an, 0, size);
         memcpy(an, array[i + 1], size);

         *appname = an;
      }
   }

   if (*database == NULL)
   {
      *database = *username;
   }

   pgexporter_log_trace("Username: %s", *username);
   pgexporter_log_trace("Database: %s", *database);

   for (int i = 0; i < counter; i++)
   {
      free(array[i]);
   }
   free(array);

   return 0;
}

int
pgexporter_extract_message(char type, struct message* msg, struct message** extracted)
{
   int offset;
   int m_length;
   void* data = NULL;
   struct message* result = NULL;

   offset = 0;
   *extracted = NULL;

   while (result == NULL && offset < msg->length)
   {
      char t = (char)pgexporter_read_byte(msg->data + offset);

      if (type == t)
      {
         m_length = pgexporter_read_int32(msg->data + offset + 1);

         result = (struct message*)malloc(sizeof(struct message));
         data = (void*)malloc(1 + m_length);

         memcpy(data, msg->data + offset, 1 + m_length);

         result->kind = pgexporter_read_byte(data);
         result->length = 1 + m_length;
         result->max_length = 1 + m_length;
         result->data = data;

         *extracted = result;

         return 0;
      }
      else
      {
         offset += 1;
         offset += pgexporter_read_int32(msg->data + offset);
      }
   }

   return 1;
}

bool
pgexporter_has_message(char type, void* data, size_t data_size)
{
   int offset;

   offset = 0;

   while (offset < data_size)
   {
      char t = (char)pgexporter_read_byte(data + offset);

      if (type == t)
      {
         return true;
      }
      else
      {
         offset += 1;
         offset += pgexporter_read_int32(data + offset);
      }
   }

   return false;
}

size_t
pgexporter_extract_message_offset(size_t offset, void* data, struct message** extracted)
{
   char type;
   int m_length;
   void* m_data;
   struct message* result = NULL;

   *extracted = NULL;

   type = (char)pgexporter_read_byte(data + offset);
   m_length = pgexporter_read_int32(data + offset + 1);

   result = (struct message*)malloc(sizeof(struct message));
   m_data = malloc(1 + m_length);

   memcpy(m_data, data + offset, 1 + m_length);

   result->kind = type;
   result->length = 1 + m_length;
   result->max_length = 1 + m_length;
   result->data = m_data;

   *extracted = result;

   return offset + 1 + m_length;
}

int
pgexporter_extract_message_from_data(char type, void* data, size_t data_size, struct message** extracted)
{
   int offset;
   void* m_data = NULL;
   int m_length;
   struct message* result = NULL;

   offset = 0;
   *extracted = NULL;

   while (result == NULL && offset < data_size)
   {
      char t = (char)pgexporter_read_byte(data + offset);

      if (type == t)
      {
         m_length = pgexporter_read_int32(data + offset + 1);

         result = (struct message*)malloc(sizeof(struct message));
         m_data = (void*)malloc(1 + m_length);

         memcpy(m_data, data + offset, 1 + m_length);

         result->kind = pgexporter_read_byte(m_data);
         result->length = 1 + m_length;
         result->max_length = 1 + m_length;
         result->data = m_data;

         *extracted = result;

         return 0;
      }
      else
      {
         offset += 1;
         offset += pgexporter_read_int32(data + offset);
      }
   }

   return 1;
}

signed char
pgexporter_read_byte(void* data)
{
   return (signed char) *((char*)data);
}

int16_t
pgexporter_read_int16(void* data)
{
   unsigned char bytes[] = {*((unsigned char*)data),
                            *((unsigned char*)(data + 1))};

   int16_t res = (int16_t)((bytes[0] << 8)) |
                 ((bytes[1]));

   return res;
}

int32_t
pgexporter_read_int32(void* data)
{
   unsigned char bytes[] = {*((unsigned char*)data),
                            *((unsigned char*)(data + 1)),
                            *((unsigned char*)(data + 2)),
                            *((unsigned char*)(data + 3))};

   int32_t res = (int32_t)((bytes[0] << 24)) |
                 ((bytes[1] << 16)) |
                 ((bytes[2] << 8)) |
                 ((bytes[3]));

   return res;
}

int64_t
pgexporter_read_int64(void* data)
{
   int64_t i0 = *((unsigned char*)data);
   int64_t i1 = *((unsigned char*)data + 1);
   int64_t i2 = *((unsigned char*)data + 2);
   int64_t i3 = *((unsigned char*)data + 3);
   int64_t i4 = *((unsigned char*)data + 4);
   int64_t i5 = *((unsigned char*)data + 5);
   int64_t i6 = *((unsigned char*)data + 6);
   int64_t i7 = *((unsigned char*)data + 7);

   i0 = i0 << 56;
   i1 = i1 << 48;
   i2 = i2 << 40;
   i3 = i3 << 32;
   i4 = i4 << 24;
   i5 = i5 << 16;
   i6 = i6 << 8;
   /* i7 = i7; */

   return i0 | i1 | i2 | i3 | i4 | i5 | i6 | i7;
}

void
pgexporter_write_byte(void* data, signed char b)
{
   *((char*)(data)) = b;
}

void
pgexporter_write_int32(void* data, int32_t i)
{
   char* ptr = (char*)&i;

   *((char*)(data + 3)) = *ptr;
   ptr++;
   *((char*)(data + 2)) = *ptr;
   ptr++;
   *((char*)(data + 1)) = *ptr;
   ptr++;
   *((char*)(data)) = *ptr;
}

void
pgexporter_write_int64(void* data, int64_t i)
{
   char* ptr = (char*)&i;

   *((char*)(data + 7)) = *ptr;
   ptr++;
   *((char*)(data + 6)) = *ptr;
   ptr++;
   *((char*)(data + 5)) = *ptr;
   ptr++;
   *((char*)(data + 4)) = *ptr;
   ptr++;
   *((char*)(data + 3)) = *ptr;
   ptr++;
   *((char*)(data + 2)) = *ptr;
   ptr++;
   *((char*)(data + 1)) = *ptr;
   ptr++;
   *((char*)(data)) = *ptr;
}

char*
pgexporter_read_string(void* data)
{
   return (char*)data;
}

void
pgexporter_write_string(void* data, char* s)
{
   memcpy(data, s, strlen(s));
}

bool
pgexporter_bigendian(void)
{
   short int word = 0x0001;
   char* b = (char*)&word;
   return (b[0] ? false : true);
}

unsigned int
pgexporter_swap(unsigned int i)
{
   return ((i << 24) & 0xff000000) |
          ((i << 8) & 0x00ff0000) |
          ((i >> 8) & 0x0000ff00) |
          ((i >> 24) & 0x000000ff);
}

void
pgexporter_libev_engines(void)
{
   unsigned int engines = ev_supported_backends();

   if (engines & EVBACKEND_SELECT)
   {
      pgexporter_log_debug("libev available: select");
   }
   if (engines & EVBACKEND_POLL)
   {
      pgexporter_log_debug("libev available: poll");
   }
   if (engines & EVBACKEND_EPOLL)
   {
      pgexporter_log_debug("libev available: epoll");
   }
   if (engines & EVBACKEND_LINUXAIO)
   {
      pgexporter_log_debug("libev available: linuxaio");
   }
   if (engines & EVBACKEND_IOURING)
   {
      pgexporter_log_debug("libev available: iouring");
   }
   if (engines & EVBACKEND_KQUEUE)
   {
      pgexporter_log_debug("libev available: kqueue");
   }
   if (engines & EVBACKEND_DEVPOLL)
   {
      pgexporter_log_debug("libev available: devpoll");
   }
   if (engines & EVBACKEND_PORT)
   {
      pgexporter_log_debug("libev available: port");
   }
}

unsigned int
pgexporter_libev(char* engine)
{
   unsigned int engines = ev_supported_backends();

   if (engine)
   {
      if (!strcmp("select", engine))
      {
         if (engines & EVBACKEND_SELECT)
         {
            return EVBACKEND_SELECT;
         }
         else
         {
            pgexporter_log_warn("libev not available: select");
         }
      }
      else if (!strcmp("poll", engine))
      {
         if (engines & EVBACKEND_POLL)
         {
            return EVBACKEND_POLL;
         }
         else
         {
            pgexporter_log_warn("libev not available: poll");
         }
      }
      else if (!strcmp("epoll", engine))
      {
         if (engines & EVBACKEND_EPOLL)
         {
            return EVBACKEND_EPOLL;
         }
         else
         {
            pgexporter_log_warn("libev not available: epoll");
         }
      }
      else if (!strcmp("linuxaio", engine))
      {
         return EVFLAG_AUTO;
      }
      else if (!strcmp("iouring", engine))
      {
         if (engines & EVBACKEND_IOURING)
         {
            return EVBACKEND_IOURING;
         }
         else
         {
            pgexporter_log_warn("libev not available: iouring");
         }
      }
      else if (!strcmp("devpoll", engine))
      {
         if (engines & EVBACKEND_DEVPOLL)
         {
            return EVBACKEND_DEVPOLL;
         }
         else
         {
            pgexporter_log_warn("libev not available: devpoll");
         }
      }
      else if (!strcmp("port", engine))
      {
         if (engines & EVBACKEND_PORT)
         {
            return EVBACKEND_PORT;
         }
         else
         {
            pgexporter_log_warn("libev not available: port");
         }
      }
      else if (!strcmp("auto", engine) || !strcmp("", engine))
      {
         return EVFLAG_AUTO;
      }
      else
      {
         pgexporter_log_warn("libev unknown option: %s", engine);
      }
   }

   return EVFLAG_AUTO;
}

char*
pgexporter_libev_engine(unsigned int val)
{
   switch (val)
   {
      case EVBACKEND_SELECT:
         return "select";
      case EVBACKEND_POLL:
         return "poll";
      case EVBACKEND_EPOLL:
         return "epoll";
      case EVBACKEND_LINUXAIO:
         return "linuxaio";
      case EVBACKEND_IOURING:
         return "iouring";
      case EVBACKEND_KQUEUE:
         return "kqueue";
      case EVBACKEND_DEVPOLL:
         return "devpoll";
      case EVBACKEND_PORT:
         return "port";
   }

   return "Unknown";
}

char*
pgexporter_get_home_directory(void)
{
   struct passwd* pw = getpwuid(getuid());

   if (pw == NULL)
   {
      return NULL;
   }

   return pw->pw_dir;
}

char*
pgexporter_get_user_name(void)
{
   struct passwd* pw = getpwuid(getuid());

   if (pw == NULL)
   {
      return NULL;
   }

   return pw->pw_name;
}

char*
pgexporter_get_password(void)
{
   char p[MAX_PASSWORD_LENGTH];
   struct termios oldt, newt;
   int i = 0;
   int c;
   char* result = NULL;

   memset(&p, 0, sizeof(p));

   tcgetattr(STDIN_FILENO, &oldt);
   newt = oldt;

   newt.c_lflag &= ~(ECHO);

   tcsetattr(STDIN_FILENO, TCSANOW, &newt);

   while ((c = getchar()) != '\n' && c != EOF && i < MAX_PASSWORD_LENGTH)
   {
      p[i++] = c;
   }
   p[i] = '\0';

   tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

   result = malloc(strlen(p) + 1);
   memset(result, 0, strlen(p) + 1);

   memcpy(result, &p, strlen(p));

   return result;
}

int
pgexporter_base64_encode(char* raw, int raw_length, char** encoded)
{
   BIO* b64_bio;
   BIO* mem_bio;
   BUF_MEM* mem_bio_mem_ptr;
   char* r = NULL;

   if (raw == NULL)
   {
      goto error;
   }

   b64_bio = BIO_new(BIO_f_base64());
   mem_bio = BIO_new(BIO_s_mem());

   BIO_push(b64_bio, mem_bio);
   BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
   BIO_write(b64_bio, raw, raw_length);
   BIO_flush(b64_bio);

   BIO_get_mem_ptr(mem_bio, &mem_bio_mem_ptr);

   BIO_set_close(mem_bio, BIO_NOCLOSE);
   BIO_free_all(b64_bio);

   BUF_MEM_grow(mem_bio_mem_ptr, (*mem_bio_mem_ptr).length + 1);
   (*mem_bio_mem_ptr).data[(*mem_bio_mem_ptr).length] = '\0';

   r = malloc(strlen((*mem_bio_mem_ptr).data) + 1);
   memset(r, 0, strlen((*mem_bio_mem_ptr).data) + 1);
   memcpy(r, (*mem_bio_mem_ptr).data, strlen((*mem_bio_mem_ptr).data));

   BUF_MEM_free(mem_bio_mem_ptr);

   *encoded = r;

   return 0;

error:

   *encoded = NULL;

   return 1;
}

int
pgexporter_base64_decode(char* encoded, size_t encoded_length, char** raw, int* raw_length)
{
   BIO* b64_bio;
   BIO* mem_bio;
   size_t size;
   char* decoded;
   int index;

   if (encoded == NULL)
   {
      goto error;
   }

   size = (encoded_length * 3) / 4 + 1;
   decoded = malloc(size);
   memset(decoded, 0, size);

   b64_bio = BIO_new(BIO_f_base64());
   mem_bio = BIO_new(BIO_s_mem());

   BIO_write(mem_bio, encoded, encoded_length);
   BIO_push(b64_bio, mem_bio);
   BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);

   index = 0;
   while (0 < BIO_read(b64_bio, decoded + index, 1))
   {
      index++;
   }

   BIO_free_all(b64_bio);

   *raw = decoded;
   *raw_length = index;

   return 0;

error:

   *raw = NULL;
   *raw_length = 0;

   return 1;
}

void
pgexporter_set_proc_title(int argc, char** argv, char* s1, char* s2)
{
#ifdef HAVE_LINUX
   char title[MAX_PROCESS_TITLE_LENGTH];
   size_t size;
   char** env = environ;
   int es = 0;
   struct configuration* config;

   config = (struct configuration*)shmem;

   // sanity check: if the user does not want to
   // update the process title, do nothing
   if (config->update_process_title == UPDATE_PROCESS_TITLE_NEVER)
   {
      return;
   }

   if (!env_changed)
   {
      for (int i = 0; env[i] != NULL; i++)
      {
         es++;
      }

      environ = (char**)malloc(sizeof(char*) * (es + 1));
      if (environ == NULL)
      {
         return;
      }

      for (int i = 0; env[i] != NULL; i++)
      {
         size = strlen(env[i]);
         environ[i] = (char*)malloc(size + 1);

         if (environ[i] == NULL)
         {
            return;
         }

         memset(environ[i], 0, size + 1);
         memcpy(environ[i], env[i], size);
      }
      environ[es] = NULL;
      env_changed = true;
   }

   // compute how long was the command line
   // when the application was started
   if (max_process_title_size == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         max_process_title_size += strlen(argv[i]) + 1;
      }
   }

   // compose the new title
   memset(&title, 0, sizeof(title));
   snprintf(title, sizeof(title) - 1, "pgexporter: %s%s%s",
            s1 != NULL ? s1 : "",
            s1 != NULL && s2 != NULL ? "/" : "",
            s2 != NULL ? s2 : "");

   // nuke the command line info
   memset(*argv, 0, max_process_title_size);

   // copy the new title over argv checking
   // the update_process_title policy
   if (config->update_process_title == UPDATE_PROCESS_TITLE_STRICT)
   {
      size = max_process_title_size;
   }
   else
   {
      // here we can set the title to a full description
      size = strlen(title) + 1;
   }

   memcpy(*argv, title, size);
   memset(*argv + size, 0, 1);

   // keep track of how long the title is now
   max_process_title_size = size;

#else
   setproctitle("-pgexporter: %s%s%s",
                s1 != NULL ? s1 : "",
                s1 != NULL && s2 != NULL ? "/" : "",
                s2 != NULL ? s2 : "");

#endif
}

int
pgexporter_mkdir(char* dir)
{
   char* p;

   for (p = dir + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';

         if (mkdir(dir, S_IRWXU) != 0)
         {
            if (errno != EEXIST)
            {
               return 1;
            }

            errno = 0;
         }

         *p = '/';
      }
   }

   if (mkdir(dir, S_IRWXU) != 0)
   {
      if (errno != EEXIST)
      {
         return 1;
      }

      errno = 0;
   }

   return 0;
}

char*
pgexporter_vappend(char* orig, unsigned int n_str, ...)
{
   size_t orig_len, fin_len;
   va_list args;
   char* str = NULL;
   char** strings = NULL;
   char* ptr = NULL;

   if (orig)
   {
      fin_len = orig_len = strlen(orig);
   }
   else
   {
      fin_len = orig_len = 0;
   }

   strings = (char**) malloc(n_str * sizeof(char*));

   va_start(args, n_str);

   for (unsigned int i = 0; i < n_str; i++)
   {
      strings[i] = va_arg(args, char*);
      fin_len += strlen(strings[i]);
   }

   str = (char*) realloc(orig, fin_len + 1);
   ptr = str + orig_len;

   for (unsigned int i = 0; i < n_str; i++)
   {
      int j = 0;
      while (strings[i][j])
         *ptr++ = strings[i][j++];
   }

   *ptr = 0;

   va_end(args);

   free(strings);

   return str;
}

char*
pgexporter_append(char* orig, char* s)
{
   return pgexporter_vappend(orig, 1, s);
}

char*
pgexporter_append_int(char* orig, int i)
{
   char number[12];

   memset(&number[0], 0, sizeof(number));
   snprintf(&number[0], 11, "%d", i);
   orig = pgexporter_append(orig, number);

   return orig;
}

char*
pgexporter_append_ulong(char* orig, unsigned long l)
{
   char number[21];

   memset(&number[0], 0, sizeof(number));
   snprintf(&number[0], 20, "%lu", l);
   orig = pgexporter_append(orig, number);

   return orig;
}

char*
pgexporter_append_bool(char* orig, bool b)
{
   if (b)
   {
      orig = pgexporter_append(orig, "1");
   }
   else
   {
      orig = pgexporter_append(orig, "0");
   }

   return orig;
}

unsigned long
pgexporter_directory_size(char* directory)
{
   unsigned long total_size = 0;
   DIR* dir;
   struct dirent* entry;
   char* p = NULL;
   struct stat st;
   unsigned long l;

   if (!(dir = opendir(directory)))
   {
      return total_size;
   }

   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_type == DT_DIR)
      {
         char path[1024];

         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         {
            continue;
         }

         snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

         total_size += pgexporter_directory_size(path);
      }
      else if (entry->d_type == DT_REG)
      {
         p = NULL;
         p = pgexporter_vappend(p, 3,
                                directory,
                                "/",
                                entry->d_name
                                );

         memset(&st, 0, sizeof(struct stat));

         stat(p, &st);

         l = st.st_size / st.st_blksize;

         if (st.st_size % st.st_blksize != 0)
         {
            l += 1;
         }

         total_size += (l * st.st_blksize);

         free(p);
      }
      else if (entry->d_type == DT_LNK)
      {
         p = NULL;
         p = pgexporter_vappend(p, 3,
                                directory,
                                "/",
                                entry->d_name
                                );

         memset(&st, 0, sizeof(struct stat));

         stat(p, &st);

         total_size += st.st_blksize;

         free(p);
      }
   }

   closedir(dir);

   return total_size;
}

int
pgexporter_get_directories(char* base, int* number_of_directories, char*** dirs)
{
   char* d = NULL;
   char** array = NULL;
   int nod = 0;
   int n;
   DIR* dir;
   struct dirent* entry;

   *number_of_directories = 0;
   *dirs = NULL;

   nod = 0;

   if (!(dir = opendir(base)))
   {
      goto error;
   }

   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_type == DT_DIR)
      {
         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         {
            continue;
         }

         nod++;
      }
   }

   closedir(dir);
   dir = NULL;

   dir = opendir(base);

   array = (char**)malloc(sizeof(char*) * nod);
   n = 0;

   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_type == DT_DIR)
      {
         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         {
            continue;
         }

         array[n] = (char*)malloc(strlen(entry->d_name) + 1);
         memset(array[n], 0, strlen(entry->d_name) + 1);
         memcpy(array[n], entry->d_name, strlen(entry->d_name));
         n++;
      }
   }

   closedir(dir);
   dir = NULL;

   pgexporter_sort(nod, array);

   free(d);
   d = NULL;

   *number_of_directories = nod;
   *dirs = array;

   return 0;

error:

   if (dir != NULL)
   {
      closedir(dir);
   }

   for (int i = 0; i < nod; i++)
   {
      free(array[i]);
   }
   free(array);

   free(d);

   *number_of_directories = 0;
   *dirs = NULL;

   return 1;
}

int
pgexporter_delete_directory(char* path)
{
   DIR* d = opendir(path);
   size_t path_len = strlen(path);
   int r = -1;
   int r2 = -1;
   char* buf;
   size_t len;
   struct dirent* entry;

   if (d)
   {
      r = 0;
      while (!r && (entry = readdir(d)))
      {
         if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         {
            continue;
         }

         len = path_len + strlen(entry->d_name) + 2;
         buf = malloc(len);

         if (buf)
         {
            struct stat statbuf;

            snprintf(buf, len, "%s/%s", path, entry->d_name);
            if (!stat(buf, &statbuf))
            {
               if (S_ISDIR(statbuf.st_mode))
               {
                  r2 = pgexporter_delete_directory(buf);
               }
               else
               {
                  r2 = unlink(buf);
               }
            }
            free(buf);
         }
         r = r2;
      }
      closedir(d);
   }

   if (!r)
   {
      r = rmdir(path);
   }

   return r;
}

int
pgexporter_get_files(char* base, int* number_of_files, char*** files)
{
   char* d = NULL;
   char** array = NULL;
   int nof = 0;
   int n;
   DIR* dir;
   struct dirent* entry;

   *number_of_files = 0;
   *files = NULL;

   nof = 0;

   if (!(dir = opendir(base)))
   {
      goto error;
   }

   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_type == DT_REG)
      {
         nof++;
      }
   }

   closedir(dir);
   dir = NULL;

   dir = opendir(base);

   array = (char**)malloc(sizeof(char*) * nof);
   n = 0;

   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_type == DT_REG)
      {
         array[n] = (char*)malloc(strlen(entry->d_name) + 1);
         memset(array[n], 0, strlen(entry->d_name) + 1);
         memcpy(array[n], entry->d_name, strlen(entry->d_name));
         n++;
      }
   }

   closedir(dir);
   dir = NULL;

   pgexporter_sort(nof, array);

   free(d);
   d = NULL;

   *number_of_files = nof;
   *files = array;

   return 0;

error:

   if (dir != NULL)
   {
      closedir(dir);
   }

   for (int i = 0; i < nof; i++)
   {
      free(array[i]);
   }
   free(array);

   free(d);

   *number_of_files = 0;
   *files = NULL;

   return 1;
}

int
pgexporter_delete_file(char* file)
{
   int ret;

   ret = unlink(file);

   if (ret != 0)
   {
      pgexporter_log_warn("pgexporter_delete_file: %s (%s)", file, strerror(errno));
      errno = 0;
      ret = 1;
   }

   return ret;
}

int
pgexporter_copy_directory(char* from, char* to)
{
   DIR* d = opendir(from);
   char* from_buffer;
   char* to_buffer;
   size_t from_length;
   size_t to_length;
   struct dirent* entry;
   struct stat statbuf;

   pgexporter_mkdir(to);

   if (d)
   {
      while ((entry = readdir(d)))
      {
         if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         {
            continue;
         }

         from_length = strlen(from) + strlen(entry->d_name) + 2;
         from_buffer = malloc(from_length);

         to_length = strlen(to) + strlen(entry->d_name) + 2;
         to_buffer = malloc(to_length);

         snprintf(from_buffer, from_length, "%s/%s", from, entry->d_name);
         snprintf(to_buffer, to_length, "%s/%s", to, entry->d_name);

         if (!stat(from_buffer, &statbuf))
         {
            if (S_ISDIR(statbuf.st_mode))
            {
               pgexporter_copy_directory(from_buffer, to_buffer);
            }
            else
            {
               pgexporter_copy_file(from_buffer, to_buffer);
            }
         }

         free(from_buffer);
         free(to_buffer);
      }
      closedir(d);
   }

   return 0;
}

int
pgexporter_copy_file(char* from, char* to)
{
   int fd_from = -1;
   int fd_to = -1;
   char buffer[8192];
   ssize_t nread = -1;
   int saved_errno = -1;

   fd_from = open(from, O_RDONLY);
   if (fd_from < 0)
   {
      goto error;
   }

   /* TODO: Permissions */
   fd_to = open(to, O_WRONLY | O_CREAT | O_EXCL, 0664);
   if (fd_to < 0)
   {
      goto error;
   }

   while ((nread = read(fd_from, buffer, sizeof(buffer))) > 0)
   {
      char* out = &buffer[0];
      ssize_t nwritten;

      do
      {
         nwritten = write(fd_to, out, nread);

         if (nwritten >= 0)
         {
            nread -= nwritten;
            out += nwritten;
         }
         else if (errno != EINTR)
         {
            goto error;
         }
      }
      while (nread > 0);
   }

   if (nread == 0)
   {
      if (close(fd_to) < 0)
      {
         fd_to = -1;
         goto error;
      }
      close(fd_from);
   }

   return 0;

error:
   saved_errno = errno;

   close(fd_from);
   if (fd_to >= 0)
   {
      close(fd_to);
   }

   errno = saved_errno;
   return 1;
}

int
pgexporter_move_file(char* from, char* to)
{
   int ret;

   ret = rename(from, to);
   if (ret != 0)
   {
      pgexporter_log_warn("pgexporter_move_file: %s -> %s (%s)", from, to, strerror(errno));
      errno = 0;
      ret = 1;
   }

   return ret;
}

int
pgexporter_basename_file(char* s, char** basename)
{
   size_t size;
   char* ext = NULL;
   char* r = NULL;

   *basename = NULL;

   ext = strrchr(s, '.');
   if (ext != NULL)
   {
      size = ext - s + 1;

      r = (char*)malloc(size);
      if (r == NULL)
      {
         goto error;
      }

      memset(r, 0, size);
      memcpy(r, s, size - 1);
   }
   else
   {
      size = strlen(s) + 1;

      r = (char*)malloc(size);
      if (r == NULL)
      {
         goto error;
      }

      memset(r, 0, size);
      memcpy(r, s, strlen(s));
   }

   *basename = r;

   return 0;

error:

   return 1;
}

bool
pgexporter_exists(char* f)
{
   if (access(f, F_OK) == 0)
   {
      return true;
   }

   return false;
}

bool
pgexporter_is_file(char* file)
{
   struct stat statbuf;

   memset(&statbuf, 0, sizeof(struct stat));

   if (!lstat(file, &statbuf))
   {
      if (S_ISREG(statbuf.st_mode))
      {
         return true;
      }
   }

   return false;
}

bool
pgexporter_is_directory(char* file)
{
   struct stat statbuf;
   memset(&statbuf, 0, sizeof(struct stat));

   if (!lstat(file, &statbuf))
   {
      if (S_ISDIR(statbuf.st_mode))
      {
         return true;
      }
   }
   return false;
}

bool
pgexporter_compare_files(char* f1, char* f2)
{
   FILE* fp1 = NULL;
   FILE* fp2 = NULL;
   struct stat statbuf1 = {0};
   struct stat statbuf2 = {0};
   size_t fs1;
   size_t fs2;
   char buf1[8192];
   char buf2[8192];
   size_t cs;
   size_t bs;

   fp1 = fopen(f1, "r");

   if (fp1 == NULL)
   {
      goto error;
   }

   fp2 = fopen(f2, "r");

   if (fp2 == NULL)
   {
      goto error;
   }

   memset(&statbuf1, 0, sizeof(struct stat));
   memset(&statbuf2, 0, sizeof(struct stat));

   if (stat(f1, &statbuf1) != 0)
   {
      errno = 0;
      goto error;
   }

   if (stat(f2, &statbuf2) != 0)
   {
      errno = 0;
      goto error;
   }

   if (statbuf1.st_size != statbuf2.st_size)
   {
      goto error;
   }

   cs = sizeof(char);
   bs = sizeof(buf1);

   while (!feof(fp1))
   {
      fs1 = fread(&buf1[0], cs, bs, fp1);
      fs2 = fread(&buf2[0], cs, bs, fp2);

      if (fs1 != fs2)
      {
         goto error;
      }

      if (memcmp(&buf1[0], &buf2[0], fs1) != 0)
      {
         goto error;
      }
   }

   fclose(fp1);
   fclose(fp2);

   return true;

error:

   if (fp1 != NULL)
   {
      fclose(fp1);
   }

   if (fp2 != NULL)
   {
      fclose(fp2);
   }

   return false;
}

int
pgexporter_symlink_file(char* from, char* to)
{
   int ret;

   ret = symlink(to, from);

   if (ret != 0)
   {
      pgexporter_log_warn("pgexporter_symlink_file: %s -> %s (%s)", from, to, strerror(errno));
      errno = 0;
      ret = 1;
   }

   return ret;
}

bool
pgexporter_is_symlink(char* file)
{
   struct stat statbuf;

   memset(&statbuf, 0, sizeof(struct stat));

   if (!lstat(file, &statbuf))
   {
      if (S_ISLNK(statbuf.st_mode))
      {
         return true;
      }
   }

   return false;
}

char*
pgexporter_get_symlink(char* symlink)
{
   ssize_t size;
   char link[1024];
   size_t alloc;
   char* result = NULL;

   memset(&link[0], 0, sizeof(link));
   size = readlink(symlink, &link[0], sizeof(link));
   link[size + 1] = '\0';

   alloc = strlen(&link[0]) + 1;
   result = malloc(alloc);
   memset(result, 0, alloc);
   memcpy(result, &link[0], strlen(&link[0]));

   return result;
}

int
pgexporter_copy_wal_files(char* from, char* to, char* start)
{
   int number_of_wal_files = 0;
   char** wal_files = NULL;
   char* basename = NULL;
   char* ff = NULL;
   char* tf = NULL;

   pgexporter_get_files(from, &number_of_wal_files, &wal_files);

   for (int i = 0; i < number_of_wal_files; i++)
   {
      pgexporter_basename_file(wal_files[i], &basename);

      if (strcmp(wal_files[i], start) >= 0)
      {
         if (pgexporter_ends_with(wal_files[i], ".partial"))
         {
            ff = pgexporter_append(ff, from);
            if (!pgexporter_ends_with(ff, "/"))
            {
               ff = pgexporter_append(ff, "/");
            }
            ff = pgexporter_append(ff, wal_files[i]);

            tf = pgexporter_append(tf, to);
            if (!pgexporter_ends_with(tf, "/"))
            {
               tf = pgexporter_append(tf, "/");
            }
            tf = pgexporter_append(tf, basename);
         }
         else
         {
            ff = pgexporter_append(ff, from);
            if (!pgexporter_ends_with(ff, "/"))
            {
               ff = pgexporter_append(ff, "/");
            }
            ff = pgexporter_append(ff, wal_files[i]);

            tf = pgexporter_append(tf, to);
            if (!pgexporter_ends_with(tf, "/"))
            {
               tf = pgexporter_append(tf, "/");
            }
            tf = pgexporter_append(tf, wal_files[i]);
         }

         pgexporter_copy_file(ff, tf);
      }

      free(basename);
      free(ff);
      free(tf);

      basename = NULL;
      ff = NULL;
      tf = NULL;
   }

   for (int i = 0; i < number_of_wal_files; i++)
   {
      free(wal_files[i]);
   }
   free(wal_files);

   return 0;
}

int
pgexporter_number_of_wal_files(char* directory, char* from, char* to)
{
   int result;
   int number_of_wal_files = 0;
   char** wal_files = NULL;
   char* basename = NULL;

   result = 0;

   pgexporter_get_files(directory, &number_of_wal_files, &wal_files);

   for (int i = 0; i < number_of_wal_files; i++)
   {
      pgexporter_basename_file(wal_files[i], &basename);

      if (strcmp(basename, from) >= 0)
      {
         if (to == NULL || strcmp(basename, to) < 0)
         {
            result++;
         }
      }

      free(basename);
      basename = NULL;
   }

   for (int i = 0; i < number_of_wal_files; i++)
   {
      free(wal_files[i]);
   }
   free(wal_files);

   return result;
}

unsigned long
pgexporter_free_space(char* path)
{
   struct statvfs buf;

   if (statvfs(path, &buf))
   {
      errno = 0;
      return 0;
   }

   return buf.f_bsize * buf.f_bavail;
}

unsigned long
pgexporter_total_space(char* path)
{
   struct statvfs buf;

   if (statvfs(path, &buf))
   {
      errno = 0;
      return 0;
   }

   return buf.f_frsize * buf.f_blocks;
}

bool
pgexporter_starts_with(char* str, char* prefix)
{
   return strncmp(prefix, str, strlen(prefix)) == 0;
}

bool
pgexporter_ends_with(char* str, char* suffix)
{
   int str_len = strlen(str);
   int suffix_len = strlen(suffix);

   return (str_len >= suffix_len) && (strcmp(str + (str_len - suffix_len), suffix) == 0);
}

void
pgexporter_sort(size_t size, char** array)
{
   qsort(array, size, sizeof(const char*), string_compare);
}

char*
pgexporter_bytes_to_string(uint64_t bytes)
{
   char* sizes[] = {"EB", "PB", "TB", "GB", "MB", "KB", "B"};
   uint64_t exbibytes = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
   uint64_t multiplier = exbibytes;
   char* result;

   result = (char*)malloc(sizeof(char) * 20);

   for (int i = 0; i < sizeof(sizes) / sizeof(*(sizes)); i++, multiplier /= 1024)
   {
      if (bytes < multiplier)
      {
         continue;
      }

      if (bytes % multiplier == 0)
      {
         sprintf(result, "%" PRIu64 " %s", bytes / multiplier, sizes[i]);
      }
      else
      {
         sprintf(result, "%.1f %s", (float)bytes / multiplier, sizes[i]);
      }

      return result;
   }

   strcpy(result, "0");
   return result;
}

int
pgexporter_read_version(char* directory, char** version)
{
   char* result = NULL;
   char* filename = NULL;
   FILE* file = NULL;
   char buf[3];

   *version = NULL;

   filename = pgexporter_vappend(filename, 2,
                                 directory,
                                 "/PG_VERSION"
                                 );

   file = fopen(filename, "r");
   if (file == NULL)
   {
      goto error;
   }

   memset(&buf[0], 0, sizeof(buf));
   if (fgets(&buf[0], sizeof(buf), file) == NULL)
   {
      goto error;
   }

   result = malloc(strlen(&buf[0]) + 1);
   memset(result, 0, strlen(&buf[0]) + 1);
   memcpy(result, &buf[0], strlen(&buf[0]));

   *version = result;

   fclose(file);

   free(filename);

   return 0;

error:

   if (file != NULL)
   {
      fclose(file);
   }

   free(filename);

   return 1;
}

int
pgexporter_read_wal(char* directory, char** wal)
{
   bool found = false;
   char* result = NULL;
   char* pgwal = NULL;
   int number_of_wal_files = 0;
   char** wal_files = NULL;

   *wal = NULL;

   pgwal = pgexporter_vappend(pgwal, 2,
                              directory,
                              "/pg_wal/"
                              );

   number_of_wal_files = 0;
   wal_files = NULL;

   pgexporter_get_files(pgwal, &number_of_wal_files, &wal_files);

   if (number_of_wal_files == 0)
   {
      goto error;
   }

   for (int i = 0; !found && i < number_of_wal_files; i++)
   {
      if (is_wal_file(wal_files[i]))
      {
         result = malloc(strlen(wal_files[i]) + 1);
         memset(result, 0, strlen(wal_files[i]) + 1);
         memcpy(result, wal_files[i], strlen(wal_files[i]));

         *wal = result;

         found = true;
      }
   }

   for (int i = 0; i < number_of_wal_files; i++)
   {
      free(wal_files[i]);
   }
   free(wal_files);

   return 0;

error:

   for (int i = 0; i < number_of_wal_files; i++)
   {
      free(wal_files[i]);
   }
   free(wal_files);

   return 1;
}

static int
string_compare(const void* a, const void* b)
{
   return strcmp(*(const char**)a, *(const char**)b);
}

static bool
is_wal_file(char* file)
{
   if (pgexporter_ends_with(file, ".history"))
   {
      return false;
   }

   if (strlen(file) != 24)
   {
      return false;
   }

   return true;
}

#ifdef DEBUG

int
pgexporter_backtrace(void)
{
#ifdef HAVE_LINUX
   void* array[100];
   size_t size;
   char** strings;

   size = backtrace(array, 100);
   strings = backtrace_symbols(array, size);

   for (size_t i = 0; i < size; i++)
   {
      printf("%s\n", strings[i]);
   }

   free(strings);
#endif

   return 0;
}

#endif
