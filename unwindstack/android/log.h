#ifndef UNWINDSTACK_ANDROID_LOG_H_
#define UNWINDSTACK_ANDROID_LOG_H_

#ifdef __ANDROID__
#include_next <android/log.h>
#else

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum android_LogPriority {
  ANDROID_LOG_UNKNOWN = 0,
  ANDROID_LOG_DEFAULT = 1,
  ANDROID_LOG_VERBOSE = 2,
  ANDROID_LOG_DEBUG = 3,
  ANDROID_LOG_INFO = 4,
  ANDROID_LOG_WARN = 5,
  ANDROID_LOG_ERROR = 6,
  ANDROID_LOG_FATAL = 7,
  ANDROID_LOG_SILENT = 8,
} android_LogPriority;

static inline int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
  (void)prio;
  if (tag != NULL && tag[0] != '\0') {
    fprintf(stderr, "%s: ", tag);
  }
  return vfprintf(stderr, fmt, ap);
}

static inline int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return ret;
}

static inline int __android_log_write(int prio, const char* tag, const char* text) {
  return __android_log_print(prio, tag, "%s", text != NULL ? text : "");
}

static inline void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  va_list ap;

  if (tag != NULL && tag[0] != '\0') {
    fprintf(stderr, "%s: ", tag);
  }
  if (fmt != NULL && fmt[0] != '\0') {
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  } else if (cond != NULL && cond[0] != '\0') {
    fprintf(stderr, "assertion failed: %s", cond);
  } else {
    fprintf(stderr, "assertion failed");
  }
  fputc('\n', stderr);
  abort();
}

static inline int __android_log_is_loggable(int prio, const char* tag, int default_prio) {
  (void)prio;
  (void)tag;
  (void)default_prio;
  return 1;
}

static inline int __android_log_is_loggable_len(int prio, const char* tag, size_t len,
                                                int default_prio) {
  (void)prio;
  (void)tag;
  (void)len;
  (void)default_prio;
  return 1;
}

#ifdef __cplusplus
}
#endif

#endif

#endif  // UNWINDSTACK_ANDROID_LOG_H_
