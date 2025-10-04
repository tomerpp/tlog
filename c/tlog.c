/**
 * Autor by Tomer Pires.
 *
 * The logger is implemented in C and designed for minimal overhead and easy integration.
 * It utilizes an internal thread that continuously pulls log messages from a queue
 * and writes them to either the console or a file,
 * depending on the configuration provided during initialization.
 *
 * When a user thread writes a message to the logger, it performs the following operations:
 *  1. It attempts to acquire a pre-allocated log element from a memory pool in a blocking manner.
 *  2. It formats the log message using snprintf,
 *     and optionally includes a timestamp if that feature is enabled.
 *  3. It then pushes the populated log element into the message queue,
 *     again in a blocking manner, for processing by the logger thread.
 *
 * To minimize performance impact when logging is disabled or,
 * the message level is below the configured threshold,
 * the logging function starts with a simple 'if' check and exits early if no logging is required.
 *
 * The internal logging thread dequeues messages
 * and writes them according to the specified log level and output configuration.
 * The logger respects per-message log levels and handles output formatting and routing transparently.
 *
 * For ease of use and integration, the entire logger is implemented in just two files:
 * 1. A header file (.h) that exposes only the minimal API needed for logging.
 * 2. A source file (.c) that contains the full implementation,
 *    including all utility and helper functions.
 *
 * Initialization and Cleanup
 * The logger requires explicit initialization and cleanup:
 * Upon initialization, the logger sets up the internal structures, memory pools, thread, and queues.
 * To clean up, the tlog_free function must be called. This function:
 * 1. Frees all allocated resources.
 * 2. Properly shuts down the internal thread.
 * 3. Flushes any remaining log messages in the queue
 *    to ensure that no data is lost during shutdown
 *    (especially useful for final logs at the end of a program's execution).
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include "tlog.h"

#ifdef _WIN32
///┌─────────────────────────────────────────────────┐
///├                   WINDOWS                       ┤
///└─────────────────────────────────────────────────┘
#include <windows.h>
#include <io.h>
#include <direct.h>

typedef struct {
    CRITICAL_SECTION cs;
} cross_mutex_t;

#define THREAD HANDLE
#define MUTEX cross_mutex_t
#define CONDITION CONDITION_VARIABLE

#define thread_create(thr, func, arg) *(thr) = CreateThread(NULL, 0, func, arg, 0, NULL)
#define thread_join(thr) WaitForSingleObject(*(thr), INFINITE)

#define mutex_init(mtx) (InitializeCriticalSectionEx(&(mtx)->cs, 4000, 0) ? 0 : -1)
#define mutex_lock(mtx) EnterCriticalSection(&(mtx)->cs)
#define mutex_unlock(mtx) LeaveCriticalSection(&(mtx)->cs)
#define mutex_destroy(mtx) DeleteCriticalSection(&(mtx)->cs)

#define cond_init(cond) (InitializeConditionVariable(cond), 0)
#define cond_wait(cond, mtx) SleepConditionVariableCS((cond), &(mtx)->cs, INFINITE)
#define cond_signal(cond) WakeConditionVariable((cond))
#define cond_broadcast(cond) WakeAllConditionVariable((cond))
#define cond_destroy(cond) // No-op: Windows does not require destruction

#define THREAD_RETURN DWORD WINAPI
#define THREAD_ARG LPVOID
#define SLEEP_MS(ms) Sleep(ms)

#else
///┌─────────────────────────────────────────────────┐
///├                    UNIX                         ┤
///└─────────────────────────────────────────────────┘
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#define THREAD pthread_t
#define MUTEX pthread_mutex_t
#define CONDITION pthread_cond_t

#define thread_create(thr, func, arg) pthread_create((thr), NULL, (func), (arg))
#define thread_join(thr) pthread_join(*(thr), NULL)

#define mutex_init(mtx) pthread_mutex_init((mtx), NULL)
#define mutex_lock(mtx) pthread_mutex_lock((mtx))
#define mutex_unlock(mtx) pthread_mutex_unlock((mtx))
#define mutex_destroy(mtx) pthread_mutex_destroy((mtx))

#define cond_init(cond) pthread_cond_init((cond), NULL)
#define cond_wait(cond, mtx) pthread_cond_wait((cond), (mtx))
#define cond_signal(cond) pthread_cond_signal((cond))
#define cond_broadcast(cond) pthread_cond_broadcast((cond))
#define cond_destroy(cond) pthread_cond_destroy((cond))

#define THREAD_RETURN void*
#define THREAD_ARG void*
#define SLEEP_MS(ms) usleep((ms) * 1000)

#endif

///┌─────────────────────────────────────────────────┐
///├                    ENUMS                        ┤
///└─────────────────────────────────────────────────┘
typedef enum tlog_enum_log_options {
    LOG_TO_FILE    = 0,
    LOG_TO_CONSOLE = 1,
}tlog_enum_log_options;
#define NUM_OF_LOG_OPTION 2

typedef enum tlog_enum_queue_start {
    // queue and not pool (must stay 0 - queue_create).
    E_QUEUE_START_EMPTY = 0,
    // pool (must stay 1 - queue_create).
    E_QUEUE_START_FULL = 1,
}tlog_enum_queue_start;

///┌─────────────────────────────────────────────────┐
///├                  STRUCTURES                     ┤
///└─────────────────────────────────────────────────┘
// element in queue.
typedef struct tlog_struct_log_element
{
    // save the level of element for know who need to log out.
    tlog_enum_levels m_level;
    // save the time that log request
    TLOG_TIME_T m_time;
    // msg that request to log.
    char* m_data;
}tlog_struct_log_element;

// blocking queue for create queue with waiting logs, and pool for manage the memory.
typedef struct tlog_queue_blocking{
    // hold the array of elements.
    tlog_struct_log_element *ps_items;
    // set the current element in queue (push next).
    uint16_t u16_front;
    // the last element in queue (pop first)
    uint16_t u16_rear;
    // num of elements in queue.
    uint16_t u16_count;
    // the size that queue can contain.
    uint16_t u16_capacity;

    // save between push and pop (thread safe)
    MUTEX mutex_lock;
    // block for push
    CONDITION cond_not_full;
    // block for pop
    CONDITION cond_not_empty;
    // flag for stop the access to queue.
    bool b_queue_run;
    // if allocate cond and mutex
    bool b_allocated;
} tlog_queue_blocking;

// the main struct that use for manage the logger.
struct tlog_struct_main {
    // levels for log one for console and one for file.
    bool m_level[NUM_OF_LOG_OPTION][NUM_OF_LEVELS];
    // ptr of files for log stdout for console and null/file that open in init.
    FILE* m_ptr_files[NUM_OF_LOG_OPTION];
    // thread that take msg from queue, log out and return to pool.
    THREAD m_thread;
    // flag for check if log run (thread run while the flag is true).
    bool m_log_is_run;
    // queue that contain elements for logs.
    tlog_queue_blocking m_queue;
    // queue that contain buffers of elements.
    tlog_queue_blocking m_pool;
    // size of elements in pool (for allow to write only in this size).
    uint16_t m_u16_size_of_element;
    // callback function, if user want to replace the default.
    void(*m_pf_callback)(const tlog_struct_callback_param*);
    // count msgs of log, one for file and one for stdout.
    uint16_t m_counters[NUM_OF_LOG_OPTION];
    // flag for check if log need to log out counter.
    bool m_b_log_counter;
    // flag that check if colors enable in console.
    bool m_b_colors_enable;
    // enum flag for way to log time, if not need to log time, not call to syscall for time.
    tlog_enum_time_level m_enum_time;
    // all allocated data in one place.
    void *m_p_data;
};

///┌─────────────────────────────────────────────────┐
///├               STATIC_VARIABLE                   ┤
///└─────────────────────────────────────────────────┘
static struct tlog_struct_main tlog = {
    .m_level = {
        [LOG_TO_FILE]={false, false, false, false},
        [LOG_TO_CONSOLE]={false, false, false, false}
    },
    .m_ptr_files = {NULL, NULL},
    .m_log_is_run = false,
    .m_thread = 0,
    .m_u16_size_of_element = 0,
    .m_counters = {0, 0},
    .m_enum_time = LOG_TIME_NO_TIME,
    .m_p_data = NULL
};

// array that contain strings according to level.
static const char tlog_array_str_level[NUM_OF_LEVELS][4] =
{"DBG", "INF", "WRN", "ERR"};

/**
 * @param level error/debug/info/wraning
 * @return color to specific level.
*/
const char* tlog_colors(const tlog_enum_levels level) {
    switch (level) {
        // purple
        case LOG_LEVEL_DEBUG: return "\x1b[35m";
        // cyan
        case LOG_LEVEL_INFO: return "\x1b[36m";
        // yellow
        case LOG_LEVEL_WARNING: return "\x1b[33m";
        // red
        case LOG_LEVEL_ERROR: return "\x1b[31m";
        default: return "\x1b[0m";
    }
}
///┌─────────────────────────────────────────────────┐
///├              HELPERS_FUNCTIONS                  ┤
///└─────────────────────────────────────────────────┘
///┌─────────────┐
///├   Queue     ┤
///└─────────────┘
/**
 * init a blocking queue, if empty, wait to element, if full, wait for element to pop.
 *
 * @tlog_queue_blocking     struct that manage the queue.
 * @tlog_struct_log_element array of elements.
 * @uint16_t                size of array.
 * @tlog_enum_queue_start   if start full or empty (elements has allocated size in buffer).
 *
 * if fail to get source (mutex/condition variable) return false.
 */
