#ifndef ARCANUM_NET_MP_LOG_H_
#define ARCANUM_NET_MP_LOG_H_

// Log levels (compile-time cutoff via MP_LOG_LEVEL)
#define MP_LOG_ERROR  0
#define MP_LOG_WARN   1
#define MP_LOG_INFO   2
#define MP_LOG_DEBUG  3
#define MP_LOG_TRACE  4

// Default: full logging in debug builds, INFO+ in release
#ifndef MP_LOG_LEVEL
#  ifdef NDEBUG
#    define MP_LOG_LEVEL MP_LOG_INFO
#  else
#    define MP_LOG_LEVEL MP_LOG_TRACE
#  endif
#endif

// Log categories
#define MP_CAT_NET     "NET"
#define MP_CAT_PKT     "PKT"
#define MP_CAT_SYNC    "SYNC"
#define MP_CAT_ANIM    "ANIM"
#define MP_CAT_SPELL   "SPELL"
#define MP_CAT_ITEM    "ITEM"
#define MP_CAT_PARTY   "PARTY"
#define MP_CAT_TIME    "TIME"
#define MP_CAT_SESSION "SESSION"
#define MP_CAT_UI      "UI"

void mp_log_init(void);
void mp_log_shutdown(void);
void mp_log_write(int level, const char* category, const char* fmt, ...);

// Convenience macros — compile away levels above MP_LOG_LEVEL
#if MP_LOG_LEVEL >= MP_LOG_ERROR
#  define MP_ERROR(cat, ...) mp_log_write(MP_LOG_ERROR, cat, __VA_ARGS__)
#else
#  define MP_ERROR(cat, ...) ((void)0)
#endif

#if MP_LOG_LEVEL >= MP_LOG_WARN
#  define MP_WARN(cat, ...)  mp_log_write(MP_LOG_WARN,  cat, __VA_ARGS__)
#else
#  define MP_WARN(cat, ...)  ((void)0)
#endif

#if MP_LOG_LEVEL >= MP_LOG_INFO
#  define MP_INFO(cat, ...)  mp_log_write(MP_LOG_INFO,  cat, __VA_ARGS__)
#else
#  define MP_INFO(cat, ...)  ((void)0)
#endif

#if MP_LOG_LEVEL >= MP_LOG_DEBUG
#  define MP_DEBUG(cat, ...) mp_log_write(MP_LOG_DEBUG, cat, __VA_ARGS__)
#else
#  define MP_DEBUG(cat, ...) ((void)0)
#endif

#if MP_LOG_LEVEL >= MP_LOG_TRACE
#  define MP_TRACE(cat, ...) mp_log_write(MP_LOG_TRACE, cat, __VA_ARGS__)
#else
#  define MP_TRACE(cat, ...) ((void)0)
#endif

// Packet direction helpers
#define MP_LOG_SEND(pkt_type, size) \
    MP_TRACE(MP_CAT_PKT, "SEND type=%-3d size=%d", (pkt_type), (size))
#define MP_LOG_RECV(pkt_type, size) \
    MP_TRACE(MP_CAT_PKT, "RECV type=%-3d size=%d", (pkt_type), (size))

#endif /* ARCANUM_NET_MP_LOG_H_ */
