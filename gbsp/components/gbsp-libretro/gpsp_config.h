
#ifndef GPSP_CONFIG_H
#define GPSP_CONFIG_H

#define GPSP_NAME                "gpSP"
#define GPSP_VERSION             "v1.0.0"
#define GPSP_NETPACKET_VERSION   "gpSP v1.0"

/* Default ROM buffer size in megabytes (this is a maximum value!) */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 32
#endif

/* Cache sizes and their config knobs */
#if defined(XT_IRAM_CACHE)   /* xtensa experiment: both caches in internal RAM */
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 32)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 16)
#elif defined(SMALL_TRANSLATION_CACHE)
  /* ESP32-S3 (PSRAM): a game's ROM code fills the cache in about 90 s and
     every flush is a burst of retranslation, so the ROM cache gets the most.
     The RAM cache keeps room for games that run far more code from IWRAM or
     EWRAM than Metroid Zero Mission (38 KB at most): the RAM blocks linked
     by direct branches are translated together and must fit in it at once,
     or the game cannot run */
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 2304)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 256)
  /* The app flushes the ROM cache between two frames once less than
     ROM_FLUSH_SOFT is left and the audio has the lead to ride out the
     retranslation, or regardless once less than ROM_FLUSH_URGENT is left,
     before translation reaches the end and flushes it mid-frame */
  #define ROM_FLUSH_SOFT (1024 * 256)
  #define ROM_FLUSH_URGENT (1024 * 128)
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* Should be an upperbound to the maximum number of bytes a single JIT'ed
   instruction can take. STM/LDM are tipically the biggest ones */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024 * 2)

/* Hash table size for ROM trans cache lookups */
#if defined(SMALL_TRANSLATION_CACHE)
#define ROM_BRANCH_HASH_BITS                           15   /* ~6000 blocks at most: 128 KB of PSRAM */
#else
#define ROM_BRANCH_HASH_BITS                           16
#endif
#define ROM_BRANCH_HASH_SIZE   (1 << ROM_BRANCH_HASH_BITS)

/* RFU Multiplayer config, do not mess around too much with it */
#define MAX_RFU_NETPLAYERS       32

#endif