static bool tlog_queue_create(tlog_queue_blocking*,tlog_struct_log_element *,
                              uint16_t, tlog_enum_queue_start);

/**
 * Push (insert) new element to queue, if queue is full wait for pop element.
 *
 * @tlog_queue_blocking     struct that manage the queue.
 * @tlog_struct_log_element element for push to queue.
 *
 * if queue is destroy return false.
 */
static bool tlog_queue_push(tlog_queue_blocking*, const tlog_struct_log_element*);

/**
 * Pop (remove) element from queue, if queue is empty wait for push new element.
 *
 * @tlog_queue_blocking     struct that manage the queue.
 * @tlog_struct_log_element element for pop from queue.
 *
 * if queue is destroy return false.
 */
static bool tlog_queue_pop(tlog_queue_blocking*, tlog_struct_log_element*);

/**
 * Set flag that queue stop running and notify to all condition to wake.
 */
static void tlog_queue_stop(tlog_queue_blocking*);

/**
 * Stop the run of queue, and free the allocation for cond & mutex.
 */
static void tlog_queue_destroy(tlog_queue_blocking*);
///┌─────────────┐
///├   Logger    ┤
///└─────────────┘
/**
 * Write time that provided, as string format, according to enum in tlog.
 * use internal static buffer, so it's not thread safe. 
 * save flags, if already write the time, not write twice (return the prev result). 
 * there is num of options to format, according to enum.
 * 0. LOG_TIME_NO_TIME:    empty string.
 * 1. LOG_TIME_ONLY_MICRO: only microseconds.
 * 2. LOG_TIME_HOURS:      add time until hours (include hours.minutes.seconds).
 * 3. LOG_TIME_DATE        add date (include day.month.year)
 */
