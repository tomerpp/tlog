# 🧾 TLOG – Lightweight Threaded Logger in C

`TLOG` is a lightweight, configurable, multi-threaded logger for C applications.  
It is designed to be **easy to integrate**, **thread-safe**, and **low overhead** — while still offering flexible log routing and formatting.

---

## 🚀 Features

- ✅ Thread-safe logging using a background thread and queue
- ✅ Configurable log levels for file and console separately
- ✅ Optional timestamping and message counters
- ✅ Blocking memory pool – no dynamic allocations during runtime
- ✅ Fully configurable via struct (`tlog_struct_config`)
- ✅ Minimal API – just two files (`tlog.h`, `tlog.c`)
- ✅ Optional user-defined output callback
- ✅ No external dependencies – uses only standard C libraries

---

## 📦 Installation

Just copy `tlog.h` and `tlog.c` into your project.  
Include `tlog.h` where needed.

```c++
#include "tlog.h"
```
---
## 🧪 Example Usage
from [main.c](./main.c)
```c++
#include <stdio.h>
#include "tlog.h"

bool init_log(const char* pc_argv_0)
{
    // set buffer size for file name (take full path).
    char buff_file_name[128] = {};
    // create name for file.
    const bool has_file_in_buff = tlog_suggest_file_name(pc_argv_0, buff_file_name, sizeof(buff_file_name));

    // create configuration for log (start with default).
    tlog_struct_config conf = tlog_default_config();

    // change conf has I want.
    // * show only micro
    conf.m_enum_time = LOG_TIME_ONLY_MICRO;
    // * set error to file.
    conf.s_levels_file.m_error = true;
    // * if set true in file, must provided file name.
    conf.pc_file = has_file_in_buff ? buff_file_name : "tlog_example.txt";

    // init the log (must free at end of use)
    const tlog_enum_errors err = tlog_init(&conf);
    const bool b_init_is_success = err == LOG_ERR_SUCCESS;
    if (b_init_is_success)
    {
        // on success can use log.
        tlog_info("log output: \"%s\"", conf.pc_file);
    }
    else
    {
        // on failed there is not meaning to log.
        printf("log: fail init error = %s\n", tlog_strerror(err));
    }
    return b_init_is_success;
}


int main(int argc, const char** argv) {
    // init log from individual implementation
    if (init_log(argv[0])) {
        // run over log option
        for (uint16_t i = 0; i < 5; ++i) {
            // log one by one
            tlog_error(  "example to error  [%u]", i);
            tlog_info(   "example to info   [%u]", i);
            tlog_warning("example to warning[%u]", i);
            tlog_debug(  "example to debug  [%u]", i);
        }
        // free at end of use (also do fetch for print all msgs).
        tlog_free();
    }
    return 0;
}
```
---
## ⚙️ Configuration (tlog_struct_config)
Use `tlog_default_config()` to start with a default configuration, then override what you need.
```c++
typedef struct tlog_struct_config {
    tlog_struct_levels s_levels_console;  // levels printed to console
    tlog_struct_levels s_levels_file;     // levels written to file
    uint8_t  u8_capacity;                 // queue capacity (pool size)
    uint16_t u16_msg_len;                // max length per log message
    const char* pc_file;                 // output file path (if used)
    void (*pf_callback)(const tlog_struct_callback_param*); // optional output override
    tlog_enum_time_level m_enum_time;    // time format in logs
    bool m_b_log_counter;                // add counter to each log line
} tlog_struct_config;
```
### Time Options (tlog_enum_time_level)
1. LOG_TIME_NONE: ` `
2. LOG_TIME_ONLY_MICRO: `UUUUUU` 
3. LOG_TIME_HOURS:`HH:MM:SS.UUUUUU` 
4. LOG_TIME_FULL_DATE: `DD.MM.YYYY_HH:MM:SS.UUUUUU`
---
## 🧵 Multithreading Details
* A dedicated background thread handles writing messages from a queue.
* All writes are thread-safe via internal mutex + cond_var protections.
* A blocking memory pool prevents dynamic allocation during runtime. 
* If the pool is full, the logging call will block until an element is available.

---

## 📤 Log Format
```
[0001][04.10.2025_12:03:44.000123][INFO] log message here
```
### Components:

* `[counter]`: Optional. Enabled via `m_b_log_counter`.
* `[time]`: Optional. Controlled by `m_enum_time`.
* `[level]`: one of `INF, DBG, WRN, ERR`
---
## 💾 File Output
* Output file is kept open during runtime. 
* File is flushed on every message (`fprintf`) but not rotated. 
* No limit on file size – use with care in long-running systems. 
* File must be specified if any file level is enabled.

---
## 🔚 Cleanup

Always call tlog_free() at the end of your program:
```c
tlog_free();
```

This function:
* Flushes all remaining messages 
* Frees memory 
* Closes the output file 
* Stops the background thread

⚠️ Not calling `tlog_free()` will result in memory/file handle leaks.

---

## 📘 Public API
```c++
struct tlog_struct_config tlog_default_config();
enum tlog_enum_errors     tlog_init(const struct tlog_struct_config* config);
void                      tlog_free();
const char*               tlog_strerror(enum tlog_enum_errors e_error);
bool                      tlog_suggest_file_name(const char* argv0, char* buffer, uint16_t len);

void tlog_warning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void tlog_error  (const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void tlog_debug  (const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void tlog_info   (const char* fmt, ...) __attribute__((format(printf, 1, 2)));
```

---

## 📊 Performance Notes

* Logging is offloaded to a separate thread for minimal delay.
* Logging calls block only if the message pool is exhausted. 
* No dynamic memory allocation after initialization. 
* Suitable for real-time or embedded-like systems with careful configuration.

---

## 📌 Limitations
* No runtime configuration change (log levels, file name, etc.)
* No file rotation or size limit (user is responsible for cleanup)
* Blocking logger — will wait if the queue is full 
* Not suitable for systems where blocking is unacceptable

---

## 🧱 Internal Structure
* 1x worker thread 
* 2x blocking queue one for queue, and one for pool.
  * any queue has 1 mutex.
  * any queue has 2 condition variable (save from empty/full).
  
---

## 🧩 Extending

You can provide a custom output handler via the `pf_callback` field in the config.
This function receives a `tlog_struct_callback_param*` with the formatted message.

---

## 📁 File Structure

```
/project
├── tlog.h   // Public API (minimal)
└── tlog.c   // Full implementation
```

---

## 📜 License

This project is licensed under the MIT License – see the [LICENSE](./LICENSE) file for details.

---

## 🙋‍♂️ Questions?

Open an issue or contribute via pull request!