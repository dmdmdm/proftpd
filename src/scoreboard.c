/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 2001, 2002 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, The ProFTPD Project and other respective copyright
 * holders give permission to link this program with OpenSSL, and distribute
 * the resulting executable, without including the source code for OpenSSL in
 * the source distribution.
 */

/*
 * ProFTPD scoreboard support.
 *
 * $Id: scoreboard.c,v 1.7 2002-10-04 19:08:26 castaglia Exp $
 */

#include "conf.h"

#include <signal.h>

/* From src/dirtree.c */
extern char ServerType;

static int scoreboard_fd = -1;
static char scoreboard_file[MAX_PATH_LEN] = RUN_DIR "/proftpd.scoreboard";

static off_t current_pos = 0;
static pr_scoreboard_header_t header;
static pr_scoreboard_entry_t entry;
static struct flock entry_lock;

static unsigned char scoreboard_read_locked = FALSE;
static unsigned char scoreboard_write_locked = FALSE;

/* Internal routines
 */

static char *handle_score_cmd(const char *fmt, va_list cmdap) {
  static char buf[80] = {'\0'};
  memset(buf, '\0', sizeof(buf));
  vsnprintf(buf, sizeof(buf), fmt, cmdap);
  buf[sizeof(buf)-1] = '\0';
  return buf;
}

static int read_scoreboard_header(pr_scoreboard_header_t *header) {
  int res = 0;

  /* NOTE: reading a struct from a file using read(2) -- bad (in general). */
  while ((res = read(scoreboard_fd, header, sizeof(pr_scoreboard_header_t))) !=
      sizeof(pr_scoreboard_header_t)) {
    if (res == 0)
      return -1;

    if (errno == EINTR) {
      pr_handle_signals();
      continue;

    } else
      return -1;
  }

  /* Note: these errors will most likely occur only for inetd-run daemons.
   * Standalone daemons erase the scoreboard on startup.
   */

  if (header->sch_magic != PR_SCOREBOARD_MAGIC) {
    pr_close_scoreboard();
    return PR_SCORE_ERR_BAD_MAGIC;
  }

  if (header->sch_version < PR_SCOREBOARD_VERSION) {
    pr_close_scoreboard();
    return PR_SCORE_ERR_OLDER_VERSION;
  }

  if (header->sch_version > PR_SCOREBOARD_VERSION) {
    pr_close_scoreboard();
    return PR_SCORE_ERR_NEWER_VERSION;
  }

  return 0;
}

static int rlock_scoreboard(void) {
  struct flock lock;

  lock.l_type = F_RDLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;

  while (fcntl(scoreboard_fd, F_SETLKW, &lock) < 0) {
    if (errno == EINTR) {
      pr_handle_signals();
      continue;

    } else
      return -1;
  }

  scoreboard_read_locked = TRUE;
  return 0;
}

static int unlock_entry(void) {

  entry_lock.l_type = F_UNLCK;
  entry_lock.l_whence = SEEK_CUR;
  entry_lock.l_len = sizeof(pr_scoreboard_entry_t);

  while (fcntl(scoreboard_fd, F_SETLKW, &entry_lock) < 0) {
    if (errno == EINTR) {
      pr_handle_signals();
      continue;

    } else
      return -1;
  }

  return 0;
}

static int unlock_scoreboard(void) {
  struct flock lock;

  lock.l_type = F_UNLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;

  scoreboard_read_locked = scoreboard_write_locked = FALSE;
  return fcntl(scoreboard_fd, F_SETLK, &lock);
}

static int wlock_entry(void) {
  entry_lock.l_type = F_WRLCK;
  entry_lock.l_whence = SEEK_CUR;
  entry_lock.l_len = sizeof(pr_scoreboard_entry_t);

  while (fcntl(scoreboard_fd, F_SETLKW, &entry_lock) < 0) {
    if (errno == EINTR) {
      pr_handle_signals();
      continue;

    } else
      return -1;
  }

  return 0;
}

static int wlock_scoreboard(void) {
  struct flock lock;

  lock.l_type = F_WRLCK;
  lock.l_whence = 0;
  lock.l_start = 0;
  lock.l_len = 0;

  while (fcntl(scoreboard_fd, F_SETLKW, &lock) < 0) {
    if (errno == EINTR) {
      pr_handle_signals();
      continue;

    } else
      return -1;
  }

  scoreboard_write_locked = TRUE;
  return 0;
}


static int write_entry(void) {
  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  lseek(scoreboard_fd, entry_lock.l_start, SEEK_SET);

  while (write(scoreboard_fd, &entry, sizeof(entry)) != sizeof(entry)) {
    if (errno == EINTR) {
      pr_handle_signals();
      continue;

    } else
      return -1;
  }

  /* Rewind. */
  lseek(scoreboard_fd, entry_lock.l_start, SEEK_SET);

  return 0;
}

/* Public routines
 */

int pr_close_scoreboard(void) {
  if (scoreboard_fd == -1)
    return 0;

  if (scoreboard_read_locked || scoreboard_write_locked)
    unlock_scoreboard();

  close(scoreboard_fd);
  scoreboard_fd = -1;

  return 0;
}