const char* tlog_get_current_time_str(TLOG_TIME_T);
/**
 * Write counter in inner static buffer (not thread safe)
 * convert uint16 into string format [0000].
 * if request to write the same number twice, in the second time return the prev buffer. 
 */
const char* tlog_counter_to_str(uint16_t);
/**
 * Pass other file pointer and stdout pointer, and call to callback function with params. 
 */
void tlog_log_out(const tlog_struct_log_element*);
/**
 * The function that inner tlog thread run. 
 * pop element to handle from queue, request to log out,
 * and return the buffer to pool. 
 */
THREAD_RETURN tlog_thread_function(THREAD_ARG);
/**
 * Function for convert struct of level to arrays of level according to enum
 */
void tlog_save_log_level(bool arrLog_level[NUM_OF_LOG_OPTION], tlog_struct_levels);
/**
 * Callback that set when request default params. 
 * log in format of [counter][time][level] msg. 
 * also add colors to stdout if is possible. 
 */
void tlog_default_callback(const tlog_struct_callback_param*);
/**
 * Inner implementation that take the mutex from queue, 
 * and log out the all msg in queue. 
 * this function call in tlog_free for not lose msgs at exit.
 */
void tlog_flash();
/**
 * check if file in tlog is not null, close the file, and set null.
 */
void tlog_close_file();
/**
 * Check if according to level, is need to log at all. 
 */
bool tlog_is_need_to_log(tlog_struct_levels);
/**
 * free all allocations in tlog. 
 */
void tlog_free_allocations();

