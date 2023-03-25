/*********************************************************
 * Copyright (C) 1998-2022 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/utsname.h>
#include <netdb.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <pwd.h>
#include <pthread.h>
#include <time.h>
#include <sys/resource.h>
#if defined(sun)
#include <sys/systeminfo.h>
#endif
#if defined(__HAIKU__)
#include <OS.h>
#endif
#include <sys/socket.h>
#if defined(__FreeBSD__) || defined(__APPLE__)
# include <sys/sysctl.h>
#endif
#if !defined(__APPLE__)
#define TARGET_OS_IPHONE 0
#endif
#if defined(__APPLE__)
#include <assert.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <CoreServices/CoreServices.h>
#endif
#include <mach-o/dyld.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <AvailabilityMacros.h>
#elif defined(__FreeBSD__)
#if !defined(RLIMIT_AS)
#  if defined(RLIMIT_VMEM)
#     define RLIMIT_AS RLIMIT_VMEM
#  else
#     define RLIMIT_AS RLIMIT_RSS
#  endif
#endif
#else
#if !defined(USING_AUTOCONF) || defined(HAVE_SYS_VFS_H)
#include <sys/vfs.h>
#endif
#if !defined(sun) && !defined __ANDROID__ && (!defined(USING_AUTOCONF) || \
                                              (defined(HAVE_SYS_IO_H) && \
                                               defined(HAVE_SYS_SYSINFO_H)))
#if defined(__i386__) || defined(__x86_64__)
#include <sys/io.h>
#else
#define NO_IOPL
#endif
#include <sys/sysinfo.h>
#ifndef HAVE_SYSINFO
#define HAVE_SYSINFO 1
#endif
#endif
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <paths.h>
#endif

#ifdef __linux__
#include <dlfcn.h>
#endif

#ifdef __HAIKU__
#include <FindDirectory.h>
#include <OS.h>
#include <private/shared/cpu_type.h>
#endif

#if !defined(_PATH_DEVNULL)
#define _PATH_DEVNULL "/dev/null"
#endif

#include "vmware.h"
#include "hostType.h"
#include "hostinfo.h"
#include "hostinfoInt.h"
#include "str.h"
#include "err.h"
#include "msg.h"
#include "log.h"
#include "posix.h"
#include "file.h"
#include "backdoor_def.h"
#include "util.h"
#include "vmstdio.h"
#include "su.h"
#include "vm_atomic.h"

#if defined(__i386__) || defined(__x86_64__)
#include "x86cpuid.h"
#endif

#include "unicode.h"
#include "guest_os.h"
#include "dynbuf.h"
#include "strutil.h"

#if defined(__APPLE__)
#include "utilMacos.h"
#include "rateconv.h"
#endif

#if defined(VMX86_SERVER) || defined(USERWORLD)
#include "uwvmkAPI.h"
#include "uwvmk.h"
#include "vmkSyscall.h"
#endif

#define LGPFX "HOSTINFO:"

#define SYSTEM_BITNESS_32 "i386"
#define SYSTEM_BITNESS_64_SUN "amd64"
#define SYSTEM_BITNESS_64_LINUX "x86_64"
#define SYSTEM_BITNESS_64_ARM_LINUX "aarch64"
#define SYSTEM_BITNESS_64_ARM_FREEBSD "arm64"
#define SYSTEM_BITNESS_MAXLEN \
   MAX(sizeof SYSTEM_BITNESS_32, \
   MAX(sizeof SYSTEM_BITNESS_64_SUN, \
   MAX(sizeof SYSTEM_BITNESS_64_LINUX, \
   MAX(sizeof SYSTEM_BITNESS_64_ARM_LINUX, \
       sizeof SYSTEM_BITNESS_64_ARM_FREEBSD))))

struct hostinfoOSVersion {
   int   hostinfoOSVersion[4];
   char *hostinfoOSVersionString;
};

static Atomic_Ptr hostinfoOSVersion;

#define DISTRO_BUF_SIZE 1024

#if !defined(__APPLE__) && !defined(VMX86_SERVER) && !defined(USERWORLD)
static const char *lsbFields[] = {
   "DISTRIB_ID=",
   "DISTRIB_RELEASE=",
   "DISTRIB_CODENAME=",
   "DISTRIB_DESCRIPTION=",
   NULL                      // MUST BE LAST
};

static const char *osReleaseFields[] = {
   "PRETTY_NAME=",
   "NAME=",
   "VERSION_ID=",
   "BUILD_ID=",
   "VERSION=",
   "CPE_NAME=",              // NIST CPE specification
   NULL                      // MUST BE LAST
};

typedef struct {
   const char *name;
   const char *filename;
} DistroInfo;

/*
 * This is the list of the location of the LSB standard distro identification
 * file for the distros known to the code in this file. The LSB standard is an
 * old standard, largely replaced by the superior os-release standard.
 *
 * If a distro *ALWAYS* supports the os-release standard (with or without an
 * LSB identifying file), there is no need to add anything to this list. For
 * distros that *ONLY* support the LSB standard, feel free to add an entry to
 * this table.
 *
 * KEEP SORTED! (sort -d)
 */

static const DistroInfo distroArray[] = {
   { "ALT",                "/etc/altlinux-release"      },
   { "Annvix",             "/etc/annvix-release"        },
   { "Arch",               "/etc/arch-release"          },
   { "Arklinux",           "/etc/arklinux-release"      },
   { "Asianux",            "/etc/asianux-release"       },
   { "Aurox",              "/etc/aurox-release"         },
   { "BlackCat",           "/etc/blackcat-release"      },
   { "Cobalt",             "/etc/cobalt-release"        },
   { "CentOS",             "/etc/centos-release"        },
   { "Conectiva",          "/etc/conectiva-release"     },
   { "Debian",             "/etc/debian_release"        },
   { "Debian",             "/etc/debian_version"        },
   { "Fedora Core",        "/etc/fedora-release"        },
   { "Gentoo",             "/etc/gentoo-release"        },
   { "Immunix",            "/etc/immunix-release"       },
   { "Knoppix",            "/etc/knoppix_version"       },
   { "Linux-From-Scratch", "/etc/lfs-release"           },
   { "Linux-PPC",          "/etc/linuxppc-release"      },
   { "Mandrake",           "/etc/mandrakelinux-release" },
   { "Mandrake",           "/etc/mandrake-release"      },
   { "Mandriva",           "/etc/mandriva-release"      },
   { "MkLinux",            "/etc/mklinux-release"       },
   { "Novell",             "/etc/nld-release"           },
   { "OracleLinux",        "/etc/oracle-release"        },
   { "Photon",             "/etc/lsb-release"           },
   { "PLD",                "/etc/pld-release"           },
   { "RedHat",             "/etc/redhat-release"        },
   { "RedHat",             "/etc/redhat_version"        },
   { "Slackware",          "/etc/slackware-release"     },
   { "Slackware",          "/etc/slackware-version"     },
   { "SMEServer",          "/etc/e-smith-release"       },
   { "Solaris",            "/etc/release"               },
   { "Sun",                "/etc/sun-release"           },
   { "SuSE",               "/etc/novell-release"        },
   { "SuSE",               "/etc/sles-release"          },
   { "SuSE",               "/etc/SuSE-release"          },
   { "Tiny Sofa",          "/etc/tinysofa-release"      },
   { "TurboLinux",         "/etc/turbolinux-release"    },
   { "Ubuntu",             "/etc/lsb-release"           },
   { "UltraPenguin",       "/etc/ultrapenguin-release"  },
   { "UnitedLinux",        "/etc/UnitedLinux-release"   },
   { "VALinux",            "/etc/va-release"            },
   { "Yellow Dog",         "/etc/yellowdog-release"     },
   { NULL,                 NULL                         },
};
#endif

/*
 * Any data obtained about the distro is obtained - UNMODIFIED - from the
 * standardized (i.e. LSB, os-release) disto description files. Under no
 * circumstances is code specific to a distro allowed or used. If the data
 * specified by a distro isn't useful, talk to the disto, not VMware.
 *
 * The fields from the standardized distro description files used may be
 * found above (i.e. lsbFields, osReleaseFields).
 *
 * Must be sorted. Keep in the same ordering as DetailedDataFieldType
 */

DetailedDataField detailedDataFields[] = {
#if defined(VM_ARM_ANY)
   { "architecture",      "Arm"   },  // Arm
#else
   { "architecture",      "X86"   },  // Intel/X86
#endif
   { "bitness",           ""      },  // "32" or "64"
   { "buildNumber",       ""      },  // When available
   { "cpeString",         ""      },  // When available
   { "distroAddlVersion", ""      },  // When available
   { "distroName",        ""      },  // Defaults to uname -s
   { "distroVersion",     ""      },  // When available
   { "familyName",        ""      },  // Defaults to uname -s
   { "kernelVersion",     ""      },  // Defaults to uname -r
   { "prettyName",        ""      },  // When available
   { NULL,                ""      },  // MUST BE LAST
};

#if defined __ANDROID__ || defined __aarch64__
/*
 * Android and arm64 do not support iopl().
 */
#define NO_IOPL
#endif

#if defined __ANDROID__
/*
 * Android doesn't support getloadavg().
 */
#define NO_GETLOADAVG
#endif


/*
 *----------------------------------------------------------------------
 *
 * HostinfoOSVersionInit --
 *
 *      Compute the OS version information
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      hostinfoOS* variables are filled in.
 *
 *----------------------------------------------------------------------
 */

