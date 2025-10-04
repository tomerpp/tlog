//
// Example for tlog - Created by Tomer Pires.
//
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