///┌─────────────────────────────────────────────────┐
///├                API_FUNCTIONS                    ┤
///└─────────────────────────────────────────────────┘
///┌─────────────┐
///├   INIT      ┤
///└─────────────┘
tlog_enum_errors tlog_init(const tlog_struct_config *s_config)
{
    // can't init twice.
    if (tlog.m_log_is_run) {
        return LOG_ERR_ALREADY_INIT;
    }
    // can't pass null as param.
    if (!s_config) {
        return LOG_ERR_PARAMS_NULL;
    }

    // calculate alloc size for all elements,
    const uint32_t u32SizeToAlloc = (sizeof(tlog_struct_log_element) * s_config->u8_capacity * 2) +
                                    (s_config->u8_capacity * s_config->u16_msg_len);

    /// check if need to log file.
    const bool bNeedLogFile = tlog_is_need_to_log(s_config->s_levels_file);
    /// check if need to log console.
    const bool bNeedLogConsole =  tlog_is_need_to_log(s_config->s_levels_console);

    // if not need to log.
    if (!bNeedLogConsole && !bNeedLogFile)
    {
        // set that log is init.
        tlog.m_log_is_run = true;
        // return true.
        return LOG_ERR_SUCCESS;
    }
    // not allow empty capacity.
    if (0 == s_config->u8_capacity) {
        return LOG_ERR_INVALID_CAPACITY;
    }
    // need to set len for write msg.
    if (0 == s_config->u16_msg_len) {
        return LOG_ERR_INVALID_MSG_LEN;
    }
    // if you need log to file, can't pass null.
    if (bNeedLogFile && !s_config->pc_file) {
        return LOG_ERR_FILE_NULL;
    }
    // callback can't be null, use default.
    if (!s_config->pf_callback) {
        return LOG_ERR_CALLBACK_NULL;
    }
    // time not in enum range
    if (LOG_TIME_NO_TIME > s_config->m_enum_time || LOG_TIME_DATE < s_config->m_enum_time) {
        return LOG_ERR_ENUM_TIME_INVALID;
    }

#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            tlog.m_b_colors_enable = SetConsoleMode(hOut, mode) && _isatty(_fileno(stdout));
        }
    }
#else
    tlog.m_b_colors_enable = isatty(fileno(stdout));
#endif


    if (bNeedLogFile)
    {
        tlog.m_ptr_files[LOG_TO_FILE] = fopen(s_config->pc_file, "w");
        if (!tlog.m_ptr_files[LOG_TO_FILE])
        {
             return LOG_ERR_FAIL_OPEN_FILE;
        }
    }

    /// alloc memory for pool.
    tlog.m_p_data = malloc(u32SizeToAlloc);
    if (NULL == tlog.m_p_data)
    {
        tlog_free_allocations();
        return LOG_ERR_MEMORY_ALLOCATION;
    }


    /// queue get empty elements
    tlog_struct_log_element* queue_elements = tlog.m_p_data;
    /// pool look at element after queue
    tlog_struct_log_element* pool_elements = queue_elements + s_config->u8_capacity;
    /// after pool element there is the data.
    char* pElementData = (char*)(pool_elements + s_config->u8_capacity);

    for (uint16_t i = 0; i < s_config->u8_capacity; ++i)
    {
        pool_elements[i].m_data = pElementData + (i * s_config->u16_msg_len);
    }

    if ( !tlog_queue_create(&tlog.m_queue, queue_elements, s_config->u8_capacity, E_QUEUE_START_EMPTY) )
    {
        tlog_free_allocations();
        return LOG_ERR_MEMORY_ALLOCATION;
    }
    if ( !tlog_queue_create(&tlog.m_pool, pool_elements, s_config->u8_capacity, E_QUEUE_START_FULL) )
    {
        tlog_free_allocations();
        return LOG_ERR_MEMORY_ALLOCATION;
    }

    tlog.m_pf_callback = s_config->pf_callback;
    tlog.m_ptr_files[LOG_TO_CONSOLE] = stdout;
    tlog_save_log_level(tlog.m_level[LOG_TO_FILE], s_config->s_levels_file);
    tlog_save_log_level(tlog.m_level[LOG_TO_CONSOLE], s_config->s_levels_console);
    tlog.m_u16_size_of_element = s_config->u16_msg_len;
    tlog.m_log_is_run = true;
    tlog.m_enum_time = s_config->m_enum_time;
    tlog.m_b_log_counter = s_config->m_b_log_counter;

    thread_create(&tlog.m_thread, &tlog_thread_function ,NULL);
    if (tlog.m_thread <= 0)
    {
        tlog_free_allocations();
        return LOG_ERR_OPEN_THREAD;
    }

    return LOG_ERR_SUCCESS;
}

const char * tlog_strerror(const tlog_enum_errors e_error) {
    switch (e_error) {
        case LOG_ERR_SUCCESS:           return "success";
        case LOG_ERR_MEMORY_ALLOCATION: return "memory_allocation";
        case LOG_ERR_FILE_NULL:         return "file_null";
        case LOG_ERR_PARAMS_NULL:       return "params_null";
        case LOG_ERR_FAIL_OPEN_FILE:    return "fail_open_file";
        case LOG_ERR_INVALID_CAPACITY:  return "invalid_capacity";
        case LOG_ERR_INVALID_MSG_LEN:   return "invalid_msg_len";
        case LOG_ERR_CALLBACK_NULL:     return "callback_null";
        case LOG_ERR_OPEN_THREAD:       return "open_thread";
        case LOG_ERR_ENUM_TIME_INVALID: return "enum_time_invalid";
        case LOG_ERR_ALREADY_INIT:      return "already_init";
            default: return "undefined_error_code";
    }
}

