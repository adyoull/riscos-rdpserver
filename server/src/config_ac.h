/* RDPServer: in place of the config_ac.h xrdp's configure would write.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef CONFIG_AC_H
#define CONFIG_AC_H

#define PACKAGE_VERSION "0.10.6.1"
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1

/* Where libxrdp looks for rsakeys.ini and the like: the server's Choices. */
const char *plat_choices_dir (void);
#define XRDP_CFG_PATH plat_choices_dir ()
#define XRDP_PID_PATH "/tmp"
#define XRDP_SBIN_PATH "/tmp"
#define XRDP_SHARE_PATH "/tmp"
#define XRDP_LIB_PATH "/tmp"
#define XRDP_LOG_PATH "/tmp"
#define XRDP_SOCKET_ROOT_PATH "/tmp"

#endif