void pr_delete_scoreboard(void) {
  if (scoreboard_fd > -1)
    close(scoreboard_fd);
  scoreboard_fd = -1;

  if (scoreboard_file)
    unlink(scoreboard_file);
}

const char *pr_get_scoreboard(void) {
  return scoreboard_file;
}

int pr_open_scoreboard(int flags, pid_t *daemon_pid) {
  int res;
  struct stat st;

  /* Prevent writing to a symlink while avoiding a race condition: open
   * the file name O_RDWR|O_CREAT first, then check to see if it's a symlink.
   * If so, close the file and error out.  If not, truncate as necessary,
   * and continue.
   */
  if ((scoreboard_fd = open(scoreboard_file, flags|O_CREAT,
      PR_SCOREBOARD_MODE)) < 0)
    return -1;

  /* Make certain that the scoreboard mode will be read-only for everyone
   * except the user owner (this allows for non-root-running daemons to
   * still modify the scoreboard).
   */
  fchmod(scoreboard_fd, 0644);

  if (fstat(scoreboard_fd, &st) < 0) {
    close(scoreboard_fd);
    scoreboard_fd = -1;
    return -1;
  }

  if (S_ISLNK(st.st_mode)) {
    close(scoreboard_fd);
    scoreboard_fd = -1;
    errno = EPERM;
    return -1;
  }

  /* Check the header of this scoreboard file. */
  if ((res = read_scoreboard_header(&header)) == -1) {

    /* If this file is newly created, it needs to have the header
     * written.
     */
    header.sch_magic = PR_SCOREBOARD_MAGIC;
    header.sch_version = PR_SCOREBOARD_VERSION;

    if (ServerType == SERVER_STANDALONE)
      header.sch_pid = getpid();
    else
      header.sch_pid = 0;

    while (write(scoreboard_fd, &header, sizeof(header)) != sizeof(header)) {
      if (errno == EINTR) {
        pr_handle_signals();
        continue;

      } else
        return -1;
    }

    return 0;

  } else if (res < 0)
    return -1;

  if (daemon_pid)
    *daemon_pid = header.sch_pid;

  return 0;
}

int pr_restore_scoreboard(void) {

  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  /* Position the file position pointer of the scoreboard back to
   * where it was, prior to the last pr_rewind_scoreboard() call.
   */
  lseek(scoreboard_fd, current_pos, SEEK_SET);
  return 0;
}

int pr_rewind_scoreboard(void) {

  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  current_pos = lseek(scoreboard_fd, 0, SEEK_CUR);

  /* Position the file position pointer of the scoreboard at the
   * start of the scoreboard (past the header).
   */
  lseek(scoreboard_fd, sizeof(pr_scoreboard_header_t), SEEK_SET);
  return 0;
}

int pr_set_scoreboard(const char *path) {
  char dir[MAX_PATH_LEN] = {'\0'};
  struct stat st;
  char *tmp = NULL;

  sstrncpy(dir, path, sizeof(dir));

  if ((tmp = strrchr(dir, '/')) == NULL) {
    errno = EINVAL;
    return -1;
  }
  *tmp = '\0';

  /* Parent directory must not be world-writeable */

  if (stat(dir, &st) < 0)
    return -1;

  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }

  if (st.st_mode & S_IWOTH) {
    errno = EPERM;
    return -1;
  }

  sstrncpy(scoreboard_file, path, sizeof(scoreboard_file));
  return 0;
}

int pr_scoreboard_add_entry(void) {
  unsigned char found_slot = FALSE;

  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  /* Write-lock the scoreboard file. */
  wlock_scoreboard();

  /* If the scoreboard is open, the file position is already past the
   * header.
   */
  while (TRUE) {
    int res = 0;
    while ((res = read(scoreboard_fd, &entry, sizeof(entry))) ==
        sizeof(entry)) {

      /* If this entry's PID is marked as zero, it means this slot can be
       * reused.
       */
      if (!entry.sce_pid) {
        entry_lock.l_start = lseek(scoreboard_fd, 0, SEEK_CUR) - sizeof(entry);
        found_slot = TRUE;
        break;
      }
    }

    if (res == 0) {
      entry_lock.l_start = lseek(scoreboard_fd, 0, SEEK_CUR);
      found_slot = TRUE;
    }

    if (found_slot)
      break;

    if (errno == EINTR)
      pr_handle_signals();
  }

  memset(&entry, '\0', sizeof(entry));

  entry.sce_pid = getpid();
  entry.sce_uid = geteuid();
  entry.sce_gid = getegid();

  if (write_entry() < 0)
    log_pri(LOG_NOTICE, "error writing scoreboard entry: %s", strerror(errno));

  /* We can unlock the scoreboard now. */
  unlock_scoreboard();

  return 0;
}

int pr_scoreboard_del_entry(unsigned char verbose) {

  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  memset(&entry, '\0', sizeof(entry));

  /* Write-lock this entry */
  wlock_entry();
  if (write_entry() < 0 && verbose)
    log_pri(LOG_NOTICE, "error deleting scoreboard entry: %s", strerror(errno));
  unlock_entry();

  return 0;
}