bool tlog_suggest_file_name(const char *pc_argv_0, char *pc_buff, uint16_t u16_buff_len) {
    static const char str_ends[] = "_log.txt";
#ifdef _WIN32
    static const char c_file_seperator = '\\';
#define mkdir(path, mode) _mkdir(path)
#else
    static const char c_file_seperator = '/';
#endif
    static const char str_dir_name[] = {c_file_seperator, 'l','o','g','s', '\0'};

    if (!pc_argv_0 || !pc_buff) {
        return false;
    }
    const uint16_t len = strlen(pc_argv_0);
    const bool b_buffer_has_space = u16_buff_len >= len + sizeof(str_ends) + sizeof(str_dir_name);
    if (!b_buffer_has_space) {
        return false;
    }

    // copy all string to buffer.
    strncpy(pc_buff, pc_argv_0, u16_buff_len);

    // find exe dir
    uint16_t run_len = len;
    for (;pc_buff[run_len] != c_file_seperator && run_len != 0; --run_len) {
    }
    const uint16_t file_len = run_len;

    // copy log dir to file name
    strncpy(pc_buff + run_len, str_dir_name, sizeof(str_dir_name));
    run_len += sizeof(str_dir_name) - 1;

    if (0 != mkdir(pc_buff, 0755) && errno != EEXIST) {
        return false;
    }

    /// copy until '\0' or '.'
    for (uint16_t len_cpy = file_len; pc_argv_0[len_cpy] != '\0' && pc_argv_0[len_cpy] != '.'; ++len_cpy, ++run_len) {
        pc_buff[run_len] = pc_argv_0[len_cpy];
    }
    strncpy(pc_buff + run_len, str_ends, u16_buff_len - run_len);

    return true;
}

///┌─────────────┐
///├   FREE      ┤
///└─────────────┘
void tlog_free()
{
    tlog.m_log_is_run = false;

    if ( tlog.m_p_data )
    {
        tlog_queue_stop(&tlog.m_pool);
        tlog_queue_stop(&tlog.m_queue);

        thread_join(&tlog.m_thread);

        /// flash the remain msg.
        tlog_flash();

        tlog_free_allocations();
    }
}
///┌─────────────┐
///├  CALLBACK   ┤
///└─────────────┘
void tlog_default_callback(const tlog_struct_callback_param* s_param)
{
    // can't pass null params
    assert(NULL != s_param);
    // file can't be null.
    assert(NULL != s_param->p_file);
    // minimum log level is debug == 0.
    assert(s_param->e_level >= LOG_LEVEL_DEBUG);
    // max log level is error == 0.
    assert(s_param->e_level <= LOG_LEVEL_ERROR);

    const bool b_log_with_colors = (s_param->p_file == stdout && tlog.m_b_colors_enable);

    fprintf(s_param->p_file, "%s%s%s[%s] %s%s\n",
            b_log_with_colors ? tlog_colors(s_param->e_level) : "",
            tlog.m_b_log_counter ? tlog_counter_to_str(s_param->u16_log_counter) : "",
            tlog_get_current_time_str(s_param->time_of_log),
            tlog_array_str_level[s_param->e_level],
            s_param->pc_msg,
            b_log_with_colors ? "\x1b[0m" : "");
}
///┌─────────────┐
///├  CONFIG     ┤
///└─────────────┘
tlog_struct_config tlog_default_config()
{
    // on default.
    const tlog_struct_config ret_default =
    {
        // show msg only to console
        .s_levels_console = {true, true, true, true},
        .s_levels_file = {false, false, false, false},
        // can insert 16 msg to queue.
        .u8_capacity = 16,
        // msg for handle in sizeof-512.
        .u16_msg_len = 512,
        .pc_file = NULL,
        // there is default function for write output.
        .pf_callback = &tlog_default_callback,
        // by default log hours time.
        .m_enum_time = LOG_TIME_HOURS,
        .m_b_log_counter = true
    };
    return ret_default;
}

