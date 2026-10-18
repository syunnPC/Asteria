// SPDX-License-Identifier: Apache-2.0

#include "Include/string.h"

#include <stdint.h>

namespace Asteria::User::Libc
{
namespace
{
char g_unknown[48U]{};
// Mutable storage matches the C API signature. Applications must not modify it.
char g_messages[] = "Success\0"
                    "Operation not permitted\0"
                    "No such file or directory\0"
                    "No such process\0"
                    "Interrupted system call\0"
                    "Input/output error\0"
                    "No such device or address\0"
                    "Argument list too long\0"
                    "Exec format error\0"
                    "Bad file descriptor\0"
                    "No child processes\0"
                    "Resource temporarily unavailable\0"
                    "Cannot allocate memory\0"
                    "Permission denied\0"
                    "Bad address\0"
                    "Block device required\0"
                    "Device or resource busy\0"
                    "File exists\0"
                    "Invalid cross-device link\0"
                    "No such device\0"
                    "Not a directory\0"
                    "Is a directory\0"
                    "Invalid argument\0"
                    "Too many open files in system\0"
                    "Too many open files\0"
                    "Inappropriate ioctl for device\0"
                    "Text file busy\0"
                    "File too large\0"
                    "No space left on device\0"
                    "Illegal seek\0"
                    "Read-only file system\0"
                    "Too many links\0"
                    "Broken pipe\0"
                    "Numerical argument out of domain\0"
                    "Numerical result out of range\0"
                    "Resource deadlock avoided\0"
                    "File name too long\0"
                    "No locks available\0"
                    "Function not implemented\0"
                    "Directory not empty\0"
                    "Too many levels of symbolic links\0"
                    "Unknown error 41\0"
                    "No message of desired type\0"
                    "Identifier removed\0"
                    "Channel number out of range\0"
                    "Level 2 not synchronized\0"
                    "Level 3 halted\0"
                    "Level 3 reset\0"
                    "Link number out of range\0"
                    "Protocol driver not attached\0"
                    "No CSI structure available\0"
                    "Level 2 halted\0"
                    "Invalid exchange\0"
                    "Invalid request descriptor\0"
                    "Exchange full\0"
                    "No anode\0"
                    "Invalid request code\0"
                    "Invalid slot\0"
                    "Unknown error 58\0"
                    "Bad font file format\0"
                    "Device not a stream\0"
                    "No data available\0"
                    "Timer expired\0"
                    "Out of streams resources\0"
                    "Machine is not on the network\0"
                    "Package not installed\0"
                    "Object is remote\0"
                    "Link has been severed\0"
                    "Advertise error\0"
                    "Srmount error\0"
                    "Communication error on send\0"
                    "Protocol error\0"
                    "Multihop attempted\0"
                    "RFS specific error\0"
                    "Bad message\0"
                    "Value too large for defined data type\0"
                    "Name not unique on network\0"
                    "File descriptor in bad state\0"
                    "Remote address changed\0"
                    "Can not access a needed shared library\0"
                    "Accessing a corrupted shared library\0"
                    ".lib section in a.out corrupted\0"
                    "Attempting to link in too many shared libraries\0"
                    "Cannot exec a shared library directly\0"
                    "Invalid or incomplete multibyte or wide character\0"
                    "Interrupted system call should be restarted\0"
                    "Streams pipe error\0"
                    "Too many users\0"
                    "Socket operation on non-socket\0"
                    "Destination address required\0"
                    "Message too long\0"
                    "Protocol wrong type for socket\0"
                    "Protocol not available\0"
                    "Protocol not supported\0"
                    "Socket type not supported\0"
                    "Operation not supported\0"
                    "Protocol family not supported\0"
                    "Address family not supported by protocol\0"
                    "Address already in use\0"
                    "Cannot assign requested address\0"
                    "Network is down\0"
                    "Network is unreachable\0"
                    "Network dropped connection on reset\0"
                    "Software caused connection abort\0"
                    "Connection reset by peer\0"
                    "No buffer space available\0"
                    "Transport endpoint is already connected\0"
                    "Transport endpoint is not connected\0"
                    "Cannot send after transport endpoint shutdown\0"
                    "Too many references: cannot splice\0"
                    "Connection timed out\0"
                    "Connection refused\0"
                    "Host is down\0"
                    "No route to host\0"
                    "Operation already in progress\0"
                    "Operation now in progress\0"
                    "Stale file handle\0"
                    "Structure needs cleaning\0"
                    "Not a XENIX named type file\0"
                    "No XENIX semaphores available\0"
                    "Is a named type file\0"
                    "Remote I/O error\0"
                    "Disk quota exceeded\0"
                    "No medium found\0"
                    "Wrong medium type\0"
                    "Operation canceled\0"
                    "Required key not available\0"
                    "Key has expired\0"
                    "Key has been revoked\0"
                    "Key was rejected by service\0"
                    "Owner died\0"
                    "State not recoverable\0"
                    "Operation not possible due to RF-kill\0"
                    "Memory page has hardware error\0";
constexpr uint16_t MessageOffsets[]{
    0U,    8U,    32U,   58U,   74U,   98U,   117U,  143U,  166U,  184U,  204U,  223U,  256U,  279U,  297U,
    309U,  331U,  355U,  367U,  393U,  408U,  424U,  439U,  456U,  486U,  506U,  537U,  552U,  567U,  591U,
    604U,  626U,  641U,  653U,  686U,  716U,  742U,  761U,  780U,  805U,  825U,  859U,  876U,  903U,  922U,
    950U,  975U,  990U,  1004U, 1029U, 1058U, 1085U, 1100U, 1117U, 1144U, 1158U, 1167U, 1188U, 1201U, 1218U,
    1239U, 1259U, 1277U, 1291U, 1316U, 1346U, 1368U, 1385U, 1407U, 1423U, 1437U, 1465U, 1480U, 1499U, 1518U,
    1530U, 1568U, 1595U, 1624U, 1647U, 1686U, 1723U, 1755U, 1803U, 1841U, 1891U, 1935U, 1954U, 1969U, 2000U,
    2029U, 2046U, 2077U, 2100U, 2123U, 2149U, 2173U, 2203U, 2244U, 2267U, 2299U, 2315U, 2338U, 2374U, 2407U,
    2432U, 2458U, 2498U, 2534U, 2580U, 2615U, 2636U, 2655U, 2668U, 2685U, 2715U, 2741U, 2759U, 2784U, 2812U,
    2842U, 2863U, 2880U, 2900U, 2916U, 2934U, 2953U, 2980U, 2996U, 3017U, 3045U, 3056U, 3078U, 3116U,
};
}
}

extern "C" char* ASTERIA_LIBC_NAME(strerror)(const int error) noexcept
{
    if (error >= 0 && error < 134)
    {
        return Asteria::User::Libc::g_messages + Asteria::User::Libc::MessageOffsets[static_cast<size_t>(error)];
    }
    char* const result = Asteria::User::Libc::g_unknown;
    static_cast<void>(ASTERIA_LIBC_NAME(strcpy)(result, "Unknown error "));
    size_t length{14U};
    unsigned int magnitude = static_cast<unsigned int>(error);
    if (error < 0)
    {
        result[length++] = '-';
        magnitude = 0U - magnitude;
    }
    const size_t start{length};
    do
    {
        result[length++] = static_cast<char>('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U);
    result[length] = '\0';
    for (size_t index{0U}; index < (length - start) / 2U; ++index)
    {
        const char saved = result[start + index];
        result[start + index] = result[length - index - 1U];
        result[length - index - 1U] = saved;
    }
    return result;
}