pr_scoreboard_entry_t *pr_scoreboard_read_entry(void) {
  static pr_scoreboard_entry_t scan_entry;
  int res = 0;
 
  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return NULL;
  }

  /* Make sure the scoreboard file is read-locked. */
  if (!scoreboard_read_locked)
    rlock_scoreboard();

  memset(&scan_entry, '\0', sizeof(scan_entry));

  /* NOTE: use readv(2)? */
  while (TRUE) {
    while ((res = read(scoreboard_fd, &scan_entry, sizeof(scan_entry))) <= 0) {
      if (res < 0 && errno == EINTR) {
        pr_handle_signals();
        continue;

      } else {
        unlock_scoreboard();
        return NULL;
      }
    }

    if (scan_entry.sce_pid) {
      unlock_scoreboard();
      return &scan_entry;

    } else
      continue;
  }

  unlock_scoreboard();
  return NULL;
}

/* We get clever with this function, so that it can be used to update
 * various entry attributes.
 */
int pr_scoreboard_update_entry(pid_t pid, ...) {
  va_list ap;
  char *tmp = NULL;
  int entry_tag = 0;
  
  if (scoreboard_fd < 0) {
    errno = EINVAL;
    return -1;
  }

  /* If updating some fields, clear the begin_idle field.
   */

  va_start(ap, pid);

  while ((entry_tag = va_arg(ap, int)) != 0) {
    switch (entry_tag) {
      case PR_SCORE_USER:
        tmp = va_arg(ap, char *);
        memset(entry.sce_user, '\0', sizeof(entry.sce_user));
        sstrncpy(entry.sce_user, tmp, sizeof(entry.sce_user));
        break;

      case PR_SCORE_CLIENT_ADDR:
        {
          char *remote_name = va_arg(ap, char *);
          p_in_addr_t *remote_ip = va_arg(ap, p_in_addr_t *);

          snprintf(entry.sce_client_addr, sizeof(entry.sce_client_addr),
            "%s [%s]", remote_name ? remote_name : "(unknown)",
            remote_ip ? inet_ntoa(*remote_ip) : "(unknown)");
          entry.sce_client_addr[sizeof(entry.sce_client_addr)-1] = '\0';
        }
        break;

      case PR_SCORE_CLASS:
        tmp = va_arg(ap, char *);
        memset(entry.sce_class, '\0', sizeof(entry.sce_class));
        sstrncpy(entry.sce_class, tmp, strlen(tmp));
        break;

      case PR_SCORE_CWD:
        tmp = va_arg(ap, char *);
        memset(entry.sce_cwd, '\0', sizeof(entry.sce_cwd));
        sstrncpy(entry.sce_cwd, tmp, sizeof(entry.sce_cwd));
        break;

      case PR_SCORE_CMD:
        {
          char *cmdstr = NULL;
          tmp = va_arg(ap, char *);
          cmdstr = handle_score_cmd(tmp, ap);

          memset(entry.sce_cmd, '\0', sizeof(entry.sce_cmd));
          sstrncpy(entry.sce_cmd, cmdstr, sizeof(entry.sce_cmd));
          tmp = va_arg(ap, void *);
        }
        break;

      case PR_SCORE_SERVER_IP:
        entry.sce_server_ip = va_arg(ap, p_in_addr_t *);
        break;

      case PR_SCORE_SERVER_PORT:
        entry.sce_server_port = va_arg(ap, int);
        break;

      case PR_SCORE_SERVER_ADDR:
        {
          p_in_addr_t *server_ip = va_arg(ap, p_in_addr_t *);
          int server_port = va_arg(ap, int);

          snprintf(entry.sce_server_addr, sizeof(entry.sce_server_addr),
            "%s:%d", server_ip ? inet_ntoa(*server_ip) : "(uknown)",
            server_port);
          entry.sce_server_addr[sizeof(entry.sce_server_addr)-1] = '\0';
        }
        break;

      case PR_SCORE_SERVER_NAME:
        tmp = va_arg(ap, char *);
        memset(entry.sce_server_name, '\0', sizeof(entry.sce_server_name));
        sstrncpy(entry.sce_server_name, tmp, sizeof(entry.sce_server_name));
        break;

      case PR_SCORE_BEGIN_IDLE:
        /* Ignore this */
        (void) va_arg(ap, time_t); 

        time(&entry.sce_begin_idle);
        break;

      case PR_SCORE_BEGIN_SESSION:
        /* Ignore this */
        (void) va_arg(ap, time_t);

        time(&entry.sce_begin_session); 
        break;

      case PR_SCORE_XFER_DONE:
        entry.sce_xfer_done = va_arg(ap, off_t);
        break;

      case PR_SCORE_XFER_SIZE:
        entry.sce_xfer_size = va_arg(ap, off_t);
        break;

      default:
        errno = EINVAL;
        return -1;
    }
  }

  /* Write-lock this entry */
  wlock_entry();
  if (write_entry() < 0)
    log_pri(LOG_NOTICE, "error writing scoreboard entry: %s", strerror(errno));
  unlock_entry();

  return 0;
}