///┌─────────────┐
///├  LOG_FUNC   ┤
///└─────────────┘
#define LOG_FUNC(d_level) do { \
if (tlog.m_log_is_run && (tlog.m_level[LOG_TO_FILE][d_level] ||\
                          tlog.m_level[LOG_TO_CONSOLE][d_level])) {\
    va_list args;\
    tlog_struct_log_element element;\
    if ( tlog_queue_pop(&tlog.m_pool, &element) ) {\
        va_start(args, pcFormat);\
        if (-1 != vsnprintf(element.m_data, tlog.m_u16_size_of_element, pcFormat, args) ) {\
            if (LOG_TIME_NO_TIME != tlog.m_enum_time) {\
                TLOG_GET_TIME(element.m_time);\
            }\
            element.m_level = d_level; \
            tlog_queue_push(&tlog.m_queue, &element); \
        } \
        else {\
            tlog_queue_push(&tlog.m_pool, &element);\
        }\
        va_end(args);\
    }\
}}while (false)

void tlog_warning(const char *pcFormat, ...)
{
    LOG_FUNC(LOG_LEVEL_WARNING);
}
void tlog_error(const char *pcFormat, ...)
{
    LOG_FUNC(LOG_LEVEL_ERROR);
}
void tlog_debug(const char *pcFormat, ...)
{
    LOG_FUNC(LOG_LEVEL_DEBUG);
}
void tlog_info(const char *pcFormat, ...)
{
    LOG_FUNC(LOG_LEVEL_INFO);
}
#undef LOG_FUNC

///┌─────────────────────────────────────────────────┐
///├              HELPERS_FUNCTIONS                  ┤
///└─────────────────────────────────────────────────┘
const char* tlog_get_current_time_str(TLOG_TIME_T log_time)
{
    // Enough for timestamp with microseconds
    static char array_buffer_32[32];
    // return on error
    static const char* INVALID_TIME_STR = "[TIME_ERROR]";
    static TLOG_TIME_T PREV_LOG_TIME;
    static const char* P_ALREADY_LOG = "";

    // if not need to log return empty string
    if (tlog.m_enum_time == LOG_TIME_NO_TIME) {
        return "";
    }

    // if already log this time
    if (0 == memcmp(&PREV_LOG_TIME, &log_time, sizeof(log_time)) )
    {
        // return the prev buffer that return.
        return P_ALREADY_LOG;
    }
    /// save to next call.
    PREV_LOG_TIME = log_time;

#ifdef _WIN32
    // Convert FILETIME (100ns intervals since 1601) to 64-bit integer
    ULONGLONG filetime = ((ULONGLONG)log_time.dwHighDateTime << 32) | log_time.dwLowDateTime;

    // Adjust to UNIX epoch (1970)
    const ULONGLONG EPOCH_DIFF_100NS = 116444736000000000ULL;
    if (filetime < EPOCH_DIFF_100NS) {
        P_ALREADY_LOG = INVALID_TIME_STR;
        return INVALID_TIME_STR;
    }
    filetime -= EPOCH_DIFF_100NS;

    // Convert to seconds and microseconds
    const ULONGLONG raw_seconds = filetime / 10000000ULL;
    const long micros = (long)((filetime % 10000000ULL) / 10);

    // Guard: check if raw_seconds fits in time_t
    if (raw_seconds > (ULONGLONG)((time_t)-1)) {
        P_ALREADY_LOG = INVALID_TIME_STR;
        return INVALID_TIME_STR;
    }
    const time_t seconds = (time_t)raw_seconds;

    // Convert to local time
    struct tm tm_info;
    if (localtime_s(&tm_info, &seconds) != 0) {
        P_ALREADY_LOG = INVALID_TIME_STR;
        return INVALID_TIME_STR;
    }
    const struct tm* ptr_tm_info = &tm_info;

#else // POSIX systems
    const struct tm* ptr_tm_info = localtime(&log_time.tv_sec);
    if (!ptr_tm_info) {
        P_ALREADY_LOG = INVALID_TIME_STR;
        return INVALID_TIME_STR;
    }
    const long micros = log_time.tv_usec;
#endif

    int copy_size = 0;
    switch (tlog.m_enum_time) {
        case LOG_TIME_ONLY_MICRO: {
            copy_size = snprintf(array_buffer_32, sizeof(array_buffer_32),
                "[%06ld]", micros);
            break;
        }
        case LOG_TIME_HOURS: {
            copy_size = snprintf(array_buffer_32, sizeof(array_buffer_32),
             "[%02d:%02d:%02d.%06ld]",
             ptr_tm_info->tm_hour,
             ptr_tm_info->tm_min,
             ptr_tm_info->tm_sec,
             micros);
            break;
        }
        case LOG_TIME_DATE: {
            copy_size = snprintf(array_buffer_32, sizeof(array_buffer_32),
             "[%02d.%02d.%04d_%02d:%02d:%02d.%06ld]",
             ptr_tm_info->tm_mday,
             ptr_tm_info->tm_mon + 1,
             ptr_tm_info->tm_year + 1900,
             ptr_tm_info->tm_hour,
             ptr_tm_info->tm_min,
             ptr_tm_info->tm_sec,
             micros);
            break;
        }
        default: {
            P_ALREADY_LOG = INVALID_TIME_STR;
            return INVALID_TIME_STR;
        }
    }
    if (copy_size > sizeof(array_buffer_32)) {
        P_ALREADY_LOG = INVALID_TIME_STR;
        return INVALID_TIME_STR;
    }

    P_ALREADY_LOG = array_buffer_32;
    return array_buffer_32;
}

