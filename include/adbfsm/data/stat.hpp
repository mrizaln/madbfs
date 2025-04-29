#pragma once

#include <sys/stat.h>

namespace adbfsm::data
{
    struct Stat
    {
        mode_t  mode  = 0;    // -rwxrwxrwx
        nlink_t links = 1;
        uid_t   uid   = 0;
        gid_t   gid   = 0;
        off_t   size  = 0;
        time_t  mtime = {};    // last modification time (only seconds part is used)
    };
}
