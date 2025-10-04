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
#ifndef TLOG_LOGGER_H
#define TLOG_LOGGER_H

#include <stdint.h> /* uint16_t, uint8_t */
#include <stdbool.h> /* bool */

///┌──────────────┐
///├   WINDOWS    ┤
///└──────────────┘
#ifdef _WIN32
#include <windows.h>
#define TLOG_TIME_T FILETIME
#define TLOG_GET_TIME(tlog_time) do{GetSystemTimePreciseAsFileTime(&tlog_time);}while(false)
///┌──────────────┐
///├     UNIX     ┤
///└──────────────┘
#else
#include <sys/time.h>

#define TLOG_TIME_T struct timeval
#define TLOG_GET_TIME(tlog_time) do{gettimeofday(&tlog_time, NULL);}while(false)
#endif

///┌─────────────────────────────────────────────────┐
///├____________________API__________________________┤
///└─────────────────────────────────────────────────┘
enum tlog_enum_errors;
struct tlog_struct_config;

/// * console:   all levels set to true.
/// * file:      all levels set to false and file pointer set to null.
/// * capacity:  num of element in queue, set by default to 16.
/// * msg len:   the len of msg that can write to log, set 512.
/// * callback:  there is default callback, for log out.
/// * enum_time: set the time in out put, by default show hours level.
/// * counter:   set true, show the counter in lof msg.
/// @return      default configuration for logger.
struct tlog_struct_config tlog_default_config();

/// function for initialization the logger, after the success, the log show msg.
/// @param s_config configuration for init the logger, can use "tlog_default_config".
/// @return enum that set if it has error or success.
enum tlog_enum_errors tlog_init(const struct tlog_struct_config* s_config);

/// convert the error enum to string.
/// @param e_error return value from "tlog_init".
/// @return string for read the error in enum.
const char* tlog_strerror(enum tlog_enum_errors e_error);

/// create file name according to exe file.
/// @param pc_argv_0 argv[0] for create logs dir next to exe file.
/// @param pc_buff buff for write the file name.
/// @param u16_buff_len the alloc size in buff.
/// @return if it has space in buff and success to open dir for log, return true.
bool tlog_suggest_file_name(const char* pc_argv_0, char* pc_buff, uint16_t u16_buff_len);

/// need to call this at end of use in log for free memory and fetch the last msgs.
void tlog_free();

/// log warning msg just if set in init to log.
/// @param pcFormat format for create msg.
/// @param ... params that set in format.
void tlog_warning(const char* pcFormat, ...) __attribute__((format(printf, 1, 2)));

/// log error msg just if set in init to log.
/// @param pcFormat format for create msg.
/// @param ... params that set in format.
void tlog_error(const char* pcFormat, ...) __attribute__((format(printf, 1, 2)));

/// log debug msg just if set in init to log.
/// @param pcFormat format for create msg.
/// @param ... params that set in format.
void tlog_debug(const char* pcFormat, ...) __attribute__((format(printf, 1, 2)));

/// log informative msg just if set in init to log.
/// @param pcFormat format for create msg.
/// @param ... params that set in format.
void tlog_info(const char* pcFormat, ...) __attribute__((format(printf, 1, 2)));


///┌─────────────────────────────────────────────────┐
///├                    ENUMS                        ┤
///└─────────────────────────────────────────────────┘
/// Specifies the log level, used for callback function,
/// and internal implementations.
typedef enum tlog_enum_levels {
    LOG_LEVEL_DEBUG   = 0,
    LOG_LEVEL_INFO    = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR   = 3,
}tlog_enum_levels;
#define NUM_OF_LEVELS 4

/// All possible errors in log initialization.
typedef enum tlog_enum_errors {
    /// success to init log - can use log.
    LOG_ERR_SUCCESS             = 0,
    /// fail to alloc memory or resource.
    LOG_ERR_MEMORY_ALLOCATION   = 1,
    /// request log to file, but not provide file name.
    LOG_ERR_FILE_NULL           = 2,
    /// pass null as param to init function.
    LOG_ERR_PARAMS_NULL         = 3,
    /// fail to call fopen for open file.
    LOG_ERR_FAIL_OPEN_FILE      = 4,
    /// set capacity to 0, is error.
    LOG_ERR_INVALID_CAPACITY    = 5,
    /// set len 0, is error.
    LOG_ERR_INVALID_MSG_LEN     = 6,
    /// not provide callback (can use default from "tlog_default_config").
    LOG_ERR_CALLBACK_NULL       = 7,
    /// fail to open thread.
    LOG_ERR_OPEN_THREAD         = 8,
    /// provide enum that is not from "tlog_enum_time_level"
    LOG_ERR_ENUM_TIME_INVALID   = 9,
    /// try to init twice without free between.
    LOG_ERR_ALREADY_INIT        = 10,
}tlog_enum_errors;

/// Define the log time output.
typedef enum tlog_enum_time_level {
    /// not log the time.
    LOG_TIME_NO_TIME,
    /// log time in microseconds only.
    LOG_TIME_ONLY_MICRO,
    /// log time of hour that log call HH:MM:SS.microseconds.
    LOG_TIME_HOURS,
    /// log the date that call DD:MM:YYYY HH:MM:SS.microseconds
    LOG_TIME_DATE,
}tlog_enum_time_level;
///┌─────────────────────────────────────────────────┐
///├                  STRUCTURES                     ┤
///└─────────────────────────────────────────────────┘
/// Defines which log levels should be active at startup.
typedef struct tlog_struct_levels
{
    bool m_debug;
    bool m_info;
    bool m_warning;
    bool m_error;
}tlog_struct_levels;

/// The parameters received in the callback function
typedef struct tlog_struct_callback_param
{
    /// msg that write to log.
    const char* pc_msg;
    /// required level.
    tlog_enum_levels e_level;
    /// file for write the msg.
    FILE* p_file;
    /// the time in call to write msg
    TLOG_TIME_T time_of_log;
    /// inner run counter.
    uint16_t u16_log_counter;
}tlog_struct_callback_param;

/// config params for tlog_init function.
typedef struct tlog_struct_config
{
    /// levels that write to console.
    tlog_struct_levels s_levels_console;
    /// levels that write to file.
    tlog_struct_levels s_levels_file;
    /// how msg can insert to log.
    uint8_t u8_capacity;
    /// size of each msg.
    uint16_t u16_msg_len;
    /// file name.
    const char* pc_file;
    /// function that call for output the buffer.
    void(*pf_callback)(const tlog_struct_callback_param*);
    /// time level for log.
    tlog_enum_time_level m_enum_time;
    /// count the log output.
    bool m_b_log_counter;
}tlog_struct_config;

#endif //TLOG_LOGGER_H