const char * tlog_counter_to_str(const uint16_t counter) {
    static char array_buffer_8[8];
    static uint16_t u16_prev_counter = UINT16_MAX;
    if (counter == u16_prev_counter) {
        return array_buffer_8;
    }
    u16_prev_counter = counter;
    if (sizeof(array_buffer_8) < snprintf(array_buffer_8, sizeof(array_buffer_8),
        "[%04u]", counter % 10000)) {
        return "";
    }
    return array_buffer_8;
}

void tlog_log_out(const tlog_struct_log_element* const tlog_element)
{
    // init params for log.
     tlog_struct_callback_param params={
        // msg that write to log.
        .pc_msg = tlog_element->m_data,
        // required level.
        .e_level = tlog_element->m_level,
        // file for write the msg.
        .p_file = NULL,
        // the time in call to write msg
        .time_of_log = tlog_element->m_time,
        // inner run counter.
        .u16_log_counter = 0
    };
    // pass over file and console.
    for (uint16_t u16Index = 0; u16Index < NUM_OF_LOG_OPTION; ++u16Index)
    {
        // if needed to log.
        if (tlog.m_level[u16Index][tlog_element->m_level])
        {
            // save counter as param.
            params.u16_log_counter = tlog.m_counters[u16Index]++;
            // save file for log.
            params.p_file = tlog.m_ptr_files[u16Index];
            // call callback for log.
            tlog.m_pf_callback(&params);
        }
    }
}

THREAD_RETURN tlog_thread_function(THREAD_ARG args) {
    tlog_struct_log_element logElement;
    (void)args;

    // while not free the logger.
    while (tlog.m_log_is_run)
    {
        // take new msg that wait to log.
        if ( !tlog_queue_pop(&tlog.m_queue, &logElement) )
        {
            // if failed, log not run anymore, set stop.
            tlog.m_log_is_run = false;
        }
        else
        {
            // if successes , log out the element.
            tlog_log_out(&logElement);
            // return element to pull.
            tlog_queue_push(&tlog.m_pool, &logElement);
        }
    }
    return 0;
}

void tlog_save_log_level(bool arrLog_level[NUM_OF_LOG_OPTION], const tlog_struct_levels sLevels)
{
    // save level in array
    arrLog_level[LOG_LEVEL_DEBUG]   = sLevels.m_debug;
    arrLog_level[LOG_LEVEL_INFO]    = sLevels.m_info;
    arrLog_level[LOG_LEVEL_WARNING] = sLevels.m_warning;
    arrLog_level[LOG_LEVEL_ERROR]   = sLevels.m_error;
}

void tlog_close_file()
{
    // if open
    if (tlog.m_ptr_files[LOG_TO_FILE])
    {
        // close
        fclose(tlog.m_ptr_files[LOG_TO_FILE]);
        // set pointer as close.
        tlog.m_ptr_files[LOG_TO_FILE] = NULL;
    }
}

void tlog_flash()
{
    // element for hold params.
    tlog_struct_log_element element;
    // lock the access to queue.
    mutex_lock(&tlog.m_queue.mutex_lock);

    // queue is not empty
    while (0 != tlog.m_queue.u16_count)
    {
        // pop element
        element = tlog.m_queue.ps_items[tlog.m_queue.u16_front];
        tlog.m_queue.u16_front = (tlog.m_queue.u16_front + 1) % tlog.m_queue.u16_capacity;
        --tlog.m_queue.u16_count;

        // log out.
        tlog_log_out(&element);
    }
    // open the lock.
    mutex_unlock(&tlog.m_queue.mutex_lock);
}

bool tlog_is_need_to_log(tlog_struct_levels s_levels)
{
    return s_levels.m_debug || s_levels.m_error || s_levels.m_info || s_levels.m_warning;
}