static void
HostinfoOSVersionInit(void)
{
   struct hostinfoOSVersion *version;
   struct utsname u;
   char *extra;
   char *p;

   if (Atomic_ReadPtr(&hostinfoOSVersion)) {
      return;
   }

   if (uname(&u) == -1) {
      Warning("%s: unable to get host OS version (uname): %s\n",
	      __FUNCTION__, Err_Errno2String(errno));
      NOT_IMPLEMENTED();
   }

   version = Util_SafeCalloc(1, sizeof *version);
   version->hostinfoOSVersionString = Util_SafeStrndup(u.release,
                                                       sizeof u.release);

   ASSERT(ARRAYSIZE(version->hostinfoOSVersion) >= 4);

   /*
    * The first three numbers are separated by '.', if there is
    * a fourth number, it's probably separated by '.' or '-',
    * but it could be preceded by anything.
    */

   extra = Util_SafeCalloc(1, sizeof u.release);
   if (sscanf(u.release, "%d.%d.%d%s",
	      &version->hostinfoOSVersion[0], &version->hostinfoOSVersion[1],
	      &version->hostinfoOSVersion[2], extra) < 1) {
      Warning("%s: unable to parse host OS version string: %s\n",
              __FUNCTION__, u.release);
      NOT_IMPLEMENTED();
   }

   /*
    * If there is a 4th number, use it, otherwise use 0.
    * Explicitly skip over any non-digits, including '-'
    */

   p = extra;
   while (*p && !isdigit(*p)) {
      p++;
   }
   sscanf(p, "%d", &version->hostinfoOSVersion[3]);
   free(extra);

   if (Atomic_ReadIfEqualWritePtr(&hostinfoOSVersion, NULL, version)) {
      free(version->hostinfoOSVersionString);
      free(version);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_OSVersionString --
 *
 *	Returns the host version information as returned in the
 *      release field of uname(2)
 *
 * Results:
 *	const char * - pointer to static buffer containing the release
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

const char *
Hostinfo_OSVersionString(void)
{
   struct hostinfoOSVersion *version;

   HostinfoOSVersionInit();

   version = Atomic_ReadPtr(&hostinfoOSVersion);

   return version->hostinfoOSVersionString;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_OSVersion --
 *
 *      Host OS release info.
 *
 * Results:
 *      The i-th component of a dotted release string.
 *	0 if i is greater than the number of components we support.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Hostinfo_OSVersion(unsigned int i)  // IN:
{
   struct hostinfoOSVersion *version;

   HostinfoOSVersionInit();

   version = Atomic_ReadPtr(&hostinfoOSVersion);

   return (i < ARRAYSIZE(version->hostinfoOSVersion)) ?
           version->hostinfoOSVersion[i] : 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_GetTimeOfDay --
 *
 *      Return the current time of day according to the host.  We want
 *      UTC time (seconds since Jan 1, 1970).
 *
 * Results:
 *      Time of day in microseconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Hostinfo_GetTimeOfDay(VmTimeType *time)  // OUT:
{
   struct timeval tv;

   gettimeofday(&tv, NULL);

   *time = ((int64)tv.tv_sec * 1000000) + tv.tv_usec;
}


/*
 *----------------------------------------------------------------------------
 *
 * Hostinfo_GetSystemBitness --
 *
 *      Determines the operating system's bitness.
 *
 * Return value:
 *      32 or 64 on success.
 *      -1 on failure. Check errno for more details of error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

int
Hostinfo_GetSystemBitness(void)
{
#if defined __linux__
   struct utsname u;

   if (uname(&u) < 0) {
      return -1;
   }

   if (strstr(u.machine, SYSTEM_BITNESS_64_LINUX) ||
       strstr(u.machine, SYSTEM_BITNESS_64_ARM_LINUX)) {
      return 64;
   } else {
      return 32;
   }
#else
   char buf[SYSTEM_BITNESS_MAXLEN] = { '\0', };
#   if defined __FreeBSD__ || defined __APPLE__
   static int mib[2] = { CTL_HW, HW_MACHINE, };
   size_t len = sizeof buf;

   if (sysctl(mib, ARRAYSIZE(mib), buf, &len, NULL, 0) == -1) {
      return -1;
   }
#   elif defined sun
#      if !defined SOL10
   /*
    * XXX: This is bad.  We define SI_ARCHITECTURE_K to what it is on Solaris
    * 10 so that we can use a single guestd build for Solaris 9 and 10. In the
    * future we should have the Solaris 9 build just return 32 -- since it did
    * not support 64-bit x86 -- and let the Solaris 10 headers define
    * SI_ARCHITECTURE_K, then have the installer symlink to the correct binary.
    * For now, though, we'll share a single build for both versions.
    */
#         define SI_ARCHITECTURE_K  518
#      endif

   if (sysinfo(SI_ARCHITECTURE_K, buf, sizeof buf) < 0) {
      return -1;
   }
#   endif

   if (strcmp(buf, SYSTEM_BITNESS_32) == 0) {
      return 32;
   } else if (strcmp(buf, SYSTEM_BITNESS_64_SUN) == 0 ||
              strcmp(buf, SYSTEM_BITNESS_64_LINUX) == 0 ||
              strcmp(buf, SYSTEM_BITNESS_64_ARM_LINUX) == 0 ||
              strcmp(buf, SYSTEM_BITNESS_64_ARM_FREEBSD) == 0) {
      return 64;
   }

   return -1;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoPostData --
 *
 *      Post the OS name data to their cached values.
 *
 * Return value:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
HostinfoPostData(const char *osName,  // IN:
                 char *osNameFull)    // IN:
{
   unsigned int lastCharPos;
   static Atomic_uint32 mutex = {0};

   /*
    * Before returning, truncate the newline character at the end of the full
    * name.
    */

   lastCharPos = strlen(osNameFull) - 1;
   if (osNameFull[lastCharPos] == '\n') {
      osNameFull[lastCharPos] = '\0';
   }

   /*
    * Serialize access. Collisions should be rare - plus the value will get
    * cached and this won't get called anymore.
    */

   while (Atomic_ReadWrite(&mutex, 1)); // Spinlock.

   if (!hostinfoCacheValid) {
      Str_Strcpy(hostinfoCachedOSName, osName, sizeof hostinfoCachedOSName);
      Str_Strcpy(hostinfoCachedOSFullName, osNameFull,
                 sizeof hostinfoCachedOSFullName);
      hostinfoCacheValid = TRUE;
   }

   Atomic_Write(&mutex, 0);  // unlock
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoOSDetailedData --
 *
 *    Builds, escapes, and stores the detailed data into the cache.
 *
 * Return value:
 *      TRUE   Success
 *      FALSE  Failure
 *
 * Side effects:
 *      Cache values are set when returning TRUE
 *
 *-----------------------------------------------------------------------------
 */

static void
HostinfoOSDetailedData(void)
{
   DetailedDataField *field;
   Bool first = TRUE;

   /* Clear the string cache */
   memset(hostinfoCachedDetailedData, '\0',
          sizeof hostinfoCachedDetailedData);

   for (field = detailedDataFields; field->name != NULL; field++) {
      if (field->value[0] != '\0') {
         /* Account for the escape and NUL char */
         int len;
         const char *c;
         char escapedString[2 * MAX_DETAILED_FIELD_LEN + 1];
         char fieldString[MAX_DETAILED_FIELD_LEN];
         int32 i = 0;

         /* Add delimiter between properties - after the first one */
         if (!first) {
            Str_Strcat(hostinfoCachedDetailedData,
                       DETAILED_DATA_DELIMITER,
                       sizeof hostinfoCachedDetailedData);
         }

         /* Escape single quotes and back slashes in the value. */
         for (c = field->value; *c != '\0'; c++) {
            if (*c == '\'' || *c == '\\') {
               escapedString[i++] = '\\';
            }

            escapedString[i++] = *c;
         }

         escapedString[i] = '\0';

         /* No trailing spaces */
         while (--i >= 0 && isspace(escapedString[i])) {
            escapedString[i] = '\0';
         }

         len = Str_Snprintf(fieldString, sizeof fieldString, "%s='%s'",
                            field->name, escapedString);
         if (len == -1) {
            Warning("%s: Error: detailed data field too large\n",
                    __FUNCTION__);
            memset(hostinfoCachedDetailedData, '\0',
                   sizeof hostinfoCachedDetailedData);
            return;
         }

         Str_Strcat(hostinfoCachedDetailedData, fieldString,
                    sizeof hostinfoCachedDetailedData);

         first = FALSE;
      }
   }
}


#if defined(__APPLE__) // MacOS
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoMacOS --
 *
 *      Determine the specifics concerning MacOS.
 *
 * Return value:
 *      Returns TRUE on success and FALSE on failure.
 *
 * Side effects:
 *      Cache values are set when returning TRUE.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoMacOS(struct utsname *buf)  // IN:
{
   int len;
   unsigned int i;
   char *productName;
   char *productVersion;
   char *productBuildVersion;
   char osName[MAX_OS_NAME_LEN];
   char osNameFull[MAX_OS_FULLNAME_LEN];
   Bool haveVersion = FALSE;
   static char const *versionPlists[] = {
      "/System/Library/CoreServices/ServerVersion.plist",
      "/System/Library/CoreServices/SystemVersion.plist"
   };

   /*
    * Read the version info from ServerVersion.plist or SystemVersion.plist.
    * Mac OS Server (10.6 and earlier) has both files, and the product name in
    * ServerVersion.plist is "Mac OS X Server". Client versions of Mac OS only
    * have SystemVersion.plist with the name "Mac OS X".
    *
    * This is better than executing system_profiler or sw_vers, or using the
    * deprecated Gestalt() function (which only gets version numbers).
    * All of those methods just read the same plist files anyway.
    */

   for (i = 0; !haveVersion && i < ARRAYSIZE(versionPlists); i++) {
      CFDictionaryRef versionDict =
         UtilMacos_CreateCFDictionaryWithContentsOfFile(versionPlists[i]);
      if (versionDict != NULL) {
         haveVersion = UtilMacos_ReadSystemVersion(versionDict,
                                                   &productName,
                                                   &productVersion,
                                                   &productBuildVersion);
         CFRelease(versionDict);
      }
   }

   Str_Strcpy(detailedDataFields[DISTRO_NAME].value, productName,
              sizeof detailedDataFields[DISTRO_NAME].value);
   Str_Strcpy(detailedDataFields[DISTRO_VERSION].value, productVersion,
              sizeof detailedDataFields[DISTRO_VERSION].value);
   Str_Strcpy(detailedDataFields[BUILD_NUMBER].value, productBuildVersion,
              sizeof detailedDataFields[BUILD_NUMBER].value);

   if (haveVersion) {
      len = Str_Snprintf(osNameFull, sizeof osNameFull,
                         "%s %s (%s) %s %s", productName, productVersion,
                         productBuildVersion, buf->sysname, buf->release);

      free(productName);
      free(productVersion);
      free(productBuildVersion);
   } else {
      Log("%s: Failed to read system version plist.\n", __FUNCTION__);

      len = Str_Snprintf(osNameFull, sizeof osNameFull, "%s %s", buf->sysname,
                         buf->release);
   }

   if (len != -1) {
      if (Hostinfo_GetSystemBitness() == 64) {
         len = Str_Snprintf(osName, sizeof osName, "%s%d%s", STR_OS_MACOS,
                            Hostinfo_OSVersion(0), STR_OS_64BIT_SUFFIX);
      } else {
         len = Str_Snprintf(osName, sizeof osName, "%s%d", STR_OS_MACOS,
                            Hostinfo_OSVersion(0));
      }
   }

   if (len == -1) {
      Warning("%s: Error: buffer too small\n", __FUNCTION__);
   } else {
      HostinfoPostData(osName, osNameFull);
   }

   return (len != -1);
}
#endif


#if defined(VMX86_SERVER) || defined(USERWORLD)  // ESXi
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoESX --
 *
 *      Determine the specifics concerning ESXi.
 *
 * Return value:
 *      TRUE   Success
 *      FALSE  Failure
 *
 * Side effects:
 *      Cache values are set when returning TRUE.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoESX(struct utsname *buf)  // IN:
{
   int len;
   uint32 major;
   uint32 minor;
   char osName[MAX_OS_NAME_LEN];
   char osNameFull[MAX_OS_FULLNAME_LEN];

   if (sscanf(buf->release, "%u.%u", &major, &minor) != 2) {
      if (sscanf(buf->release, "%u", &major) != 1) {
         major = 0;
      }

      minor = 0;
   }

   if (major <= 4) {
      Str_Strcpy(osName, STR_OS_VMKERNEL, sizeof osName);
   } else {
      if (minor == 0) {
         Str_Sprintf(osName, sizeof osName, "%s%d", STR_OS_VMKERNEL,
                     major);
      } else {
         Str_Sprintf(osName, sizeof osName, "%s%d%d", STR_OS_VMKERNEL,
                     major, minor);
      }
   }

   len = Str_Snprintf(osNameFull, sizeof osNameFull, "VMware ESXi %s",
                      buf->release);

   if (len == -1) {
      Warning("%s: Error: buffer too small\n", __FUNCTION__);
   } else {
      HostinfoPostData(osName, osNameFull);
   }

   return (len != -1);
}
#endif


#if !defined(__APPLE__) && !defined(VMX86_SERVER) && !defined(USERWORLD)

typedef struct ShortNameSet {
   const char   *pattern;
   const char   *shortName;
   Bool        (*setFunc)(const struct ShortNameSet *entry, // IN:
                          int version,                      // IN:
                          const char *distroLower,          // IN:
                          char *distroShort,                // OUT:
                          int distroShortSize);             // IN:
} ShortNameSet;


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSearchShortNames --
 *
 *      Search a generic ShortNameSet table.
 *      If a match is found execute the entry's setFunc and
 *      return the result.
 *
 * Return value:
 *      TRUE    success; a match was found
 *      FALSE   failure; no match was found
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSearchShortNames(const ShortNameSet *array, // IN:
                         int  version,              // IN:
                         const char *distroLower,   // IN:
                         char *distroShort,         // OUT:
                         int distroShortSize)       // IN:
{
   const ShortNameSet *p = array;

   ASSERT(p != NULL);

   while (p->pattern != NULL) {
      ASSERT(p->setFunc != NULL);

      if (strstr(distroLower, p->pattern) != NULL) {
         return (*p->setFunc)(p, version, distroLower, distroShort,
                              distroShortSize);
      }

      p++;
   }

   return FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoArchString --
 *
 *      Return the machine architecture prefix. The X86 and X86_64 machine
 *      architectures are implied - no prefix. All others require an official
 *      string from VMware.
 *
 * Return value:
 *      As above.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static const char *
HostinfoArchString(void)
{
#if defined(VM_ARM_ANY)
   return STR_OS_ARM_PREFIX;
#elif defined(VM_X86_ANY)
   return "";
#else
#error Unsupported architecture!
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGenericSetShortName --
 *
 *      Set the short name using the short name entry in the specified table
 *      entry.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoGenericSetShortName(const ShortNameSet *entry, // IN:
                            int version,               // IN:
                            const char *distroLower,   // IN:
                            char *distroShort,         // OUT:
                            int distroShortSize)       // IN:
{
   ASSERT(entry != NULL);
   ASSERT(entry->shortName != NULL);

   Str_Sprintf(distroShort, distroShortSize, "%s%s", HostinfoArchString(),
               entry->shortName);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetAmazonShortName --
 *
 *      Set the short name for the Amazon distro.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetAmazonShortName(const ShortNameSet *entry, // IN: Unused
                           int version,               // IN:
                           const char *distroLower,   // IN: Unused
                           char *distroShort,         // OUT:
                           int distroShortSize)       // IN:
{
   if (version < 2) {
      version = 2;
   }

   Str_Sprintf(distroShort, distroShortSize, "%s%s%d",
               HostinfoArchString(), STR_OS_AMAZON_LINUX, version);

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetAsianuxShortName --
 *
 *      Set short name for the Asianux distro.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetAsianuxShortName(const ShortNameSet *entry, // IN: Unused
                            int version,               // IN:
                            const char *distroLower,   // IN: Unused
                            char *distroShort,         // OUT:
                            int distroShortSize)       // IN:
{
   if (version < 3) {
      Str_Strcpy(distroShort, STR_OS_ASIANUX, distroShortSize);
   } else {
      Str_Sprintf(distroShort, distroShortSize, "%s%s%d",
                  HostinfoArchString(), STR_OS_ASIANUX, version);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetCentosShortName --
 *
 *      Set the short name for the CentOS distro.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetCentosShortName(const ShortNameSet *entry, // IN: Unused
                           int version,               // IN:
                           const char *distroLower,   // IN: Unused
                           char *distroShort,         // OUT:
                           int distroShortSize)       // IN:
{
   if (version < 6) {
      Str_Strcpy(distroShort, STR_OS_CENTOS, distroShortSize);
   } else {
      Str_Sprintf(distroShort, distroShortSize, "%s%s%d",
                  HostinfoArchString(), STR_OS_CENTOS, version);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetDebianShortName --
 *
 *      Set the short name for the Debian distro.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetDebianShortName(const ShortNameSet *entry, // IN: Unused
                           int version,               // IN:
                           const char *distroLower,   // IN: Unused
                           char *distroShort,         // OUT:
                           int distroShortSize)       // IN:
{
   if (version <= 4) {
      Str_Strcpy(distroShort, STR_OS_DEBIAN "4", distroShortSize);
   } else {
      Str_Sprintf(distroShort, distroShortSize, "%s%s%d",
                  HostinfoArchString(), STR_OS_DEBIAN, version);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetOracleShortName --
 *
 *      Set the short name for the Oracle distro.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetOracleShortName(const ShortNameSet *entry, // IN: Unused
                           int version,               // IN:
                           const char *distroLower,   // IN: Unused
                           char *distroShort,         // OUT:
                           int distroShortSize)       // IN:
{
   /*
    * [root@localhost ~]# lsb_release -sd
    * "Enterprise Linux Enterprise Linux Server release 5.4 (Carthage)"
    *
    * Not sure why they didn't brand their releases as "Oracle Enterprise
    * Linux". Oh well. It's fixed in 6.0, though.
    */

   if (version == 0) {
      Str_Sprintf(distroShort, distroShortSize, "%s%s",
                  HostinfoArchString(), STR_OS_ORACLE);
   } else {
      Str_Sprintf(distroShort, distroShortSize, "%s%s%d",
                  HostinfoArchString(), STR_OS_ORACLE, version);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetRedHatShortName --
 *
 *      Set short name of the RedHat distro.
 *
 * Return value:
 *      TRUE    success
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetRedHatShortName(const ShortNameSet *entry, // IN: Unused
                           int version,               // IN:
                           const char *distroLower,   // IN:
                           char *distroShort,         // OUT:
                           int distroShortSize)       // IN:
{
   if (strstr(distroLower, "enterprise") == NULL) {
      Str_Sprintf(distroShort, distroShortSize, "%s%s",
                  HostinfoArchString(), STR_OS_RED_HAT);
   } else {
      if (version == 0) {
         Str_Sprintf(distroShort, distroShortSize, "%s%s",
                     HostinfoArchString(), STR_OS_RED_HAT_EN);
      } else {
         Str_Sprintf(distroShort, distroShortSize, "%s%s%d",
                     HostinfoArchString(), STR_OS_RED_HAT_EN, version);
      }
   }

   return TRUE;
}


/*
 *      Short name subarray for the SUSE Enterprise distro.
 *
 *      Keep in sorted order (sort -d)!
 */

#define SUSE_SAP_LINUX "server for sap applications 12"

static const ShortNameSet suseEnterpriseShortNameArray[] = {
   { "desktop 10",    STR_OS_SLES "10",  HostinfoGenericSetShortName },
   { "desktop 11",    STR_OS_SLES "11",  HostinfoGenericSetShortName },
   { "desktop 12",    STR_OS_SLES "12",  HostinfoGenericSetShortName },
   { "desktop 15",    STR_OS_SLES "15",  HostinfoGenericSetShortName },
   { "desktop 16",    STR_OS_SLES "16",  HostinfoGenericSetShortName },
   { "server 10",     STR_OS_SLES "10",  HostinfoGenericSetShortName },
   { "server 11",     STR_OS_SLES "11",  HostinfoGenericSetShortName },
   { "server 12",     STR_OS_SLES "12",  HostinfoGenericSetShortName },
   { "server 15",     STR_OS_SLES "15",  HostinfoGenericSetShortName },
   { "server 16",     STR_OS_SLES "16",  HostinfoGenericSetShortName },
   { SUSE_SAP_LINUX,  STR_OS_SLES "12",  HostinfoGenericSetShortName },
   { NULL,            NULL,              NULL                        } // MUST BE LAST
};


/*
 *      Short name array for the SUSE distro.
 *
 *      Keep in sorted order (sort -d)!
 */

static const ShortNameSet suseShortNameArray[] = {
   { "sun",           STR_OS_SUN_DESK,     HostinfoGenericSetShortName },
   { "novell",        STR_OS_NOVELL "9",   HostinfoGenericSetShortName },
   { NULL,            NULL,                NULL                        } // MUST BE LAST
};


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSetSuseShortName --
 *
 *      Set the short name for the SUSE distros. Due to ownership and naming
 *      changes, other distros have to be "filtered" and named differently.
 *
 * Return value:
 *      TRUE    success
 *      FALSE   failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSetSuseShortName(const ShortNameSet *entry, // IN:
                         int version,               // IN:
                         const char *distroLower,   // IN:
                         char *distroShort,         // OUT:
                         int distroShortSize)       // IN:
{
   Bool found;

   ASSERT(entry != NULL);

   if (strstr(distroLower, "enterprise") == NULL) {
      found = HostinfoSearchShortNames(suseShortNameArray, version,
                                       distroLower, distroShort,
                                       distroShortSize);
      if (!found) {
         Str_Sprintf(distroShort, distroShortSize, "%s%s",
                     HostinfoArchString(), STR_OS_SUSE);
      }
   } else {
      found = HostinfoSearchShortNames(suseEnterpriseShortNameArray, version,
                                       distroLower, distroShort,
                                       distroShortSize);
      if (!found) {
         Str_Sprintf(distroShort, distroShortSize, "%s%s",
                     HostinfoArchString(), STR_OS_SLES);
      }
   }

   return TRUE;
}


/*
 * Table mapping from distro name to the officially recognized shortname.
 *
 * WARNING: If you are not VMware, do not change this table. Values that are
 * not recognized by the VMware host will be ignored. Any change here must
 * be accompanied by additional changes to the host.
 *
 * If you are interested in extending this table, do not send a pull request.
 * Instead, submit a request via the open-vm-tools github issue tracker
 * https://github.com/vmware/open-vm-tools/issues.
 *
 * Some distros do not have a simple substitution and special logic is
 * necessary to handle distros that do not have simple substitutions.
 *
 * Some of the special logic - functions - use a subtable.
 * If you're not VMware, do not add anything to those tables either.
 */

static const ShortNameSet shortNameArray[] = {
/* Long distro name      Short distro name          Short name set function */
{ "almalinux",           STR_OS_ALMA_LINUX,         HostinfoGenericSetShortName },
{ "amazon",              NULL,                      HostinfoSetAmazonShortName  },
{ "annvix",              STR_OS_ANNVIX,             HostinfoGenericSetShortName },
{ "arch",                STR_OS_ARCH,               HostinfoGenericSetShortName },
{ "arklinux",            STR_OS_ARKLINUX,           HostinfoGenericSetShortName },
{ "asianux",             NULL,                      HostinfoSetAsianuxShortName },
{ "aurox",               STR_OS_AUROX,              HostinfoGenericSetShortName },
{ "black cat",           STR_OS_BLACKCAT,           HostinfoGenericSetShortName },
{ "centos",              NULL,                      HostinfoSetCentosShortName  },
{ "cobalt",              STR_OS_COBALT,             HostinfoGenericSetShortName },
{ "conectiva",           STR_OS_CONECTIVA,          HostinfoGenericSetShortName },
{ "debian",              NULL,                      HostinfoSetDebianShortName  },
{ "red hat",             NULL,                      HostinfoSetRedHatShortName  },
/* Red Hat must come before the Enterprise Linux entry */
{ "enterprise linux",    NULL,                      HostinfoSetOracleShortName  },
{ "fedora",              STR_OS_FEDORA,             HostinfoGenericSetShortName },
{ "gentoo",              STR_OS_GENTOO,             HostinfoGenericSetShortName },
{ "immunix",             STR_OS_IMMUNIX,            HostinfoGenericSetShortName },
{ "linux-from-scratch",  STR_OS_LINUX_FROM_SCRATCH, HostinfoGenericSetShortName },
{ "linux-ppc",           STR_OS_LINUX_PPC,          HostinfoGenericSetShortName },
{ "mandrake",            STR_OS_MANDRAKE,           HostinfoGenericSetShortName },
{ "mandriva",            STR_OS_MANDRIVA,           HostinfoGenericSetShortName },
{ "mklinux",             STR_OS_MKLINUX,            HostinfoGenericSetShortName },
{ "opensuse",            STR_OS_OPENSUSE,           HostinfoGenericSetShortName },
{ "oracle",              NULL,                      HostinfoSetOracleShortName  },
{ "pld",                 STR_OS_PLD,                HostinfoGenericSetShortName },
{ "rocky linux",         STR_OS_ROCKY_LINUX,        HostinfoGenericSetShortName },
{ "slackware",           STR_OS_SLACKWARE,          HostinfoGenericSetShortName },
{ "sme server",          STR_OS_SMESERVER,          HostinfoGenericSetShortName },
{ "suse",                NULL,                      HostinfoSetSuseShortName    },
{ "tiny sofa",           STR_OS_TINYSOFA,           HostinfoGenericSetShortName },
{ "turbolinux",          STR_OS_TURBO,              HostinfoGenericSetShortName },
{ "ubuntu",              STR_OS_UBUNTU,             HostinfoGenericSetShortName },
{ "ultra penguin",       STR_OS_ULTRAPENGUIN,       HostinfoGenericSetShortName },
{ "united linux",        STR_OS_UNITEDLINUX,        HostinfoGenericSetShortName },
{ "va linux",            STR_OS_VALINUX,            HostinfoGenericSetShortName },
{ "vmware photon",       STR_OS_PHOTON,             HostinfoGenericSetShortName },
{ "yellow dog",          STR_OS_YELLOW_DOG,         HostinfoGenericSetShortName },
{ NULL,                  NULL,                      NULL                        } // MUST BE LAST
};


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetOSShortName --
 *
 *      Returns distro information based in .vmx format (distroShort).
 *
 * Return value:
 *
 *      True - we found the Short Name and copied it to distroShort
 *      False - we did not find the short name for the specified versionStr
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoGetOSShortName(const char *distro,     // IN: full distro name
                       const char *versionStr, // IN/OPT: distro version
                       char *distroShort,      // OUT: short distro name
                       int distroShortSize)    // IN: size of short distro name
{
   uint32 version;
   Bool found;
   char *distroLower;

   ASSERT(distro != NULL);
   ASSERT(distroShort != NULL);

   /* Come up with a distro version */

   if (versionStr == NULL) {
      const char *p = distro;

      /* The first digit in the distro string is the version */
      while (*p != '\0') {
         if (isdigit(*p)) {
            versionStr = p;
            break;
         }

         p++;
      }
   }

   if (versionStr == NULL) {
      version = 0;
   } else {
      if (sscanf(versionStr, "%u", &version) != 1) {
         version = 0;
      }
   }

   /* Normalize the distro string */
   distroLower = Str_ToLower(Util_SafeStrdup(distro));

   /* Search distroLower for a match */
   found = HostinfoSearchShortNames(shortNameArray, version, distroLower,
                                    distroShort, distroShortSize);

   free(distroLower);

   return found;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoReadDistroFile --
 *
 *      Attempt to open and read the specified distro identification file.
 *      If the file has data and can be read, attempt to identify the distro.
 *
 *      os-release rules require strict compliance. No data unless things
 *      are perfect. For the LSB, we will return the contents of the file
 *      even if things aren't strictly compliant.
 *
 * Return value:
 *     !NULL  Success. A pointer to an dynamically allocated array of pointers
 *                     to dynamically allocated strings, one for each field
 *                     corresponding to the values argument plus one which
 *                     contains a concatenation of all discovered data.
 *      NULL  Failure.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static char **
HostinfoReadDistroFile(Bool osReleaseRules,   // IN: osRelease rules
                       const char *fileName,  // IN: distro file
                       const char *values[])  // IN: search strings
{
   int i;
   int fd;
   DynBuf b;
   int bufSize;
   struct stat st;
   char lineBuf[DISTRO_BUF_SIZE];
   FILE *s = NULL;
   uint32 nArgs = 0;
   Bool any = FALSE;
   Bool success = FALSE;
   char **result = NULL;
   char *distroOrig = NULL;

   /* It's OK for the file to not exist, don't warn for this.  */
   if ((fd = Posix_Open(fileName, O_RDONLY)) == -1) {
      return FALSE;
   }

   DynBuf_Init(&b);

   if (fstat(fd, &st)) {
      Warning("%s: could not stat file '%s': %d\n", __FUNCTION__, fileName,
           errno);
      goto out;
   }

   if (st.st_size == 0) {
      Warning("%s: Cannot work with empty file.\n", __FUNCTION__);
      goto out;
   }

   bufSize = st.st_size;

   distroOrig = Util_SafeCalloc(bufSize + 1, sizeof *distroOrig);

   if (read(fd, distroOrig, bufSize) != bufSize) {
      Warning("%s: could not read file '%s': %d\n", __FUNCTION__, fileName,
              errno);
      goto out;
   }

   distroOrig[bufSize] = '\0';

   lseek(fd, 0, SEEK_SET);

   s = fdopen(fd, "r");

   if (s == NULL) {
      Warning("%s: fdopen conversion failed.\n", __FUNCTION__);
      goto out;
   }

   /*
    * Attempt to parse a file with one name=value pair per line. Values are
    * expected to be embedded in double quotes.
    */

   nArgs = 0;
   for (i = 0; values[i] != NULL; i++) {
      nArgs++;
   }
   nArgs++;  // For the appended version of the data

   result = Util_SafeCalloc(nArgs, sizeof(char *));

   while (fgets(lineBuf, sizeof lineBuf, s) != NULL) {
      for (i = 0; values[i] != NULL; i++) {
          size_t len = strlen(values[i]);

          if (strncmp(lineBuf, values[i], len) == 0) {
             char *p;
             char *data;

             if (lineBuf[len] == '"') {
                data = &lineBuf[len + 1];
                p = strrchr(data, '"');

                if (p == NULL) {
                   Warning("%s: Invalid os-release file.", __FUNCTION__);
                   goto out;
                }
             } else {
                data = &lineBuf[len];

                p = strchr(data, '\n');

                if (p == NULL) {
                   Warning("%s: os-release file line too long.",
                           __FUNCTION__);
                   goto out;
                }
             }

             *p = '\0';

             if (p >= &data[MAX_DETAILED_FIELD_LEN]) {
                Warning("%s: Unexpectedly long data encountered; truncated.",
                        __FUNCTION__);

                data[MAX_DETAILED_FIELD_LEN - 1] = '\0';
             }

             if (any) {
                DynBuf_Strcat(&b, " ");
             }

             DynBuf_Strcat(&b, data);
             result[i] = Util_SafeStrdup(data);

             any = TRUE;
          }
      }
   }

   if (ferror(s)) {
       Warning("%s: Error occurred while reading '%s'\n", __FUNCTION__,
               fileName);

       goto out;
   }

   if (DynBuf_GetSize(&b) == 0) {
      /*
       * The distro identification file was not standards compliant.
       */

      if (osReleaseRules) {
         /*
          * We must strictly comply with the os-release standard. Error.
          */

         success = FALSE;
      } else {
         /*
          * Our old code played fast and loose with the LSB standard. If there
          * was a distro identification file but the contents were not LSB
          * compliant (e.g. RH 7.2), we returned success along with the
          * contents "as is"... in the hopes that the available data would
          * be "good enough". Continue the practice to maximize compatibility.
          */

         DynBuf_Strcat(&b, distroOrig);
         DynBuf_Append(&b, "\0", 1);  // Terminate the string

         success = TRUE;
      }
   } else {
      DynBuf_Append(&b, "\0", 1);  // Terminate the string

      success = TRUE;
   }

out:
   if (s != NULL) {
      fclose(s);
   } else if (fd != -1) {
      close(fd);
   }

   free(distroOrig);

   if (success) {
      result[nArgs - 1] = DynBuf_Detach(&b);
   } else {
      Util_FreeStringList(result, nArgs);
      result = NULL;
   }

   DynBuf_Destroy(&b);

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HostinfoGetCmdOutput --
 *
 *      Run a cmd & get its cmd line output
 *
 * Results:
 *      An allocated string or NULL if an error occurred.
 *
 * Side effects:
 *	The cmd is run.
 *
 *----------------------------------------------------------------------
 */

static char *
HostinfoGetCmdOutput(const char *cmd)  // IN:
{
   Bool isSuperUser = FALSE;
   DynBuf db;
   FILE *stream;
   char *out = NULL;

   /*
    * Attempt to lower privs, because we use popen and an attacker
    * may control $PATH.
    */
   if (vmx86_linux && Id_IsSuperUser()) {
      Id_EndSuperUser(getuid());
      isSuperUser = TRUE;
   }

   DynBuf_Init(&db);

   stream = Posix_Popen(cmd, "r");
   if (stream == NULL) {
      Warning("Unable to get output of command \"%s\"\n", cmd);

      goto exit;
   }

   for (;;) {
      char *line = NULL;
      size_t size;

      switch (StdIO_ReadNextLine(stream, &line, 0, &size)) {
      case StdIO_Error:
         goto closeIt;
         break;

      case StdIO_EOF:
         break;

      case StdIO_Success:
         break;

      default:
         NOT_IMPLEMENTED();
      }

      if (line == NULL) {
         break;
      }

      /* size does not include the NUL terminator. */
      DynBuf_Append(&db, line, size);
      free(line);
   }

   /* Return NULL instead of an empty string if there's no output. */
   if (DynBuf_Get(&db) != NULL) {
      out = DynBuf_DetachString(&db);
   }

 closeIt:
   pclose(stream);

 exit:
   DynBuf_Destroy(&db);

   if (isSuperUser) {
      Id_BeginSuperUser();
   }

   return out;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoOsRelease --
 *
 *      Attempt to return the distro identification data we're interested in
 *      from the os-release standard file(s). Look for the os-release data by
 *      following the priority order established by the os-release standard.
 *
 *      The fields of interest are found in osReleaseFields above.
 *
 *      https://www.linux.org/docs/man5/os-release.html
 *
 * Return value:
 *      -1     Failure. No data returned.
 *      0..n   Success. A "score", the number of interesting pieces of data
 *             found. The strings found, in the order specified by the
 *             search table above, are returned in args as an array of pointers
 *             to dynamically allocated strings.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static int
HostinfoOsRelease(char ***args)  // OUT:
{
   int score;

   *args = HostinfoReadDistroFile(TRUE, "/etc/os-release", osReleaseFields);

   if (*args == NULL) {
      *args = HostinfoReadDistroFile(TRUE, "/usr/lib/os-release",
                                     osReleaseFields);
   }

   if (*args == NULL) {
      score = -1;
   } else {
      uint32 i;
      size_t fields = ARRAYSIZE(osReleaseFields) - 1;  // Exclude terminator

      score = 0;

      for (i = 0; i < fields; i++) {
         if ((*args)[i] != NULL) {
            score++;
         }
      }
   }

   return score;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoLsbRemoveQuotes --
 *
 *      If present, removes 1 set of double quotes around a LSB output.
 *
 * Return value:
 *      Pointer to a substring of the input
 *
 * Side effects:
 *      Replaces second double quote with a NUL character.
 *
 *-----------------------------------------------------------------------------
 */

static char *
HostinfoLsbRemoveQuotes(char *lsbOutput)  // IN/OUT:
{
   char *lsbStart = lsbOutput;

   ASSERT(lsbStart != NULL);

   if (lsbStart[0] == '"') {
      char *quoteEnd = strchr(++lsbStart, '"');

      if (quoteEnd != NULL) {
         *quoteEnd = '\0';
      }
   }

   return lsbStart;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoLsb --
 *
 *      Attempt to return the distro identification data we're interested in
 *      from the LSB standard file.
 *
 *      The fields of interest are found in lsbFields above.
 *
 *      https://refspecs.linuxfoundation.org/lsb.shtml
 *
 * Return value:
 *      -1     Failure. No data returned.
 *      0..n   Success. A "score", the number of interesting pieces of data
 *             found. The strings found, in the order specified by the
 *             search table above, are returned in args as an array of pointers
 *             to dynamically allocated strings.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static int
HostinfoLsb(char ***args)  // OUT:
{
   uint32 i;
   int score;
   char *lsbOutput;
   size_t fields = ARRAYSIZE(lsbFields) - 1;  // Exclude terminator

   /*
    * Try to get OS detailed information from the lsb_release command.
    */

   lsbOutput = HostinfoGetCmdOutput("/usr/bin/lsb_release -sd 2>/dev/null");

   if (lsbOutput == NULL) {
      /*
       * Try to get more detailed information from the version file.
       */

      for (i = 0; distroArray[i].filename != NULL; i++) {
         *args = HostinfoReadDistroFile(FALSE, distroArray[i].filename,
                                        lsbFields);

         if (*args != NULL) {
            break;
         }
      }
   } else {
      *args = Util_SafeCalloc(fields + 1, sizeof(char *));

      /* LSB Description (pretty name) */
      (*args)[fields] = Util_SafeStrdup(HostinfoLsbRemoveQuotes(lsbOutput));
      free(lsbOutput);

      /* LSB Distributor */
      lsbOutput = HostinfoGetCmdOutput("/usr/bin/lsb_release -si 2>/dev/null");
      if (lsbOutput != NULL) {
         (*args)[0] = Util_SafeStrdup(HostinfoLsbRemoveQuotes(lsbOutput));
         free(lsbOutput);
      }

      /* LSB Release */
      lsbOutput = HostinfoGetCmdOutput("/usr/bin/lsb_release -sr 2>/dev/null");
      if (lsbOutput != NULL) {
         (*args)[1] = Util_SafeStrdup(HostinfoLsbRemoveQuotes(lsbOutput));
         free(lsbOutput);
      }

      /* LSB Description */
      (*args)[3] = Util_SafeStrdup((*args)[fields]);
   }

   if (*args == NULL) {
      score = -1;
   } else {
      score = 0;

      for (i = 0; i < fields; i++) {
         if ((*args)[i] != NULL) {
            score++;
         }
      }
   }


   return score;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoDefaultLinux --
 *
 *      Build and return generic data about the Linux disto. Only return what
 *      has been required - short description (i.e. guestOS string), long
 *      description (nice looking string).
 *
 * Return value:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
HostinfoDefaultLinux(char *distro,            // OUT/OPT:
                     size_t distroSize,       // IN:
                     char *distroShort,       // OUT/OPT:
                     size_t distroShortSize)  // IN:
{
   char generic[128];
   const char *distroOut = NULL;
   const char *distroShortOut = NULL;
   int majorVersion = Hostinfo_OSVersion(0);
   int minorVersion = Hostinfo_OSVersion(1);

   switch (majorVersion) {
   case 1:
      distroOut = STR_OS_OTHER_FULL;
      distroShortOut = STR_OS_OTHER;
      break;

   case 2:
      if (minorVersion < 4) {
         distroOut = STR_OS_OTHER_FULL;
         distroShortOut = STR_OS_OTHER;
      } else if (minorVersion < 6) {
         distroOut = STR_OS_OTHER_24_FULL;
         distroShortOut = STR_OS_OTHER_24;
      } else {
         distroOut = STR_OS_OTHER_26_FULL;
         distroShortOut = STR_OS_OTHER_26;
      }

      break;

   case 3:
      distroOut = STR_OS_OTHER_3X_FULL;
      distroShortOut = STR_OS_OTHER_3X;
      break;

   case 4:
      distroOut = STR_OS_OTHER_4X_FULL;
      distroShortOut = STR_OS_OTHER_4X;
      break;

   case 5:
      distroOut = STR_OS_OTHER_5X_FULL;
      distroShortOut = STR_OS_OTHER_5X;
      break;

   case 6:
      distroOut = STR_OS_OTHER_6X_FULL;
      distroShortOut = STR_OS_OTHER_6X;
      break;

   default:
      /*
       * Anything newer than this code explicitly handles returns the
       * "highest" known short description and a dynamically created,
       * appropriate long description.
       */

      Str_Sprintf(generic, sizeof generic, "Other Linux %d.%d kernel",
                  majorVersion, minorVersion);
      distroOut = generic;
      distroShortOut = STR_OS_OTHER_5X;
   }

   if (distro != NULL) {
      ASSERT(distroOut != NULL);
      Str_Strcpy(distro, distroOut, distroSize);
   }

   if (distroShort != NULL) {
      ASSERT(distroShortOut != NULL);
      Str_Strcpy(distroShort, distroShortOut, distroShortSize);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoBestScore --
 *
 *      Return the best distro and distroShort data possible. Do this by
 *      examining the LSB and os-release data and choosing the "best fit".
 *      The "best fit" is determined inspecting the available information
 *      returned by distro identification methods. If none is available,
 *      return a safe, generic result. Otherwise, use the method that has
 *      the highest score (of valid, useful data).
 *
 * Return value:
 *      None
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
HostinfoBestScore(char *distro,            // OUT:
                  size_t distroSize,       // IN:
                  char *distroShort,       // OUT:
                  size_t distroShortSize)  // IN:
{
   char **lsbData = NULL;
   char **osReleaseData = NULL;
   int lsbScore = HostinfoLsb(&lsbData);
   int osReleaseScore = HostinfoOsRelease(&osReleaseData);

   /*
    * Now that the os-release standard is long stable, choose it over the LSB
    * standard, all things being the same.
    */

   if ((lsbScore > 0) && (lsbScore > osReleaseScore)) {
      size_t fields = ARRAYSIZE(lsbFields) - 1;  // Exclude terminator

      if (lsbData[0] != NULL) {  // Name
         Str_Strcpy(detailedDataFields[DISTRO_NAME].value, lsbData[0],
                    sizeof detailedDataFields[DISTRO_NAME].value);
      }

      if (lsbData[1] != NULL) {  // Release
         Str_Strcpy(detailedDataFields[DISTRO_VERSION].value, lsbData[1],
                    sizeof detailedDataFields[DISTRO_VERSION].value);
      }

      if (lsbData[3] != NULL) {  // Description
         Str_Strcpy(detailedDataFields[PRETTY_NAME].value, lsbData[3],
                    sizeof detailedDataFields[PRETTY_NAME].value);
      }

      if (lsbData[fields] != NULL) {
         Str_Strcpy(distro, lsbData[fields], distroSize);
      }

      /* If this isn't a recognized distro, specify a default. */
      if (!HostinfoGetOSShortName(distro, lsbData[1], distroShort,
                                  distroShortSize)) {
         HostinfoDefaultLinux(NULL, 0, distroShort, distroShortSize);
      }

      goto bail;
   }

   if (osReleaseScore > 0) {
      size_t fields = ARRAYSIZE(osReleaseFields) - 1;  // Exclude terminator

      if (osReleaseData[0] != NULL) {
         Str_Strcpy(detailedDataFields[PRETTY_NAME].value, osReleaseData[0],
                    sizeof detailedDataFields[PRETTY_NAME].value);
      }

      if (osReleaseData[1] != NULL) {
         Str_Strcpy(detailedDataFields[DISTRO_NAME].value, osReleaseData[1],
                    sizeof detailedDataFields[DISTRO_NAME].value);
      }

      if (osReleaseData[2] != NULL) {
         Str_Strcpy(detailedDataFields[DISTRO_VERSION].value, osReleaseData[2],
                    sizeof detailedDataFields[DISTRO_VERSION].value);
      }

      if (osReleaseData[3] != NULL) {
         Str_Strcpy(detailedDataFields[BUILD_NUMBER].value, osReleaseData[3],
                    sizeof detailedDataFields[BUILD_NUMBER].value);
      }

      if (osReleaseData[4] != NULL) {
         Str_Strcpy(detailedDataFields[DISTRO_ADDL_VERSION].value,
                    osReleaseData[4],
                    sizeof detailedDataFields[DISTRO_ADDL_VERSION].value);
      }

      if (osReleaseData[5] != NULL) {
         Str_Strcpy(detailedDataFields[CPE_STRING].value, osReleaseData[5],
                    sizeof detailedDataFields[CPE_STRING].value);
      }

      if (osReleaseData[fields] != NULL) {
         Str_Strcpy(distro, osReleaseData[fields], distroSize);
      }

      /* If this isn't a recognized distro, specify a default. */
      if (!HostinfoGetOSShortName(distro, osReleaseData[2],
                                  distroShort, distroShortSize)){
         HostinfoDefaultLinux(NULL, 0, distroShort, distroShortSize);
      }

      goto bail;
   }

   /* Not LSB or os-release compliant. Report something generic. */
   HostinfoDefaultLinux(distro, distroSize, distroShort, distroShortSize);

bail:

   if (lsbData != NULL) {
      Util_FreeStringList(lsbData, ARRAYSIZE(lsbFields));
   }

   if (osReleaseData != NULL) {
      Util_FreeStringList(osReleaseData, ARRAYSIZE(osReleaseFields));
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoLinux --
 *
 *      Determine the specifics concerning Linux.
 *
 * Return value:
 *      TRUE   Success
 *      FALSE  Failure
 *
 * Side effects:
 *      Cache values are set when returning TRUE
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoLinux(struct utsname *buf)  // IN:
{
   int len;
   char distro[DISTRO_BUF_SIZE];
   char distroShort[DISTRO_BUF_SIZE];
   char osName[MAX_OS_NAME_LEN];
   char osNameFull[MAX_OS_FULLNAME_LEN];

   HostinfoBestScore(distro, sizeof distro, distroShort, sizeof distroShort);

   len = Str_Snprintf(osNameFull, sizeof osNameFull, "%s %s %s", buf->sysname,
                      buf->release, distro);

   if (len != -1) {
      if (Hostinfo_GetSystemBitness() == 64) {
         len = Str_Snprintf(osName, sizeof osName, "%s%s", distroShort,
                            STR_OS_64BIT_SUFFIX);
      } else {
         len = Str_Snprintf(osName, sizeof osName, "%s", distroShort);
      }
   }

   if (len == -1) {
      Warning("%s: Error: buffer too small\n", __FUNCTION__);
   } else {
      HostinfoPostData(osName, osNameFull);
   }

   return (len != -1);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoBSD --
 *
 *      Determine the specifics concerning BSD.
 *
 * Return value:
 *      TRUE   Success
 *      FALSE  Failure
 *
 * Side effects:
 *      Cache values are set when returning TRUE
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoBSD(struct utsname *buf)  // IN:
{
   int len;
   int majorVersion;
   char distroShort[DISTRO_BUF_SIZE];
   char osName[MAX_OS_NAME_LEN];
   char osNameFull[MAX_OS_FULLNAME_LEN];

   /*
    * FreeBSD releases report their version as "x.y-RELEASE".
    */

   majorVersion = Hostinfo_OSVersion(0);

   /*
    * FreeBSD 11 and later are identified using a different guest ID than
    * older FreeBSD.
    */

   if (majorVersion < 11) {
      Str_Strcpy(distroShort, STR_OS_FREEBSD, sizeof distroShort);
   } else {
      Str_Sprintf(distroShort, sizeof distroShort, "%s%s%d",
                  HostinfoArchString(), STR_OS_FREEBSD, majorVersion);
   }

   len = Str_Snprintf(osNameFull, sizeof osNameFull, "%s %s", buf->sysname,
                      buf->release);

   if (len != -1) {
      if (Hostinfo_GetSystemBitness() == 64) {
         len = Str_Snprintf(osName, sizeof osName, "%s%s", distroShort,
                            STR_OS_64BIT_SUFFIX);
      } else {
         len = Str_Snprintf(osName, sizeof osName, "%s", distroShort);
      }
   }

   if (len == -1) {
      Warning("%s: Error: buffer too small\n", __FUNCTION__);
   } else {
      HostinfoPostData(osName, osNameFull);
   }

   return (len != -1);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSun --
 *
 *      Determine the specifics concerning Sun.
 *
 * Return value:
 *      TRUE   Success
 *      FALSE  Failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSun(struct utsname *buf)  // IN:
{
   int len;
   char osName[MAX_OS_NAME_LEN];
   char osNameFull[MAX_OS_FULLNAME_LEN];
   char solarisRelease[3] = "";

   /*
    * Solaris releases report their version as "x.y". For our supported
    * releases it seems that x is always "5", and is ignored in favor of "y"
    * for the version number.
    */

   if (sscanf(buf->release, "5.%2[0-9]", solarisRelease) != 1) {
      return FALSE;
   }

   len = Str_Snprintf(osNameFull, sizeof osNameFull, "%s %s", buf->sysname,
                      buf->release);

   if (len != -1) {
      if (Hostinfo_GetSystemBitness() == 64) {
         len = Str_Snprintf(osName, sizeof osName, "%s%s%s", STR_OS_SOLARIS,
                            solarisRelease, STR_OS_64BIT_SUFFIX);
      } else {
         len = Str_Snprintf(osName, sizeof osName, "%s%s", STR_OS_SOLARIS,
                            solarisRelease);
      }
   }

   if (len == -1) {
      Warning("%s: Error: buffer too small\n", __FUNCTION__);
   } else {
      HostinfoPostData(osName, osNameFull);
   }

   return (len != -1);
}
#endif // !defined(__APPLE__) && !defined(VMX86_SERVER) && !defined(USERWORLD)


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoOSData --
 *
 *      Determine the OS short (.vmx format) and long names.
 *
 * Return value:
 *      TRUE   Success
 *      FALSE  Failure
 *
 * Side effects:
 *      Cache values are set when returning TRUE.
 *
 *-----------------------------------------------------------------------------
 */

Bool
HostinfoOSData(void)
{
   Bool success;
   struct utsname buf;
   const char *bitness;

   /*
    * Use uname to get complete OS information.
    */

   if (uname(&buf) < 0) {
      Warning("%s: uname failed %d\n", __FUNCTION__, errno);

      return FALSE;
   }

   Str_Strcpy(detailedDataFields[FAMILY_NAME].value, buf.sysname,
              sizeof detailedDataFields[FAMILY_NAME].value);
   Str_Strcpy(detailedDataFields[KERNEL_VERSION].value, buf.release,
              sizeof detailedDataFields[KERNEL_VERSION].value);
   /* Default distro name is set to uname's sysname field */
   Str_Strcpy(detailedDataFields[DISTRO_NAME].value, buf.sysname,
              sizeof detailedDataFields[DISTRO_NAME].value);

#if defined(VMX86_SERVER) || defined(USERWORLD)  // ESXi
   bitness = "64";
#else
   bitness = (Hostinfo_GetSystemBitness() == 64) ? "64" : "32";
#endif
   Str_Strcpy(detailedDataFields[BITNESS].value, bitness,
              sizeof detailedDataFields[BITNESS].value);

#if defined(VMX86_SERVER) || defined(USERWORLD)  // ESXi
   success = HostinfoESX(&buf);
#elif defined(__APPLE__) // MacOS
   success = HostinfoMacOS(&buf);
#else
   if (strstr(buf.sysname, "Linux")) {
      success = HostinfoLinux(&buf);
   } else if (strstr(buf.sysname, "FreeBSD")) {
      success = HostinfoBSD(&buf);
   } else if (strstr(buf.sysname, "SunOS")) {
      success = HostinfoSun(&buf);
   } else {
      success = FALSE;  // Unknown to us
   }
#endif

   /* Build detailed data */
   HostinfoOSDetailedData();

   return success;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_NumCPUs --
 *
 *      Get the number of logical CPUs on the host.  If the CPUs are
 *      hyperthread-capable, this number may be larger than the number of
 *      physical CPUs.  For example, if the host has four hyperthreaded
 *      physical CPUs with 2 logical CPUs apiece, this function returns 8.
 *
 *      This function returns the number of CPUs that the host presents to
 *      applications, which is what we want in the vast majority of cases.  We
 *      would only ever care about the number of physical CPUs for licensing
 *      purposes.
 *
 * Results:
 *      On success, the number of CPUs (> 0) the host tells us we have.
 *      On failure, 0xFFFFFFFF (-1).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

uint32
Hostinfo_NumCPUs(void)
{
#if defined(sun)
   static int count = 0;

   if (count <= 0) {
      count = sysconf(_SC_NPROCESSORS_CONF);
   }

   return count;
#elif defined(__APPLE__)
   uint32 out;
   size_t outSize = sizeof out;

   /*
    * Quoting sys/sysctl.h:
    * "
    * These are the support HW selectors for sysctlbyname.  Parameters that
    * are byte counts or frequencies are 64 bit numbers. All other parameters
    * are 32 bit numbers.
    * ...
    * hw.activecpu - The number of processors currently available for executing
    *                threads. Use this number to determine the number threads
    *                to create in SMP aware applications. This number can
    *                change when power management modes are changed.
    * "
    *
    * Apparently the only way to retrieve this info is by name, and I have
    * verified the info changes when you dynamically switch a CPU
    * offline/online. --hpreg
    */

   if (sysctlbyname("hw.activecpu", &out, &outSize, NULL, 0) == -1) {
      return -1;
   }

   return out;
#elif defined(__FreeBSD__)
   uint32 out;
   size_t outSize = sizeof out;

#if __FreeBSD__version >= 500019
   if (sysctlbyname("kern.smp.cpus", &out, &outSize, NULL, 0) == -1) {
      return -1;
   }
#else
   if (sysctlbyname("machdep.smp_cpus", &out, &outSize, NULL, 0) == -1) {
      if (errno == ENOENT) {
         out = 1;
      } else {
         return -1;
      }
   }
#endif

   return out;
#elif defined(__HAIKU__)
   static int count = 0;

   if (count <= 0) {
      system_info info;

      get_system_info(&info);
      count = info.cpu_count;
   }

   return count;
#else
   static int count = 0;

   if (count <= 0) {
      FILE *f;
      char *line;

#if defined(VMX86_SERVER)
      if (HostType_OSIsVMK()) {
         VMK_ReturnStatus status = VMKernel_GetNumCPUsUsed(&count);

         if (status != VMK_OK) {
            count = 0;

            return -1;
         }

         return count;
      }
#endif
      f = Posix_Fopen("/proc/cpuinfo", "r");
      if (f == NULL) {
	 return -1;
      }

      while (StdIO_ReadNextLine(f, &line, 0, NULL) == StdIO_Success) {
	 if (strncmp(line, "processor", strlen("processor")) == 0) {
	    count++;
	 }
	 free(line);
      }

      fclose(f);

      if (count == 0) {
	 return -1;
      }
   }

   return count;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_NameGet --
 *
 *      Return the fully qualified host name of the host.
 *      Thread-safe. --hpreg
 *
 * Results:
 *      The (memorized) name on success
 *      NULL on failure
 *
 * Side effects:
 *      A host name resolution can occur.
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_NameGet(void)
{
   char *result;

   static Atomic_Ptr state; /* Implicitly initialized to NULL. --hpreg */

   result = Atomic_ReadPtr(&state);

   if (UNLIKELY(result == NULL)) {
      char *before;

      result = Hostinfo_HostName();

      before = Atomic_ReadIfEqualWritePtr(&state, NULL, result);

      if (before) {
         free(result);

         result = before;
      }
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetUser --
 *
 *      Return current user name, or NULL if can't tell.
 *      XXX Not thread-safe (somebody could do a setenv()). --hpreg
 *
 * Results:
 *      User name.  Must be free()d by caller.
 *
 * Side effects:
 *	No.
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_GetUser(void)
{
   char buffer[BUFSIZ];
   struct passwd pw;
   struct passwd *ppw = &pw;
   char *env = NULL;
   char *name = NULL;

   if ((Posix_Getpwuid_r(getuid(), &pw, buffer, sizeof buffer, &ppw) == 0) &&
       (ppw != NULL)) {
      if (ppw->pw_name) {
         name = Unicode_Duplicate(ppw->pw_name);
      }
   }

   if (!name) {
      env = Posix_Getenv("USER");

      if (env) {
         name = Unicode_Duplicate(env);
      }
   }

   return name;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetLoadAverage --
 *
 *      Returns system average load.
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoGetLoadAverage(float *avg0,  // IN/OUT:
                       float *avg1,  // IN/OUT:
                       float *avg2)  // IN/OUT:
{
#if !defined(NO_GETLOADAVG) && (defined(__linux__) && !defined(__UCLIBC__)) || defined(__APPLE__)
   double avg[3];
   int res;

   res = getloadavg(avg, 3);
   if (res < 3) {
      NOT_TESTED_ONCE();

      return FALSE;
   }

   if (avg0) {
      *avg0 = (float) avg[0];
   }
   if (avg1) {
      *avg1 = (float) avg[1];
   }
   if (avg2) {
      *avg2 = (float) avg[2];
   }

   return TRUE;
#else
   /*
    * Not implemented. This function is currently only used in the vmx, so
    * getloadavg is always available to us. If the linux tools ever need this,
    * we can go back to having a look at the output of /proc/loadavg, but
    * let's not do that now as long as it's not necessary.
    */

   NOT_IMPLEMENTED();

   return FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetLoadAverage --
 *
 *      Returns system average load * 100.
 *
 * Results:
 *      TRUE/FALSE
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetLoadAverage(uint32 *avg)  // IN/OUT:
{
   float avg0 = 0;

   if (!HostinfoGetLoadAverage(&avg0, NULL, NULL)) {
      return FALSE;
   }

   *avg = (uint32) 100 * avg0;

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_LogLoadAverage --
 *
 *      Logs system average load.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

void
Hostinfo_LogLoadAverage(void)
{
   float avg0 = 0, avg1 = 0, avg2 = 0;

   if (HostinfoGetLoadAverage(&avg0, &avg1, &avg2)) {
      Log("LOADAVG: %.2f %.2f %.2f\n", avg0, avg1, avg2);
   }
}


#if __APPLE__
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSystemTimerMach --
 *
 *      Returns system time based on a monotonic, nanosecond-resolution,
 *      fast timer provided by the Mach kernel. Requires speed conversion
 *      so is non-trivial (but lockless).
 *
 *      See also Apple TechNote QA1398.
 *
 *      NOTE: on x86, macOS does TSC->ns conversion in the commpage
 *      for mach_absolute_time() to correct for speed-stepping, so x86
 *      should always be 1:1 a.k.a. 'unity'.
 *
 *      On iOS, mach_absolute_time() uses an ARM register and always
 *      needs conversion.
 *
 * Results:
 *      Current value of timer
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static VmTimeType
HostinfoSystemTimerMach(void)
{
   static Atomic_uint64 machToNS;

   union {
      uint64 raw;
      RateConv_Ratio ratio;
   } u;
   VmTimeType result;

   u.raw = Atomic_Read64(&machToNS);

   if (UNLIKELY(u.raw == 0)) {
      mach_timebase_info_data_t timeBase;
      kern_return_t kr;

      /* Ensure atomic works correctly */
      ASSERT_ON_COMPILE(sizeof u == sizeof(machToNS));

      kr = mach_timebase_info(&timeBase);
      ASSERT(kr == KERN_SUCCESS);

      /*
       * Officially, TN QA1398 recommends using a static variable and
       *    NS = mach_absolute_time() * timeBase.numer / timebase.denom
       * (where denom != 0 is an obvious init check).
       * In practice...
       * x86 (incl x86_64) has only been seen using 1/1
       * iOS has been seen to use 125/3 (~24MHz)
       * PPC has been seen to use 1000000000/33333335 (~33MHz),
       *     which overflows 64-bit multiply in ~8 seconds (!!)
       *     (2^63 signed bits / 2^30 numer / 2^30 ns/sec ~= 2^3)
       * We will use fixed-point for everything because it's faster
       * than floating point and (with a 128-bit multiply) cannot overflow.
       * Even in the 'unity' case, the four instructions in x86_64 fixed-point
       * are about as expensive as checking for 'unity'.
       */
      if (timeBase.numer == 1 && timeBase.denom == 1) {
         u.ratio.mult = 1;  // Trivial conversion
         u.ratio.shift = 0;
      } else {
         Bool status;
         status = RateConv_ComputeRatio(timeBase.denom, timeBase.numer,
                                        &u.ratio);
         VERIFY(status);  // Assume we can get fixed-point parameters
      }

      /*
       * Use ReadWrite for implicit barrier.
       * Initialization is idempotent, so no multi-init worries.
       * The fixed-point conversions are stored in an atomic to ensure
       * they are never read "torn".
       */
      Atomic_ReadWrite64(&machToNS, u.raw);
      ASSERT(u.raw != 0);  // Used as initialization check
   }

   /* Fixed-point */
   result = Muls64x32s64(mach_absolute_time(),
                         u.ratio.mult, u.ratio.shift);

   /*
    * A smart programmer would use a global variable to ASSERT that
    * mach_absolute_time() and/or the fixed-point result is non-decreasing.
    * This turns out to be impractical: the synchronization needed to safely
    * make that check can prevent the very effect being checked.
    * Thus, we simply trust the documentation.
    */

   ASSERT(result >= 0);
   return result;
}
#endif

/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_SystemTimerNS --
 *
 *      Return the time.
 *         - These timers are documented to be non-decreasing
 *         - These timers never take locks
 *
 * NOTES:
 *      These are the routines to use when performing timing measurements.
 *
 *      The actual resolution of these "clocks" are undefined - it varies
 *      depending on hardware, OSen and OS versions.
 *
 *     *** NOTE: This function and all children must be callable
 *     while RANK_logLock is held. ***
 *
 * Results:
 *      The time in nanoseconds is returned.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

VmTimeType
Hostinfo_SystemTimerNS(void)
{
#ifdef __APPLE__
   return HostinfoSystemTimerMach();
#else
   struct timespec ts;
   int ret;

   /*
    * clock_gettime() is implemented on Linux as a commpage routine that
    * adds a known offset to TSC, which makes it very fast. Other OSes...
    * are at worst a single syscall (see: vmkernel PR820064), which still
    * makes this the best time API. Also, clock_gettime() allows nanosecond
    * resolution and any alternative is worse: gettimeofday() is microsecond.
    */
   ret = clock_gettime(CLOCK_MONOTONIC, &ts);
   ASSERT(ret == 0);

   return (VmTimeType)ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_LogMemUsage --
 *      Log system memory usage.
 *
 * Results:
 *      System memory usage is logged.
 *
 * Side effects:
 *      No.
 *
 *-----------------------------------------------------------------------------
 */

void
Hostinfo_LogMemUsage(void)
{
   int fd = Posix_Open("/proc/self/statm", O_RDONLY);

   if (fd != -1) {
      size_t len;
      char buf[64];

      len = read(fd, buf, sizeof buf);
      close(fd);

      if (len != -1) {
         int a[7] = { 0 };

         buf[len < sizeof buf ? len : sizeof buf - 1] = '\0';

         sscanf(buf, "%d %d %d %d %d %d %d",
                &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);

         Log("RUSAGE size=%d resident=%d share=%d trs=%d lrs=%d drs=%d dt=%d\n",
             a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_ResetProcessState --
 *
 *      Clean up signal handlers and file descriptors before an exec().
 *      Fds which need to be kept open can be passed as an array.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Hostinfo_ResetProcessState(const int *keepFds, // IN:
                           size_t numKeepFds)  // IN:
{
   int s, fd;
   struct sigaction sa;
   struct rlimit rlim;

   /*
    * Disable itimers before resetting the signal handlers.
    * Otherwise, the process may still receive timer signals:
    * SIGALRM, SIGVTARLM, or SIGPROF.
    */

   struct itimerval it;
   it.it_value.tv_sec = it.it_value.tv_usec = 0;
   it.it_interval.tv_sec = it.it_interval.tv_usec = 0;
   setitimer(ITIMER_REAL, &it, NULL);
   setitimer(ITIMER_VIRTUAL, &it, NULL);
   setitimer(ITIMER_PROF, &it, NULL);

   for (s = 1; s <= NSIG; s++) {
      sa.sa_handler = SIG_DFL;
      sigfillset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART;
      sigaction(s, &sa, NULL);
   }

   for (fd = (int) sysconf(_SC_OPEN_MAX) - 1; fd > STDERR_FILENO; fd--) {
      size_t i;

      for (i = 0; i < numKeepFds; i++) {
         if (fd == keepFds[i]) {
            break;
         }
      }
      if (i == numKeepFds) {
         (void) close(fd);
      }
   }

   if (getrlimit(RLIMIT_AS, &rlim) == 0) {
      rlim.rlim_cur = rlim.rlim_max;
      setrlimit(RLIMIT_AS, &rlim);
   }

#ifdef __linux__
#ifndef NO_IOPL
   /*
    * Drop iopl to its default value.
    * iopl() is not implemented in userworlds
    */
   if (!vmx86_server) {
      int err;
      uid_t euid;

      euid = Id_GetEUid();
      /* At this point, _unless we are running as root_, we shouldn't have root
         privileges --hpreg */
      ASSERT(euid != 0 || getuid() == 0);
      Id_SetEUid(0);
      err = iopl(0);
      Id_SetEUid(euid);
      VERIFY(err == 0);
   }
#endif
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_Daemonize --
 *
 *      Cross-platform daemon(3)-like wrapper.
 *
 *      Restarts the current process as a daemon, given the path to the
 *      process (usually from Hostinfo_GetModulePath).  This means:
 *
 *         * You're detached from your parent.  (Your parent doesn't
 *           need to wait for you to exit.)
 *         * Your process no longer has a controlling terminal or
 *           process group.
 *         * Your stdin/stdout/stderr fds are redirected to /dev/null. All
 *           other descriptors, except for the ones that are passed in the
 *           parameter keepFds, are closed.
 *         * Your signal handlers are reset to SIG_DFL in the daemonized
 *           process, and all the signals are unblocked.
 *         * Your main() function is called with the specified NULL-terminated
 *           argument list.
 *
 *      (Don't forget that the first string in args is argv[0] -- the
 *      name of the process).
 *
 *      Unless 'flags' contains HOSTINFO_DAEMONIZE_NOCHDIR, then the
 *      current directory of the daemon process is set to "/".
 *
 *      Unless 'flags' contains HOSTINFO_DAEMONIZE_NOCLOSE, then all stdio
 *      file descriptors of the daemon process are redirected to /dev/null.
 *      This is true even if the stdio descriptors are included in keepFds,
 *      i.e. the list of fds to be kept open.
 *
 *      If 'flags' contains HOSTINFO_DAEMONIZE_EXIT, then upon successful
 *      launch of the daemon, the original process will exit.
 *
 *      If pidPath is non-NULL, then upon success, writes the PID
 *      (as a US-ASCII string followed by a newline) of the daemon
 *      process to that path.
 *
 *      If 'flags' contains HOSTINFO_DAEMONIZE_LOCKPID and pidPath is
 *      non-NULL, then an exclusive flock(2) is taken on pidPath to prevent
 *      multiple instances of the service from running.
 *
 * Results:
 *      FALSE if the process could not be daemonized.  errno contains
 *      the error on failure.
 *      TRUE if 'flags' does not contain HOSTINFO_DAEMONIZE_EXIT and
 *      the process was daemonized.
 *      Otherwise, if the process was daemonized, this function does
 *      not return, and flow continues from your own main() function.
 *
 * Side effects:
 *      The current process is restarted with the given arguments.
 *      The process state is reset (see Hostinfo_ResetProcessState).
 *      A new session is created (so the process has no controlling terminal).
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_Daemonize(const char *path,             // IN: NUL-terminated UTF-8
                                                 // path to exec
                   char * const *args,           // IN: NULL-terminated UTF-8
                                                 // argv list
                   HostinfoDaemonizeFlags flags, // IN: flags
                   const char *pidPath,          // IN/OPT: NUL-terminated
                                                 // UTF-8 path to write PID
                   const int *keepFds,           // IN/OPT: array of fds to be
                                                 // kept open
                   size_t numKeepFds)            // IN: number of fds in
                                                 // keepFds
{
   /*
    * We use the double-fork method to make a background process whose
    * parent is init instead of the original process.
    *
    * We do this instead of calling daemon(), because daemon() is
    * deprecated on Mac OS 10.5 hosts, and calling it causes a compiler
    * warning.
    *
    * We must exec() after forking, because Mac OS library frameworks
    * depend on internal Mach ports, which are not correctly propagated
    * across fork calls.  exec'ing reinitializes the frameworks, which
    * causes them to reopen their Mach ports.
    */

   int pidPathFd = -1;
   int childPid;
   int pipeFds[2] = { -1, -1 };
   uint32 err = EINVAL;
   char *pathLocalEncoding = NULL;
   char **argsLocalEncoding = NULL;
   int *tempFds = NULL;
   size_t numTempFds = numKeepFds + 1;
   sigset_t sig;

   ASSERT_ON_COMPILE(sizeof (errno) <= sizeof err);
   ASSERT(args);
   ASSERT(path);
   ASSERT(numKeepFds == 0 || keepFds);

   if (pidPath) {
      pidPathFd = Posix_Open(pidPath, O_WRONLY | O_CREAT, 0644);
      if (pidPathFd == -1) {
         err = errno;
         Warning("%s: Couldn't open PID path [%s], error %u.\n",
                 __FUNCTION__, pidPath, err);
         errno = err;
         return FALSE;
      }

      /*
       * Lock this file to take a mutex on daemonizing this process. The child
       * will keep this file descriptor open for as long as it is running.
       *
       * flock(2) is a BSD extension (also supported on Linux) which creates a
       * lock that is inherited by the child after fork(2). fcntl(2) locks do
       * not have this property. Solaris only supports fcntl(2) locks.
       */
#ifndef sun
      if ((flags & HOSTINFO_DAEMONIZE_LOCKPID) &&
          flock(pidPathFd, LOCK_EX | LOCK_NB) == -1) {
         err = errno;
         Warning("%s: Lock held on PID path [%s], error %u, not daemonizing.\n",
                 __FUNCTION__, pidPath, err);
         errno = err;
         close(pidPathFd);
         return FALSE;
      }
#endif

      numTempFds++;
   }

   if (pipe(pipeFds) == -1) {
      err = errno;
      Warning("%s: Couldn't create pipe, error %u.\n", __FUNCTION__, err);
      pipeFds[0] = pipeFds[1] = -1;
      goto cleanup;
   }

   tempFds = malloc(sizeof tempFds[0] * numTempFds);
   if (!tempFds) {
      err = errno;
      Warning("%s: Couldn't allocate memory, error %u.\n", __FUNCTION__, err);
      goto cleanup;
   }
   if (keepFds) {
      memcpy(tempFds, keepFds, sizeof tempFds[0] * numKeepFds);
   }
   tempFds[numKeepFds++] = pipeFds[1];
   if (pidPath) {
      tempFds[numKeepFds++] = pidPathFd;
   }

   if (fcntl(pipeFds[1], F_SETFD, 1) == -1) {
      err = errno;
      Warning("%s: Couldn't set close-on-exec for fd %d, error %u.\n",
              __FUNCTION__, pipeFds[1], err);
      goto cleanup;
   }

   /* Convert the strings from UTF-8 before we fork. */
   pathLocalEncoding = Unicode_GetAllocBytes(path, STRING_ENCODING_DEFAULT);
   if (!pathLocalEncoding) {
      Warning("%s: Couldn't convert path [%s] to default encoding.\n",
              __FUNCTION__, path);
      goto cleanup;
   }

   argsLocalEncoding = Unicode_GetAllocList(args, STRING_ENCODING_DEFAULT, -1);
   if (!argsLocalEncoding) {
      Warning("%s: Couldn't convert arguments to default encoding.\n",
              __FUNCTION__);
      goto cleanup;
   }

   childPid = fork();

   switch (childPid) {
   case -1:
      err = errno;
      Warning("%s: Couldn't fork first child, error %u.\n", __FUNCTION__,
              err);
      goto cleanup;
   case 0:
      /* We're the first child.  Continue on. */
      break;
   default:
      {
         /* We're the original process.  Check if the first child exited. */
         int status;

         close(pipeFds[1]);
         waitpid(childPid, &status, 0);
         if (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS) {
            Warning("%s: Child %d exited with error %d.\n",
                    __FUNCTION__, childPid, WEXITSTATUS(status));
            goto cleanup;
         } else if (WIFSIGNALED(status)) {
            Warning("%s: Child %d exited with signal %d.\n",
                    __FUNCTION__, childPid, WTERMSIG(status));
            goto cleanup;
         }

         /*
          * Check if the second child exec'ed successfully.  If it had
          * an error, it will write a uint32 errno to this pipe before
          * exiting.  Otherwise, its end of the pipe will be closed on
          * exec and this call will fail as expected.
          * The assumption is that we don't get a partial read. In case,
          * it did happen, we can detect it by the number of bytes read.
          */

         while (TRUE) {
            int res = read(pipeFds[0], &err, sizeof err);

            if (res > 0) {
               Warning("%s: Child could not exec %s, read %d, error %u.\n",
                       __FUNCTION__, path, res, err);
               goto cleanup;
            } else if ((res == -1) && (errno == EINTR)) {
               continue;
            }
            break;
         }

         err = 0;
         goto cleanup;
      }
   }

   /*
    * Close all fds except for the write end of the error pipe (which we've
    * already set to close on successful exec), the pid file, and the ones
    * requested by the caller. Also reset the signal mask to unblock all
    * signals. fork() clears pending signals.
    */

   Hostinfo_ResetProcessState(tempFds, numKeepFds);
   free(tempFds);
   tempFds = NULL;
   sigfillset(&sig);
   sigprocmask(SIG_UNBLOCK, &sig, NULL);

   if (!(flags & HOSTINFO_DAEMONIZE_NOCLOSE) && setsid() == -1) {
      Warning("%s: Couldn't create new session, error %d.\n",
              __FUNCTION__, errno);

      _exit(EXIT_FAILURE);
   }

   switch (fork()) {
   case -1:
      {
         Warning("%s: Couldn't fork second child, error %d.\n",
                 __FUNCTION__, errno);

         _exit(EXIT_FAILURE);
      }
   case 0:
      // We're the second child.  Continue on.
      break;
   default:
      /*
       * We're the first child.  We don't need to exist any more.
       *
       * Exiting here causes the second child to be reparented to the
       * init process, so the original process doesn't need to wait
       * for the child we forked off.
       */

      _exit(EXIT_SUCCESS);
   }

   /*
    * We can't use our i18n wrappers for file manipulation at this
    * point, since we've forked; internal library mutexes might be
    * invalid.
    */

   if (!(flags & HOSTINFO_DAEMONIZE_NOCHDIR) && chdir("/") == -1) {
      uint32 err = errno;

      Warning("%s: Couldn't chdir to /, error %u.\n", __FUNCTION__, err);

      /* Let the original process know we failed to chdir. */
      if (write(pipeFds[1], &err, sizeof err) == -1) {
         Warning("%s: Couldn't write to parent pipe: %u, "
                 "original error: %u.\n", __FUNCTION__, errno, err);
      }
      _exit(EXIT_FAILURE);
   }

   if (!(flags & HOSTINFO_DAEMONIZE_NOCLOSE)) {
      int fd;

      fd = open(_PATH_DEVNULL, O_RDONLY);
      if (fd != -1) {
         dup2(fd, STDIN_FILENO);
         close(fd);
      }

      fd = open(_PATH_DEVNULL, O_WRONLY);
      if (fd != -1) {
         dup2(fd, STDOUT_FILENO);
         dup2(fd, STDERR_FILENO);
         close(fd);
      }
   }

   if (pidPath) {
      int64 pid;
      char pidString[32];
      int pidStringLen;

      ASSERT_ON_COMPILE(sizeof (pid_t) <= sizeof pid);
      ASSERT(pidPathFd >= 0);

      pid = getpid();
      pidStringLen = Str_Sprintf(pidString, sizeof pidString,
                                 "%"FMT64"d\n", pid);
      if (pidStringLen <= 0) {
         err = EINVAL;

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      if (ftruncate(pidPathFd, 0) == -1) {
         err = errno;
         Warning("%s: Couldn't truncate path [%s], error %d.\n",
                 __FUNCTION__, pidPath, err);

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      if (write(pidPathFd, pidString, pidStringLen) != pidStringLen) {
         err = errno;
         Warning("%s: Couldn't write PID to path [%s], error %d.\n",
                 __FUNCTION__, pidPath, err);

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      if (fsync(pidPathFd) == -1) {
         err = errno;
         Warning("%s: Couldn't flush PID to path [%s], error %d.\n",
                 __FUNCTION__, pidPath, err);

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      /* Leave pidPathFd open to hold the mutex until this process exits. */
      if (!(flags & HOSTINFO_DAEMONIZE_LOCKPID)) {
         close(pidPathFd);
      }
   }

   if (execv(pathLocalEncoding, argsLocalEncoding) == -1) {
      err = errno;
      Warning("%s: Couldn't exec %s, error %d.\n", __FUNCTION__, path, err);

      /* Let the original process know we failed to exec. */
      if (write(pipeFds[1], &err, sizeof err) == -1) {
         Warning("%s: Couldn't write to parent pipe: %u, "
                 "original error: %u.\n", __FUNCTION__, errno, err);
      }
      _exit(EXIT_FAILURE);
   }

   NOT_REACHED();

  cleanup:
   free(tempFds);

   if (pipeFds[0] != -1) {
      close(pipeFds[0]);
   }
   if (pipeFds[1] != -1) {
      close(pipeFds[1]);
   }
   Util_FreeStringList(argsLocalEncoding, -1);
   free(pathLocalEncoding);

   if (err == 0) {
      if (flags & HOSTINFO_DAEMONIZE_EXIT) {
         _exit(EXIT_SUCCESS);
      }
   } else {
      if (pidPath) {
         /*
          * Unlink pidPath on error before closing pidPathFd to avoid racing
          * with another process attempting to daemonize and unlinking the
          * file it created instead.
          */
         if (Posix_Unlink(pidPath) != 0) {
            Warning("%s: Unable to unlink %s: %u\n",
                    __FUNCTION__, pidPath, errno);
         }
      }

      errno = err;
   }

   if (pidPath) {
      close(pidPathFd);
   }

   return err == 0;
}


#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__HAIKU__)
/*
 *----------------------------------------------------------------------
 *
 * HostinfoGetCpuInfo --
 *
 *      Get some attribute from /proc/cpuinfo for a given CPU
 *
 * Results:
 *      On success: Allocated, NUL-terminated attribute string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
HostinfoGetCpuInfo(int nCpu,         // IN:
                   const char *name) // IN:
{
   FILE *f;
   char *line;
   int cpu = 0;
   char *value = NULL;

   f = Posix_Fopen("/proc/cpuinfo", "r");

   if (f == NULL) {
      Warning(LGPFX" %s: Unable to open /proc/cpuinfo\n", __FUNCTION__);

      return NULL;
   }

   while (cpu <= nCpu &&
          StdIO_ReadNextLine(f, &line, 0, NULL) == StdIO_Success) {
      char *s;

      if ((s = strstr(line, name)) &&
          (s = strchr(s, ':'))) {
         char *e;

         s++;
         e = s + strlen(s);

         /* Skip leading and trailing while spaces */
         for (; s < e && isspace(*s); s++);
         for (; s < e && isspace(e[-1]); e--);
         *e = 0;

         /* Free previous value */
         free(value);
         value = strdup(s);
         VERIFY(value);

         cpu++;
      }
      free(line);
   }

   fclose(f);

   return value;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetRatedCpuMhz --
 *
 *      Get the rated CPU speed of a given processor.
 *      Return value is in MHz.
 *
 * Results:
 *      TRUE on success, FALSE on failure
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetRatedCpuMhz(int32 cpuNumber,  // IN:
                        uint32 *mHz)      // OUT:
{
#if defined(__APPLE__) || defined(__FreeBSD__)

#  if defined(__APPLE__)
#     define CPUMHZ_SYSCTL_NAME "hw.cpufrequency_max"
#  elif __FreeBSD__version >= 50011
#     define CPUMHZ_SYSCTL_NAME "hw.clockrate"
#  endif

#  if defined(CPUMHZ_SYSCTL_NAME)
   uint32 hz;
   size_t hzSize = sizeof hz;

   /* 'cpuNumber' is ignored: Intel Macs are always perfectly symmetric. */

   if (sysctlbyname(CPUMHZ_SYSCTL_NAME, &hz, &hzSize, NULL, 0) == -1) {
      return FALSE;
   }

   *mHz = hz / 1000000;

   return TRUE;
#  else
   return FALSE;
#  endif
#elif defined(__HAIKU__)
   cpu_info info;
   if (get_cpu_info(cpuNumber, 1, &info) != B_OK) {
      return FALSE;
   }
   *mHz = info.current_frequency / 1000;
   return TRUE;
#else
#if defined(VMX86_SERVER)
   if (HostType_OSIsVMK()) {
      uint32 tscKhzEstimate;
      VMK_ReturnStatus status = VMKernel_GetTSCkhzEstimate(&tscKhzEstimate);

      /*
       * The TSC frequency matches the CPU frequency in all modern CPUs.
       * Regardless, the TSC frequency is a much better estimate of
       * reality than failing or returning zero.
       */

      *mHz = tscKhzEstimate / 1000;

      return (status == VMK_OK);
   }
#endif

   {
      float fMhz = 0;
      char *readVal = HostinfoGetCpuInfo(cpuNumber, "cpu MHz");

      if (readVal == NULL) {
         return FALSE;
      }

      if (sscanf(readVal, "%f", &fMhz) == 1) {
         *mHz = (unsigned int)(fMhz + 0.5);
      }

      free(readVal);
   }

   return TRUE;
#endif
}


#if defined(__APPLE__) || defined(__FreeBSD__)
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetSysctlStringAlloc --
 *
 *      Obtains the value of a string-type host sysctl.
 *
 * Results:
 *      On success: Allocated, NUL-terminated string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static char *
HostinfoGetSysctlStringAlloc(char const *sysctlName) // IN
{
   char *desc;
   size_t descSize;

   if (sysctlbyname(sysctlName, NULL, &descSize, NULL, 0) == -1) {
      return NULL;
   }

   desc = malloc(descSize);
   if (!desc) {
      return NULL;
   }

   if (sysctlbyname(sysctlName, desc, &descSize, NULL, 0) == -1) {
      free(desc);

      return NULL;
   }

   return desc;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetCpuDescription --
 *
 *      Get the descriptive name associated with a given CPU.
 *
 * Results:
 *      On success: Allocated, NUL-terminated string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_GetCpuDescription(uint32 cpuNumber)  // IN:
{
#if defined(__APPLE__)
   /* 'cpuNumber' is ignored: Intel Macs are always perfectly symmetric. */
   return HostinfoGetSysctlStringAlloc("machdep.cpu.brand_string");
#elif defined(__FreeBSD__)
   return HostinfoGetSysctlStringAlloc("hw.model");
#elif defined(__HAIKU__)
   system_info info;
   get_system_info(&info);

   uint32 topologyNodeCount = 0;
   cpu_topology_node_info* topology = NULL;
   get_cpu_topology_info(NULL, &topologyNodeCount);
   if (topologyNodeCount != 0)
      topology = malloc(sizeof(cpu_topology_node_info) * topologyNodeCount);
   get_cpu_topology_info(topology, &topologyNodeCount);

   enum cpu_platform platform = B_CPU_UNKNOWN;
   enum cpu_vendor cpuVendor = B_CPU_VENDOR_UNKNOWN;
   uint32 cpuModel = 0;
   for (uint32 i = 0; i < topologyNodeCount; i++) {
      switch (topology[i].type) {
         case B_TOPOLOGY_ROOT:
            platform = topology[i].data.root.platform;
            break;

         case B_TOPOLOGY_PACKAGE:
            cpuVendor = topology[i].data.package.vendor;
            break;

         case B_TOPOLOGY_CORE:
            cpuModel = topology[i].data.core.model;
            break;

         default:
            break;
      }
   }
   free(topology);

   const char *vendor = get_cpu_vendor_string(cpuVendor);
   const char *model = get_cpu_model_string(platform, cpuVendor, cpuModel);

   char modelString[32];

   if (model == NULL && vendor == NULL)
      model = "(Unknown)";
   else if (model == NULL) {
      model = modelString;
      snprintf(modelString, 32, "(Unknown %" B_PRIx32 ")", cpuModel);
   }

   size_t len = strlen(vendor) + strlen(model) + 2;
   char *desc = (char *)malloc(len);
   snprintf(desc, len, "%s%s%s", vendor ? vendor : "", vendor ? " " : "", model);
   return desc;
#elif defined VMX86_SERVER
   /* VMKernel treats mName as an in/out parameter so terminate it. */
   char mName[64] = { 0 };

   if (VMKernel_GetCPUModelName(cpuNumber, mName,
                                sizeof(mName)) == VMK_OK) {
      mName[sizeof(mName) - 1] = '\0';

      return strdup(mName);
   }

   return NULL;
#else
   return HostinfoGetCpuInfo(cpuNumber, "model name");
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_Execute --
 *
 *      Start program 'path'.  If 'wait' is TRUE, wait for program
 *	to complete and return exit status.
 *
 * Results:
 *      Exit status of 'path'.
 *
 * Side effects:
 *      Run a separate program.
 *
 *----------------------------------------------------------------------
 */

int
Hostinfo_Execute(const char *path,   // IN:
                 char * const *args, // IN:
                 Bool wait,          // IN:
                 const int *keepFds, // IN/OPT: array of fds to be kept open
                 size_t numKeepFds)  // IN: number of fds in keepFds
{
   int pid;
   int status;

   if (path == NULL) {
      return 1;
   }

   pid = fork();

   if (pid == -1) {
      return -1;
   }

   if (pid == 0) {
      Hostinfo_ResetProcessState(keepFds, numKeepFds);
      Posix_Execvp(path, args);
      exit(127);
   }

   if (wait) {
      for (;;) {
         if (waitpid(pid, &status, 0) == -1) {
            if (errno == ECHILD) {
               return 0;   // This sucks.  We really don't know.
            }
            if (errno != EINTR) {
               return -1;
            }
         } else {
            return status;
         }
      }
   } else {
      return 0;
   }
}


#ifdef __APPLE__
/*
 * How to retrieve kernel zone information. A little bit of history
 * ---
 * 1) In Mac OS versions < 10.6, we could retrieve kernel zone information like
 *    zprint did, i.e. by invoking the host_zone_info() Mach call.
 *
 *    osfmk/mach/mach_host.defs defines both arrays passed to host_zone_info()
 *    as 'out' parameters, but the implementation of the function in
 *    osfmk/kern/zalloc.c clearly treats them as 'inout' parameters. This issue
 *    is confirmed in practice: the input values passed by the user process are
 *    ignored. Now comes the scary part: is the input of the kernel function
 *    deterministically invalid, or is it some non-deterministic garbage (in
 *    which case the kernel could corrupt the user address space)? The answer
 *    is in the Mach IPC code. A cursory kernel debugging session seems to
 *    imply that the input pointer values are garbage, but the input size
 *    values are always 0. So host_zone_info() seems safe to use in practice.
 *
 * 2) In Mac OS 10.6, Apple introduced the 64-bit kernel.
 *
 *    2.1) They modified host_zone_info() to always returns KERN_NOT_SUPPORTED
 *         when the sizes (32-bit or 64-bit) of the user and kernel virtual
 *         address spaces do not match. Was bug 377049.
 *
 *         zprint got away with it by re-executing itself to match the kernel.
 *
 *    2.2) They broke the ABI for 64-bit user processes: the
 *         'zone_info.zi_*_size' fields are 32-bit in the Mac OS 10.5 SDK, and
 *         64-bit in the Mac OS 10.6 SDK. So a 64-bit user process compiled
 *         against the Mac OS 10.5 SDK works with the Mac OS 10.5 (32-bit)
 *         kernel but fails with the Mac OS 10.6 64-bit kernel.
 *
 *         zprint in Mac OS 10.6 is compiled against the Mac OS 10.6 SDK, so it
 *         got away with it.
 *
 *    The above two things made it very impractical for us to keep calling
 *    host_zone_info(). Instead we invoked zprint and parsed its non-localized
 *    output.
 *
 * 3) In Mac OS 10.7, Apple cleaned their mess and solved all the above
 *    problems by introducing a new mach_zone_info() Mach call. So this is what
 *    we use now. Was bug 816610.
 *
 * 4) In Mac OS 10.8, Apple appears to have modified mach_zone_info() to always
 *    return KERN_INVALID_HOST(!) when the calling process (not the calling
 *    thread!) is not root.
 */


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetKernelZoneElemSize --
 *
 *      Retrieve the size of the elements in a named kernel zone.
 *
 * Results:
 *      On success: the size (in bytes) > 0.
 *      On failure: 0.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

size_t
Hostinfo_GetKernelZoneElemSize(char const *name) // IN: Kernel zone name
{
   size_t result = 0;
   mach_zone_name_t *namesPtr;
   mach_msg_type_number_t namesSize;
   mach_zone_info_t *infosPtr;
   mach_msg_type_number_t infosSize;
   kern_return_t kr;
   mach_msg_type_number_t i;

   ASSERT(name);

   kr = mach_zone_info(mach_host_self(), &namesPtr, &namesSize, &infosPtr,
                       &infosSize);
   if (kr != KERN_SUCCESS) {
      Warning("%s: mach_zone_info failed %u.\n", __FUNCTION__, kr);
      return result;
   }

   ASSERT(namesSize == infosSize);
   for (i = 0; i < namesSize; i++) {
      if (!strcmp(namesPtr[i].mzn_name, name)) {
         result = infosPtr[i].mzi_elem_size;
         /* Check that nothing of value was lost during the cast. */
         ASSERT(result == infosPtr[i].mzi_elem_size);
         break;
      }
   }

   ASSERT_ON_COMPILE(sizeof namesPtr <= sizeof (vm_address_t));
   kr = vm_deallocate(mach_task_self(), (vm_address_t)namesPtr,
                      namesSize * sizeof *namesPtr);
   ASSERT(kr == KERN_SUCCESS);

   ASSERT_ON_COMPILE(sizeof infosPtr <= sizeof (vm_address_t));
   kr = vm_deallocate(mach_task_self(), (vm_address_t)infosPtr,
                      infosSize * sizeof *infosPtr);
   ASSERT(kr == KERN_SUCCESS);

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetHardwareModel --
 *
 *      Obtains the hardware model identifier (i.e. "MacPro5,1") from the host.
 *
 * Results:
 *      On success: Allocated, NUL-terminated string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_GetHardwareModel(void)
{
   return HostinfoGetSysctlStringAlloc("hw.model");
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_ProcessIsRosetta --
 *
 *      Checks if the current process is running as a translated binary.
 *
 * Results:
 *      0 for a native process, 1 for a translated process,
 *      and -1 when an error occurs.
 *
 * Side effects:
 *      None
 *----------------------------------------------------------------------
 */

int
Hostinfo_ProcessIsRosetta(void)
{
   int ret = 0;
   size_t size = sizeof ret;

   if (sysctlbyname("sysctl.proc_translated", &ret, &size, NULL, 0) == -1) {
      return errno == ENOENT ? 0 : -1;
   }
   return ret;
}
#endif /* __APPLE__ */


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_SystemUpTime --
 *
 *      Return system uptime in microseconds.
 *
 *      Please note that the actual resolution of this "clock" is undefined -
 *      it varies between OSen and OS versions. Use Hostinfo_SystemTimerUS
 *      whenever possible.
 *
 * Results:
 *      System uptime in microseconds or zero in case of a failure.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

VmTimeType
Hostinfo_SystemUpTime(void)
{
#if defined(__APPLE__)
   return Hostinfo_SystemTimerUS();
#elif defined(__HAIKU__)
   return system_time();
#elif defined(__linux__)
   int res;
   double uptime;
   int fd;
   char buf[256];

   static Atomic_Int fdStorage = { -1 };
   static Atomic_uint32 logFailedPread = { 1 };

   /*
    * /proc/uptime does not exist on Visor.  Use syscall instead.
    * Discovering Visor is a run-time check with a compile-time hint.
    */
   if (vmx86_server && HostType_OSIsVMK()) {
      uint64 uptime;
#ifdef VMX86_SERVER
      if (UNLIKELY(VMKernel_GetUptimeUS(&uptime) != VMK_OK)) {
         Log("%s: failure!\n", __FUNCTION__);

         uptime = 0;  // A timer read failure - this is really bad!
      }
#endif
      return uptime;
   }

   fd = Atomic_ReadInt(&fdStorage);

   /* Do we need to open the file the first time through? */
   if (UNLIKELY(fd == -1)) {
      fd = open("/proc/uptime", O_RDONLY);

      if (fd == -1) {
         Warning(LGPFX" Failed to open /proc/uptime: %s\n",
                 Err_Errno2String(errno));

         return 0;
      }

      /* Try to swap ours in. If we lose the race, close our fd */
      if (Atomic_ReadIfEqualWriteInt(&fdStorage, -1, fd) != -1) {
         close(fd);
      }

      /* Get the winning fd - either ours or theirs, doesn't matter anymore */
      fd = Atomic_ReadInt(&fdStorage);
   }

   ASSERT(fd != -1);

   res = pread(fd, buf, sizeof buf - 1, 0);
   if (res == -1) {
      /*
       * In case some kernel broke pread (like 2.6.28-rc1), have a fall-back
       * instead of spewing the log.  This should be rare.  Using a lock
       * around lseek and read does not work here as it will deadlock with
       * allocTrack/fileTrack enabled.
       */

      if (Atomic_ReadIfEqualWrite(&logFailedPread, 1, 0) == 1) {
         Warning(LGPFX" Failed to pread /proc/uptime: %s\n",
                 Err_Errno2String(errno));
      }
      fd = open("/proc/uptime", O_RDONLY);
      if (fd == -1) {
         Warning(LGPFX" Failed to retry open /proc/uptime: %s\n",
                 Err_Errno2String(errno));

         return 0;
      }
      res = read(fd, buf, sizeof buf - 1);
      close(fd);
      if (res == -1) {
         Warning(LGPFX" Failed to read /proc/uptime: %s\n",
                 Err_Errno2String(errno));

         return 0;
      }
   }
   ASSERT(res < sizeof buf);
   buf[res] = '\0';

   if (sscanf(buf, "%lf", &uptime) != 1) {
      Warning(LGPFX" Failed to parse /proc/uptime\n");

      return 0;
   }

   return uptime * 1000 * 1000;
#else
NOT_IMPLEMENTED();
#endif
}


#if !defined(__APPLE__) && !defined(__HAIKU__)
/*
 *----------------------------------------------------------------------
 *
 * HostinfoFindEntry --
 *
 *      Search a buffer for a pair `STRING <blanks> DIGITS'
 *	and return the number DIGITS, or 0 when fail.
 *
 * Results:
 *      TRUE on  success, FALSE on failure
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool
HostinfoFindEntry(char *buffer,         // IN: Buffer
                  const char *string,   // IN: String sought
                  unsigned int *value)  // OUT: Value
{
   char *p = strstr(buffer, string);
   unsigned int val;

   if (p == NULL) {
      return FALSE;
   }

   p += strlen(string);

   while (*p == ' ' || *p == '\t') {
      p++;
   }
   if (*p < '0' || *p > '9') {
      return FALSE;
   }

   val = strtoul(p, NULL, 10);
   if ((errno == ERANGE) || (errno == EINVAL)) {
      return FALSE;
   }

   *value = val;

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * HostinfoGetMemInfo --
 *
 *      Get some attribute from /proc/meminfo
 *      Return value is in KB.
 *
 * Results:
 *      TRUE on success, FALSE on failure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
HostinfoGetMemInfo(const char *name,    // IN:
                   unsigned int *value) // OUT:
{
   size_t len;
   char   buffer[4096];

   int fd = Posix_Open("/proc/meminfo", O_RDONLY);

   if (fd == -1) {
      Warning(LGPFX" %s: Unable to open /proc/meminfo\n", __FUNCTION__);

      return FALSE;
   }

   len = read(fd, buffer, sizeof buffer - 1);
   close(fd);

   if (len == -1) {
      return FALSE;
   }

   buffer[len] = '\0';

   return HostinfoFindEntry(buffer, name, value);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSysinfo --
 *
 *      Retrieve system information on a Linux system.
 *
 * Results:
 *      TRUE on success: '*totalRam', '*freeRam', '*totalSwap' and '*freeSwap'
 *                       are set if not NULL
 *      FALSE on failure
 *
 * Side effects:
 *      None.
 *
 *      This seems to be a very expensive call: like 5ms on 1GHz P3 running
 *      RH6.1 Linux 2.2.12-20.  Yes, that's 5 milliseconds.  So caller should
 *      take care.  -- edward
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSysinfo(uint64 *totalRam,  // OUT: Total RAM in bytes
                uint64 *freeRam,   // OUT: Free RAM in bytes
                uint64 *totalSwap, // OUT: Total swap in bytes
                uint64 *freeSwap)  // OUT: Free swap in bytes
{
#ifdef HAVE_SYSINFO
   // Found in linux/include/kernel.h for a 2.5.6 kernel --hpreg
   struct vmware_sysinfo {
	   long uptime;			/* Seconds since boot */
	   unsigned long loads[3];	/* 1, 5, and 15 minute load averages */
	   unsigned long totalram;	/* Total usable main memory size */
	   unsigned long freeram;	/* Available memory size */
	   unsigned long sharedram;	/* Amount of shared memory */
	   unsigned long bufferram;	/* Memory used by buffers */
	   unsigned long totalswap;	/* Total swap space size */
	   unsigned long freeswap;	/* swap space still available */
	   unsigned short procs;	/* Number of current processes */
	   unsigned short pad;		/* explicit padding for m68k */
	   unsigned long totalhigh;	/* Total high memory size */
	   unsigned long freehigh;	/* Available high memory size */
	   unsigned int mem_unit;	/* Memory unit size in bytes */
	   // Padding: libc5 uses this..
	   char _f[20 - 2 * sizeof(long) - sizeof(int)];
   };
   struct vmware_sysinfo si;

   if (sysinfo((struct sysinfo *)&si) < 0) {
      return FALSE;
   }

   if (si.mem_unit == 0) {
      /*
       * Kernel versions < 2.3.23. Those kernels used a smaller sysinfo
       * structure, whose last meaningful field is 'procs' --hpreg
       */

      si.mem_unit = 1;
   }

   if (totalRam) {
      *totalRam = (uint64)si.totalram * si.mem_unit;
   }
   if (freeRam) {
      *freeRam = (uint64)si.freeram * si.mem_unit;
   }
   if (totalSwap) {
      *totalSwap = (uint64)si.totalswap * si.mem_unit;
   }
   if (freeSwap) {
      *freeSwap = (uint64)si.freeswap * si.mem_unit;
   }

   return TRUE;
#else // ifdef HAVE_SYSINFO
   NOT_IMPLEMENTED();
#endif // ifdef HAVE_SYSINFO
}
#endif // ifndef __APPLE__


#if defined(__linux__) || defined(__FreeBSD__) || defined(sun)
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetLinuxMemoryInfoInPages --
 *
 *      Obtain the minimum memory to be maintained, total memory available,
 *      and free memory available on the host (Linux) in pages.
 *
 * Results:
 *      TRUE on success: '*minSize', '*maxSize' and '*currentSize' are set
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
HostinfoGetLinuxMemoryInfoInPages(unsigned int *minSize,      // OUT:
                                  unsigned int *maxSize,      // OUT:
                                  unsigned int *currentSize)  // OUT:
{
   uint64 total;
   uint64 free;
   unsigned int cached = 0;

   /*
    * Note that the free memory provided by linux does not include buffer and
    * cache memory. Linux tries to use the free memory to cache file. Most of
    * those memory can be freed immediately when free memory is low,
    * so for our purposes it should be counted as part of the free memory .
    * There is no good way to collect the useable free memory in 2.2 and 2.4
    * kernel.
    *
    * Here is our solution: The free memory we report includes cached memory.
    * Mmapped memory is reported as cached. The guest RAM memory, which is
    * mmaped to a ram file, therefore make up part of the cached memory. We
    * exclude the size of the guest RAM from the amount of free memory that we
    * report here. Since we don't know about the RAM size of other VMs, we
    * leave that to be done in serverd/MUI.
    */

   if (HostinfoSysinfo(&total, &free, NULL, NULL) == FALSE) {
      return FALSE;
   }

   /*
    * Convert to pages and round up total memory to the nearest multiple of 8
    * or 32 MB, since the "total" amount of memory reported by Linux is the
    * total physical memory - amount used by the kernel.
    */

   if (total < (uint64)128 * 1024 * 1024) {
      total = ROUNDUP(total, (uint64)8 * 1024 * 1024);
   } else {
      total = ROUNDUP(total, (uint64)32 * 1024 * 1024);
   }

   *minSize = 128; // XXX - Figure out this value
   *maxSize = total / PAGE_SIZE;

   HostinfoGetMemInfo("Cached:", &cached);
   if (currentSize) {
      *currentSize = free / PAGE_SIZE + cached / (PAGE_SIZE / 1024);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetSwapInfoInPages --
 *
 *      Obtain the total swap and free swap on the host (Linux) in
 *      pages.
 *
 * Results:
 *      TRUE on success: '*totalSwap' and '*freeSwap' are set if not NULL
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetSwapInfoInPages(unsigned int *totalSwap,  // OUT:
                            unsigned int *freeSwap)   // OUT:
{
   uint64 total;
   uint64 free;

   if (HostinfoSysinfo(NULL, NULL, &total, &free) == FALSE) {
      return FALSE;
   }

   if (totalSwap != NULL) {
      *totalSwap = total / PAGE_SIZE;
   }

   if (freeSwap != NULL) {
      *freeSwap = free / PAGE_SIZE;
   }

   return TRUE;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetMemoryInfoInPages --
 *
 *      Obtain the minimum memory to be maintained, total memory available,
 *      and free memory available on the host in pages.
 *
 * Results:
 *      TRUE on success: '*minSize', '*maxSize' and '*currentSize' are set
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetMemoryInfoInPages(unsigned int *minSize,      // OUT:
                              unsigned int *maxSize,      // OUT:
                              unsigned int *currentSize)  // OUT:
{
#if defined(__APPLE__)
   mach_msg_type_number_t count;
   vm_statistics_data_t stat;
   kern_return_t error;
   uint64_t memsize;
   size_t memsizeSize = sizeof memsize;

   /*
    * Largely inspired by
    * darwinsource-10.4.5/top-15/libtop.c::libtop_p_vm_sample().
    */

   count = HOST_VM_INFO_COUNT;
   error = host_statistics(mach_host_self(), HOST_VM_INFO,
                           (host_info_t) &stat, &count);

   if (error != KERN_SUCCESS || count != HOST_VM_INFO_COUNT) {
      Warning("%s: Unable to retrieve host vm stats.\n", __FUNCTION__);

      return FALSE;
   }

   // XXX Figure out this value.
   *minSize = 128;

   /*
    * XXX Hopefully this includes cached memory as well. We should check.
    * No. It returns only completely used pages.
    */

   *currentSize = stat.free_count;

   /*
    * Adding up the stat values does not sum to 100% of physical memory.
    * The correct value is available from sysctl so we do that instead.
    */

   if (sysctlbyname("hw.memsize", &memsize, &memsizeSize, NULL, 0) == -1) {
      Warning("%s: Unable to retrieve host vm hw.memsize.\n", __FUNCTION__);

      return FALSE;
   }

   *maxSize = memsize / PAGE_SIZE;
   return TRUE;
#elif defined(__HAIKU__)
   system_info info;
   status_t status = get_system_info(&info);
   if (status == B_OK)
   {
      *minSize = info.needed_memory / B_PAGE_SIZE;
      *currentSize = info.max_pages - info.free_memory / B_PAGE_SIZE;
      *maxSize = info.max_pages;
      return TRUE;
   }

   return FALSE;
#elif defined(VMX86_SERVER)
   uint64 total;
   uint64 free;
   VMK_ReturnStatus status;

   if (VmkSyscall_Init(FALSE, NULL, 0)) {
      status = VMKernel_GetMemSize(&total, &free);
      if (status == VMK_OK) {
         *minSize = 128;
         *maxSize = total / PAGE_SIZE;
         *currentSize = free / PAGE_SIZE;

         return TRUE;
      }
   }

   return FALSE;
#else
   return HostinfoGetLinuxMemoryInfoInPages(minSize, maxSize, currentSize);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetModulePath --
 *
 *	Retrieve the full path to the executable. Not supported under VMvisor.
 *
 *      The value can be controlled by the invoking user, so the calling code
 *      should perform extra checks if it is going to use the value to
 *      open/exec content in a security-sensitive context.
 *
 * Results:
 *      On success: The allocated, NUL-terminated file path.
 *         Note: This path can be a symbolic or hard link; it's just one
 *         possible path to access the executable.
 *
 *      On failure: NULL.
 *
 * Side effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_GetModulePath(uint32 priv)  // IN:
{
   char *path;

#if defined(__APPLE__)
   uint32_t pathSize = FILE_MAXPATH;
#elif defined(__HAIKU__)
   uint32_t pathSize = PATH_MAX;
#else
   uid_t uid = -1;
#endif

   if ((priv != HGMP_PRIVILEGE) && (priv != HGMP_NO_PRIVILEGE)) {
      Warning("%s: invalid privilege parameter\n", __FUNCTION__);

      return NULL;
   }

#if defined(__APPLE__)
   path = Util_SafeMalloc(pathSize);
   if (_NSGetExecutablePath(path, &pathSize)) {
      Warning(LGPFX" %s: _NSGetExecutablePath failed.\n", __FUNCTION__);
      free(path);

      return NULL;
   }

#elif defined(__HAIKU__)
   path = Util_SafeMalloc(pathSize);
   if (find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, path,
                 pathSize) != B_OK) {
      Warning(LGPFX" %s: find_path failed.\n", __FUNCTION__);
      free(path);

      return NULL;
   }

#else
#if defined(VMX86_SERVER)
   if (HostType_OSIsVMK()) {
      return NULL;
   }
#endif

   // "/proc/self/exe" only exists on Linux 2.2+.
   ASSERT(Hostinfo_OSVersion(0) > 2 ||
          (Hostinfo_OSVersion(0) == 2 && Hostinfo_OSVersion(1) >= 2));

   if (priv == HGMP_PRIVILEGE) {
      uid = Id_BeginSuperUser();
   }

   path = Posix_ReadLink("/proc/self/exe");

   if (priv == HGMP_PRIVILEGE) {
      Id_EndSuperUser(uid);
   }

   if (path == NULL) {
      Warning(LGPFX" %s: readlink failed: %s\n", __FUNCTION__,
              Err_Errno2String(errno));
   }
#endif

   return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_GetLibraryPath --
 *
 *      Try and deduce the path to the library where the specified
 *      address resides. Expected usage is that the caller will pass
 *      in the address of one of the caller's own functions.
 *
 *      Not implemented on MacOS.
 *
 * Results:
 *      The path (which MAY OR MAY NOT BE ABSOLUTE) or NULL on failure.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *----------------------------------------------------------------------
 */

char *
Hostinfo_GetLibraryPath(void *addr)  // IN
{
#ifdef __linux__
   Dl_info info;

   if (dladdr(addr, &info)) {
      return Unicode_Alloc(info.dli_fname, STRING_ENCODING_DEFAULT);
   }
   return NULL;
#else
   return NULL;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_AcquireProcessSnapshot --
 *
 *      Acquire a snapshot of the process table. On POSIXen, this is
 *      a NOP.
 *
 * Results:
 *      !NULL - A process snapshot pointer.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

struct HostinfoProcessSnapshot {
   int dummy;
};

static HostinfoProcessSnapshot hostinfoProcessSnapshot = { 0 };

HostinfoProcessSnapshot *
Hostinfo_AcquireProcessSnapshot(void)
{
   return &hostinfoProcessSnapshot;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_ReleaseProcessSnapshot --
 *
 *      Release a snapshot of the process table. On POSIXen, this is
 *      a NOP.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Hostinfo_ReleaseProcessSnapshot(HostinfoProcessSnapshot *s)  // IN/OPT:
{
   if (s != NULL) {
      VERIFY(s == &hostinfoProcessSnapshot);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_QueryProcessExistence --
 *
 *      Determine if a PID is "alive" or "dead". Failing to be able to
 *      do this perfectly, do not make any assumption - say the answer
 *      is unknown.
 *
 * Results:
 *      HOSTINFO_PROCESS_QUERY_ALIVE    Process is alive
 *      HOSTINFO_PROCESS_QUERY_DEAD     Process is dead
 *      HOSTINFO_PROCESS_QUERY_UNKNOWN  Don't know
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

HostinfoProcessQuery
Hostinfo_QueryProcessExistence(int pid)  // IN:
{
   HostinfoProcessQuery result;

   switch ((kill(pid, 0) == -1) ? errno : 0) {
   case 0:
   case EPERM:
      result = HOSTINFO_PROCESS_QUERY_ALIVE;
      break;

   case ESRCH:
      result = HOSTINFO_PROCESS_QUERY_DEAD;
      break;

   default:
      result = HOSTINFO_PROCESS_QUERY_UNKNOWN;
      break;
   }

   return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_QueryProcessSnapshot --
 *
 *      Determine if a PID is "alive" or "dead" within the specified
 *      process snapshot. Failing to be able to do this perfectly,
 *      do not make any assumption - say the answer is unknown.
 *
 * Results:
 *      HOSTINFO_PROCESS_QUERY_ALIVE    Process is alive
 *      HOSTINFO_PROCESS_QUERY_DEAD     Process is dead
 *      HOSTINFO_PROCESS_QUERY_UNKNOWN  Don't know
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

HostinfoProcessQuery
Hostinfo_QueryProcessSnapshot(HostinfoProcessSnapshot *s,  // IN:
                              int pid)                     // IN:
{
   ASSERT(s != NULL);

   return Hostinfo_QueryProcessExistence(pid);
}