void tlog_free_allocations()
{
    tlog_queue_destroy(&tlog.m_queue);
    tlog_queue_destroy(&tlog.m_pool);
    tlog_close_file();
    if ( tlog.m_p_data ) {
        free(tlog.m_p_data);
        tlog.m_p_data = NULL;
    }
    tlog.m_log_is_run = false;
    memset(tlog.m_level, 0, sizeof(tlog.m_level));
    tlog.m_pf_callback = NULL;
    tlog.m_ptr_files[LOG_TO_CONSOLE] = NULL;
    tlog.m_u16_size_of_element = 0;
    tlog.m_b_log_counter = false;
    tlog.m_b_colors_enable = false;
    tlog.m_counters[LOG_TO_CONSOLE] = 0;
    tlog.m_counters[LOG_TO_FILE] = 0;
    tlog.m_enum_time = LOG_TIME_NO_TIME;
}

///┌─────────────────────────────────────────────────┐
///├                   Queue                         ┤
///└─────────────────────────────────────────────────┘
bool tlog_queue_create(tlog_queue_blocking* q, tlog_struct_log_element *arrElements, const uint16_t u16ArraySize, const tlog_enum_queue_start eStart)
{
    if (q->b_allocated) {return false;}
    // save the items.
    q->ps_items = arrElements;
    // save the capacity.
    q->u16_capacity = u16ArraySize;
    // init front
    q->u16_front = 0;
    // init rear
    q->u16_rear = 0;
    // if queue start full count == capacity, if start empty count == 0.
    q->u16_count = u16ArraySize * eStart;

    // init mutex
    if ( 0 != mutex_init(&q->mutex_lock) ) {
        return false;
    }
    // init condition full.
    if (0 != cond_init(&q->cond_not_full) ) {
        mutex_destroy(&q->mutex_lock);
        return false;
    }
    // init condition empty.
    if (0 != cond_init(&q->cond_not_empty) ) {
        mutex_destroy(&q->mutex_lock);
        cond_destroy(&q->cond_not_full);
        return false;
    }
    // if success set that run.
    q->b_queue_run = true;
    q->b_allocated = true;
    return true;
}

// Blocking enqueue
bool tlog_queue_push(tlog_queue_blocking *q, const tlog_struct_log_element* element)
{
    if (NULL == q || !q->b_queue_run || !q->b_allocated) { return false; }

    mutex_lock(&q->mutex_lock);

    while (q->u16_count == q->u16_capacity && q->b_queue_run)
    {
        // Wait until there's space
        cond_wait(&q->cond_not_full, &q->mutex_lock);
    }
    if (q->b_queue_run)
    {
        q->ps_items[q->u16_rear] = *element;
        q->u16_rear = (q->u16_rear + 1) % q->u16_capacity;
        q->u16_count++;

        // Signal that the queue is not empty
        cond_signal(&q->cond_not_empty);
    }
    const bool status = q->b_queue_run;
    mutex_unlock(&q->mutex_lock);

    return status;
}

// Blocking dequeue
bool tlog_queue_pop(tlog_queue_blocking *q, tlog_struct_log_element* element)
{
    if (NULL == q || !q->b_queue_run || !q->b_allocated) { return false; }

    mutex_lock(&q->mutex_lock);

    while (0 == q->u16_count && q->b_queue_run)
    {
        /// Wait until there's something to dequeue
        cond_wait(&q->cond_not_empty, &q->mutex_lock);
    }
    if (q->b_queue_run)
    {
        *element = q->ps_items[q->u16_front];
        q->u16_front = (q->u16_front + 1) % q->u16_capacity;
        --q->u16_count;

        /// Signal that the queue is not full
        cond_signal(&q->cond_not_full);
    }
    const bool status = q->b_queue_run;

    mutex_unlock(&q->mutex_lock);

    return status;
}


void tlog_queue_stop(tlog_queue_blocking* q)
{
    if (!q->b_allocated) {return;}

    mutex_lock(&q->mutex_lock);
    // set false for stop pop from q.
    q->b_queue_run = false;
    mutex_unlock(&q->mutex_lock);

    // wake up the all conditions
    cond_broadcast(&q->cond_not_full);
    cond_broadcast(&q->cond_not_empty);
}

void tlog_queue_destroy(tlog_queue_blocking* q)
{
    if (q->b_allocated)
    {
        // stop.
        tlog_queue_stop(q);

        // prevent access.
        q->u16_capacity = 0;
        q->u16_count = 0;
        q->u16_front = 0;
        q->u16_rear = 0;
        q->ps_items = NULL;

        // destroy allocations
        mutex_destroy(&q->mutex_lock);
        cond_destroy(&q->cond_not_full);
        cond_destroy(&q->cond_not_empty);
        q->b_allocated = false;
    }
}