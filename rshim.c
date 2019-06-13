/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 *
 * Copyright 2019 Mellanox Technologies. All Rights Reserved.
 *
 */

#include <fuse/cuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#include <libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "rshim.h"

/* Maximum number of devices supported. */
#define RSHIM_MAX_DEV 64

/* RShim timer interval in milliseconds. */
#define RSHIM_TIMER_INTERVAL 1

/* Keepalive period in milliseconds. */
static int rshim_keepalive_period = 300;

/* Skip SW_RESET during booting. */
static int rshim_sw_reset_skip;

/* Advanced config options. */
static int rshim_adv_cfg;

#define RSH_KEEPALIVE_MAGIC_NUM 0x5089836482ULL

/* Circular buffer macros. */
#define CIRC_SPACE(head,tail,size) CIRC_CNT((tail),((head)+1),(size))
#define CIRC_SPACE_TO_END(head,tail,size) \
  ({int end = (size) - 1 - (head); \
    int n = (end + (tail)) & ((size)-1); \
    n <= end ? n : end+1;})
#define CIRC_CNT(head,tail,size) (((head) - (tail)) & ((size)-1))
#define CIRC_CNT_TO_END(head,tail,size) \
  ({int end = (size) - (tail); \
    int n = ((head) + end) & ((size)-1); \
    n < end ? n : end;})

#define read_empty(bd, chan) \
  (CIRC_CNT((bd)->read_fifo[chan].head, \
    (bd)->read_fifo[chan].tail, READ_FIFO_SIZE) == 0)
#define read_full(bd, chan) \
  (CIRC_SPACE((bd)->read_fifo[chan].head, \
    (bd)->read_fifo[chan].tail, READ_FIFO_SIZE) == 0)
#define read_space(bd, chan) \
  CIRC_SPACE((bd)->read_fifo[chan].head, \
    (bd)->read_fifo[chan].tail, READ_FIFO_SIZE)
#define read_cnt(bd, chan) \
  CIRC_CNT((bd)->read_fifo[chan].head, \
    (bd)->read_fifo[chan].tail, READ_FIFO_SIZE)
#define read_cnt_to_end(bd, chan) \
  CIRC_CNT_TO_END((bd)->read_fifo[chan].head, \
    (bd)->read_fifo[chan].tail, READ_FIFO_SIZE)
#define read_data_ptr(bd, chan) \
  ((bd)->read_fifo[chan].data + \
    ((bd)->read_fifo[chan].tail & (READ_FIFO_SIZE - 1)))
#define read_consume_bytes(bd, chan, nbytes) \
  ((bd)->read_fifo[chan].tail = \
    ((bd)->read_fifo[chan].tail + (nbytes)) & (READ_FIFO_SIZE - 1))
#define read_space_to_end(bd, chan) \
  CIRC_SPACE_TO_END((bd)->read_fifo[chan].head, \
    (bd)->read_fifo[chan].tail, READ_FIFO_SIZE)
#define read_space_offset(bd, chan) \
  ((bd)->read_fifo[chan].head & (READ_FIFO_SIZE - 1))
#define read_space_ptr(bd, chan) \
  ((bd)->read_fifo[chan].data + read_space_offset(bd, (chan)))
#define read_add_bytes(bd, chan, nbytes) \
  ((bd)->read_fifo[chan].head = \
    ((bd)->read_fifo[chan].head + (nbytes)) & (READ_FIFO_SIZE - 1))
#define read_reset(bd, chan) \
  ((bd)->read_fifo[chan].head = (bd)->read_fifo[chan].tail = 0)

#define write_empty(bd, chan) \
  (CIRC_CNT((bd)->write_fifo[chan].head, \
    (bd)->write_fifo[chan].tail, WRITE_FIFO_SIZE) == 0)
#define write_full(bd, chan) \
  (CIRC_SPACE((bd)->write_fifo[chan].head, \
    (bd)->write_fifo[chan].tail, WRITE_FIFO_SIZE) == 0)
#define write_space(bd, chan) \
  CIRC_SPACE((bd)->write_fifo[chan].head, \
    (bd)->write_fifo[chan].tail, WRITE_FIFO_SIZE)
#define write_cnt(bd, chan) \
  CIRC_CNT((bd)->write_fifo[chan].head, \
    (bd)->write_fifo[chan].tail, WRITE_FIFO_SIZE)
#define write_cnt_to_end(bd, chan) \
  CIRC_CNT_TO_END((bd)->write_fifo[chan].head, \
    (bd)->write_fifo[chan].tail, WRITE_FIFO_SIZE)
#define write_data_offset(bd, chan) \
  ((bd)->write_fifo[chan].tail & (WRITE_FIFO_SIZE - 1))
#define write_data_ptr(bd, chan) \
  ((bd)->write_fifo[chan].data + write_data_offset(bd, (chan)))
#define write_consume_bytes(bd, chan, nbytes) \
  ((bd)->write_fifo[chan].tail = ((bd)->write_fifo[chan].tail + (nbytes)) & \
    (WRITE_FIFO_SIZE - 1))
#define write_space_to_end(bd, chan) \
  CIRC_SPACE_TO_END((bd)->write_fifo[chan].head, \
    (bd)->write_fifo[chan].tail, WRITE_FIFO_SIZE)
#define write_space_ptr(bd, chan) \
  ((bd)->write_fifo[chan].data + \
    ((bd)->write_fifo[chan].head & (WRITE_FIFO_SIZE - 1)))
#define write_add_bytes(bd, chan, nbytes) \
  ((bd)->write_fifo[chan].head = ((bd)->write_fifo[chan].head + (nbytes)) & \
    (WRITE_FIFO_SIZE - 1))
#define write_reset(bd, chan) \
  ((bd)->write_fifo[chan].head = (bd)->write_fifo[chan].tail = 0)

/*
 * Tile-to-host bits (UART 0 scratchpad).
 */
/*
 * Output write pointer mask.  Note that this is the maximum size; the
 * write pointer may be smaller if requested by the host.
 */
#define CONS_RSHIM_T2H_OUT_WPTR_MASK     0x3FF

/* Tile is done mask. */
#define CONS_RSHIM_T2H_DONE_MASK         0x400

/*
 * Input read pointer mask.  Note that this is the maximum size; the read
 * pointer may be smaller if requested by the host.
 */
#define CONS_RSHIM_T2H_IN_RPTR_MASK      0x1FF800

/* Input read pointer shift. */
#define CONS_RSHIM_T2H_IN_RPTR_SHIFT     11

/* Tile is done mask. */
#define CONS_RSHIM_T2H_DONE_MASK         0x400

/* Number of words to send as sync-data (calculated by packet MTU). */
#define TMFIFO_MAX_SYNC_WORDS            (1536 / 8)

/* Terminal characteristics for newly created consoles. */
#define INIT_C_CC "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0"
static struct termios init_console_termios = {
  .c_iflag = INLCR | ICRNL,
  .c_oflag = OPOST | ONLCR,
  .c_cflag = B115200 | HUPCL | CLOCAL | CREAD | CS8,
  .c_lflag = ISIG | ICANON | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
  .c_line = 0,
  .c_cc = INIT_C_CC,
};

/* RShim global mutex. */
static pthread_mutex_t rshim_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Current timer ticks. */
static int rshim_timer_ticks;

/* File handler for the worker function. */
static int rshim_work_fd[2];

/* Current RShim backend name. */
static char *rshim_backend_name;

/* Global epoll handler. */
int rshim_epoll_fd;

/* Array of devices and device names. */
struct rshim_backend *rshim_devs[RSHIM_MAX_DEV];
char *rshim_dev_names[RSHIM_MAX_DEV];

/* Name of the sub-device types. */
char *rshim_dev_minor_names[RSH_DEV_TYPES] = {
    [RSH_DEV_TYPE_RSHIM] = "rshim",
    [RSH_DEV_TYPE_BOOT] = "boot",
    [RSH_DEV_TYPE_TMFIFO] = "console",
    [RSH_DEV_TYPE_MISC] = "misc",
};

int rshim_log_level = 3;

/* FIFO reset. */
static void rshim_fifo_reset(struct rshim_backend *bd);

/* Global lock / unlock. */

void rshim_lock(void)
{
    pthread_mutex_lock(&rshim_mutex);
}

void rshim_unlock(void)
{
    pthread_mutex_unlock(&rshim_mutex);
}

static int rshim_fd_full_read(int fd, void *data, int len)
{
  int cc, total = 0;
  char *buf = (char *)data;

  while (len > 0) {
    cc = read(fd, buf, len);
    if (cc < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }

    if (cc == 0)
      break;

    buf += cc;
    total += cc;
    len -= cc;
  }

  return total;
}

static int rshim_fd_full_write(int fd, void *data, int len)
{
  int total = 0;
  char *buf = (char *)data;

  while (len > 0) {
    ssize_t written = write(fd, buf, len);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return written;
    }
    total += written;
    buf += written;
    len -= written;
  }

  return total;
}

/* Wake up the worker function. */
static void rshim_work_signal(struct rshim_backend *bd)
{
  if (__sync_bool_compare_and_swap(&bd->work_pending, false, true))
    rshim_fd_full_write(rshim_work_fd[1], &bd, sizeof(bd));
}

/*
 * Read some bytes from RShim.
 *
 * The provided buffer size should be multiple of 8 bytes. If not, the
 * leftover bytes (which presumably were sent as NUL bytes by the sender)
 * will be discarded.
 */
static ssize_t rshim_read_default(struct rshim_backend *bd, int devtype,
                                  char *buf, size_t count)
{
  int retval, total = 0, avail = 0;
  uint64_t reg;

  /* Read is only supported for RShim TMFIFO. */
  if (devtype != RSH_DEV_TYPE_TMFIFO) {
    RSHIM_ERR("bad devtype %d\n", devtype);
    return -EINVAL;
  }

  if (bd->is_boot_open)
    return 0;

  while (total < count) {
    if (avail == 0) {
      retval = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_TM_TILE_TO_HOST_STS, &reg);
      if (retval < 0)
        break;
      avail = reg & RSH_TM_TILE_TO_HOST_STS__COUNT_MASK;
      if (avail == 0)
        break;
    }
    retval = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_TM_TILE_TO_HOST_DATA, &reg);
    if (retval < 0)
      break;

    /*
     * Convert it to little endian before sending to RShim. The other side
     * should decode it as little endian as well which is usually the default
     * case.
     */
    reg = le64toh(reg);
    if (total + sizeof(reg) <= count) {
      *(uint64_t *)buf = reg;
      buf += sizeof(reg);
      total += sizeof(reg);
    } else {
      /* Copy the rest data which is less than 8 bytes. */
      memcpy(buf, &reg, count - total);
      total = count;
      break;
    }
    avail--;
  }

  return total;
}

/*
 * Write some bytes to the RShim backend.
 *
 * If count is not multiple of 8-bytes, the data will be padded to 8-byte
 * aligned which is required by RShim HW.
 */
static ssize_t rshim_write_delayed(struct rshim_backend *bd, int devtype,
                                   const char *buf, size_t count)
{
  uint64_t reg;
  char pad_buf[sizeof(uint64_t)] = { 0 };
  int size_addr, size_mask, data_addr, max_size;
  int retval, avail = 0, byte_cnt = 0, retry;

  switch (devtype) {
  case RSH_DEV_TYPE_TMFIFO:
    if (bd->is_boot_open)
      return count;
    size_addr = RSH_TM_HOST_TO_TILE_STS;
    size_mask = RSH_TM_HOST_TO_TILE_STS__COUNT_MASK;
    data_addr = RSH_TM_HOST_TO_TILE_DATA;
    retval = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_TM_HOST_TO_TILE_CTL, &reg);
    if (retval < 0) {
      RSHIM_ERR("read_rshim error %d\n", retval);
      return retval;
    }
    max_size = (reg >> RSH_TM_HOST_TO_TILE_CTL__MAX_ENTRIES_SHIFT) &
               RSH_TM_HOST_TO_TILE_CTL__MAX_ENTRIES_RMASK;
    break;

  case RSH_DEV_TYPE_BOOT:
    size_addr = RSH_BOOT_FIFO_COUNT;
    size_mask = RSH_BOOT_FIFO_COUNT__BOOT_FIFO_COUNT_MASK;
    data_addr = RSH_BOOT_FIFO_DATA;
    max_size = RSH_BOOT_FIFO_SIZE;
    break;

  default:
    RSHIM_ERR("bad devtype %d\n", devtype);
    return -EINVAL;
  }

  while (byte_cnt < count) {
    /* Check the boot cancel condition. */
    if (devtype == RSH_DEV_TYPE_BOOT && !bd->boot_work_buf)
      break;

    /* Add padding if less than 8 bytes left. */
    if (byte_cnt + sizeof(uint64_t) > count) {
      memcpy(pad_buf, buf, count - byte_cnt);
      buf = (const char *)pad_buf;
    }

    retry = 0;
    while (avail <= 0) {
      /* Calculate available space in words. */
      retval = bd->read_rshim(bd, RSHIM_CHANNEL, size_addr, &reg);
      if (retval < 0) {
        RSHIM_ERR("read_rshim error %d\n", retval);
        break;
      }
      avail = max_size - (int)(reg & size_mask) - 8;
      if (avail > 0)
        break;

      /*
       * Retry 100s, or else return failure since the other side seems not
       * to be responding.
       */
      if (++retry > 100000)
        return -ETIMEDOUT;
      pthread_mutex_unlock(&bd->mutex);
      usleep(1000);
      pthread_mutex_lock(&bd->mutex);
    }

    reg = *(uint64_t *)buf;
    /*
     * Convert to little endian before sending to RShim. The
     * receiving side should call le64toh() to convert it back.
     */
    reg = htole64(reg);
    retval = bd->write_rshim(bd, RSHIM_CHANNEL, data_addr, reg);
    if (retval < 0) {
      RSHIM_ERR("write_rshim error %d\n", retval);
      break;
    }
    buf += sizeof(reg);
    byte_cnt += sizeof(reg);
    avail--;
  }

  /* Return number shouldn't count the padded bytes. */
  return (byte_cnt > count) ? count : byte_cnt;
}

static ssize_t rshim_write_default(struct rshim_backend *bd, int devtype,
                                   const char *buf, size_t count)
{
  int retval;

  switch (devtype) {
  case RSH_DEV_TYPE_TMFIFO:
    if (bd->is_boot_open)
      return count;

    /* Set the flag so there is only one outstanding request. */
    bd->spin_flags |= RSH_SFLG_WRITING;

    /* Wake up the worker. */
    bd->fifo_work_buf = (char *)buf;
    bd->fifo_work_buf_len = count;
    bd->fifo_work_devtype = devtype;
    bd->has_fifo_work = 1;
    rshim_work_signal(bd);
    return 0;

  case RSH_DEV_TYPE_BOOT:
    bd->boot_work_buf_len = count;
    bd->boot_work_buf_actual_len = 0;
    bd->boot_work_buf = (char *)buf;
    rshim_work_signal(bd);

    retval = pthread_cond_wait(&bd->boot_write_complete_cond, &bd->mutex);
    /* Cancel the request if interrupted. */
    if (retval)
      bd->boot_work_buf = NULL;

    return bd->boot_work_buf_actual_len;

  default:
    RSHIM_ERR("bad devtype %d\n", devtype);
    return -EINVAL;
  }
}

/* Boot file operations routines */

/*
 * Wait for boot to complete, if necessary.  Return 0 if the boot is done
 * and it's safe to continue, an error code if something went wrong.  Note
 * that this routine must be called with the device mutex held.  If it
 * returns successfully, the mutex will still be held (although it may have
 * been dropped and reacquired); if it returns unsuccessfully the mutex
 * will have been dropped.
 */
static int wait_for_boot_done(struct rshim_backend *bd)
{
  int retval;

  if (!bd->has_reprobe || rshim_sw_reset_skip)
    return 0;

  if (!bd->has_rshim || bd->is_booting) {
    while (bd->is_booting) {
      RSHIM_INFO("boot write, waiting for re-probe\n");
      /*
       * FIXME: might we want a timeout here, too?  If the reprobe takes a very
       * long time, something's probably wrong.  Maybe a couple of minutes?
       */
      retval = pthread_cond_wait(&bd->boot_complete_cond, &bd->mutex);
      if (retval)
        return retval;
    }

    if (!bd->has_rshim)
      return -ENODEV;
  }

  return 0;
}

static void rshim_boot_open(fuse_req_t req, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  int i, rc;

  pthread_mutex_lock(&bd->mutex);

  if (bd->is_boot_open) {
    RSHIM_INFO("can't boot, boot file already open\n");
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, -EBUSY);
    return;
  }

  if (!bd->has_rshim) {
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, -NODEV);
    return;
  }

  RSHIM_INFO("begin booting\n");
  bd->is_booting = 1;

  /*
   * Before we reset the chip, make sure we don't have any
   * outstanding writes, and flush the write and read FIFOs. (Note
   * that we can't have any outstanding reads, since we kill those
   * upon release of the TM FIFO file.)
   */
  if (bd->cancel)
    bd->cancel(bd, RSH_DEV_TYPE_TMFIFO, true);
  bd->read_buf_bytes = 0;
  bd->read_buf_pkt_rem = 0;
  bd->read_buf_pkt_padding = 0;
  pthread_mutex_lock(&bd->ringlock);
  bd->spin_flags &= ~RSH_SFLG_WRITING;
  for (i = 0; i < TMFIFO_MAX_CHAN; i++) {
    read_reset(bd, i);
    write_reset(bd, i);
  }
  pthread_mutex_unlock(&bd->ringlock);

  /* Set RShim (external) boot mode. */
  rc = bd->write_rshim(bd, RSHIM_CHANNEL, RSH_BOOT_CONTROL,
                       RSH_BOOT_CONTROL__BOOT_MODE_VAL_NONE);
  if (rc) {
    RSHIM_ERR("boot_open: error %d writing boot control\n", rc);
    bd->is_booting = 0;
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, rc);
    return;
  }

  if (rshim_sw_reset_skip) {
    bd->is_boot_open = 1;
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, 0);
  }

  bd->is_boot_open = 1;

  /* SW reset. */
  rc = bd->write_rshim(bd, RSHIM_CHANNEL, RSH_RESET_CONTROL,
                       RSH_RESET_CONTROL__RESET_CHIP_VAL_KEY);

  /* Reset the TmFifo. */
  rshim_fifo_reset(bd);

  /*
   * Note that occasionally, we get various errors on writing to
   * the reset register.  This appears to be caused by the chip
   * actually resetting before the response goes out, or perhaps by
   * our noticing the device unplug before we've seen the response.
   * Either way, the chip _does_ actually reset, so we just ignore
   * the error.  Should we ever start getting these errors without
   * the chip being reset, we'll have to figure out how to handle
   * this more intelligently.  (One potential option is to not reset
   * directly, but to set up a down counter to do the reset, but that
   * seems kind of kludgy, especially since Tile software might also
   * be trying to use the down counter.)
   */
  if (rc && rc != -EPROTO && rc != -ESHUTDOWN &&
#ifdef RSH_USB_BMC
    /*
     * The host driver on the BMC sometimes produces EOVERFLOW on
     * reset.  It also seems to have seems to have some sort of bug
     * which makes it return more bytes than we actually wrote!  In
     * that case we're returning EBADE.
     */
    rc != -EOVERFLOW && rc != -EBADE &&
#endif
    rc != -ETIMEDOUT && rc != -EPIPE) {
    RSHIM_ERR("boot_open: error %d writing reset control\n", rc);
    pthread_mutex_unlock(&bd->mutex);
    bd->is_boot_open = 0;

    fuse_reply_err(req, rc);
    return;
  }

  if (rc)
    RSHIM_ERR("boot_open: got error %d on reset write\n", rc);

  pthread_mutex_unlock(&bd->mutex);

  if (!bd->has_reprobe)
    sleep(10);

  fuse_reply_open(req, fi);
}

static void rshim_boot_write(fuse_req_t req, const char *user_buffer,
                             size_t count, off_t off, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  size_t bytes_written = 0, bytes_left;
  int retval = 0, whichbuf = 0;

  /*
   * Hardware requires that we send multiples of 8 bytes.  Ideally
   * we'd handle the case where we got unaligned writes by
   * accumulating the residue somehow, but none of our clients
   * typically do this, so we just clip the size to prevent any
   * inadvertent errors from causing hardware problems.
   */
  bytes_left = count & (-((size_t)8));
  if (!bytes_left) {
    fuse_reply_write(req, 0);
    return;
  }

  pthread_mutex_lock(&bd->mutex);
  if (bd->is_in_boot_write) {
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, -EBUSY);
    return;
  }

  retval = wait_for_boot_done(bd);
  if (retval) {
    RSHIM_ERR("boot_write: wait for boot failed, err %d\n", retval);
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, retval);
    return;
  }

  /*
   * We're going to drop the mutex while we wait for any outstanding
   * write to complete; this keeps another thread from getting in here
   * while we do that.
   */
  bd->is_in_boot_write = 1;

  while (bytes_left) {
    size_t buf_bytes = MIN(BOOT_BUF_SIZE, bytes_left);
    char *buf = bd->boot_buf[whichbuf];

    whichbuf ^= 1;
    memcpy(buf, user_buffer, buf_bytes);

    retval = bd->write(bd, RSH_DEV_TYPE_BOOT, buf, buf_bytes);
    if (retval > 0) {
      bytes_left -= retval;
      user_buffer += retval;
      bytes_written += retval;
    } else if (retval == 0) {
      /* Wait for some time instead of busy polling. */
      usleep(1000);
      continue;
    }
    if (retval != buf_bytes)
      break;
  }

  bd->is_in_boot_write = 0;
  pthread_mutex_unlock(&bd->mutex);

  /*
   * Return an error in case the 'count' is not multiple of 8 bytes.
   * At this moment, the truncated data has already been sent to
   * the BOOT fifo and hopefully it could still boot the chip.
   */
  if (count % 8 != 0) {
    fuse_reply_err(req, -EINVAL);
    return;
  }

  if (bytes_written >= 0)
    fuse_reply_write(req, bytes_written);
  else
    fuse_reply_err(req, retval);
}

static void rshim_boot_release(fuse_req_t req, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  int retval;

  /* Restore the boot mode register. */
  retval = bd->write_rshim(bd, RSHIM_CHANNEL,
                           RSH_BOOT_CONTROL,
                           RSH_BOOT_CONTROL__BOOT_MODE_VAL_EMMC);
  if (retval)
    RSHIM_ERR("couldn't set boot_control, err %d\n", retval);

  pthread_mutex_lock(&bd->mutex);
  bd->is_boot_open = 0;
  rshim_work_signal(bd);
  pthread_mutex_unlock(&bd->mutex);

  fuse_reply_err(req, 0);
}

static const struct cuse_lowlevel_ops rshim_boot_fops = {
    .open = rshim_boot_open,
    .write = rshim_boot_write,
    .release = rshim_boot_release,
};

/* FIFO common routines */

/*
 * Signal an error on the FIFO, and wake up anyone who might need to know
 * about it.
 */
static void rshim_fifo_err(struct rshim_backend *bd, int err)
{
  int i;

  bd->tmfifo_error = err;
  pthread_cond_broadcast(&bd->fifo_write_complete_cond);
  for (i = 0; i < TMFIFO_MAX_CHAN; i++) {
    pthread_cond_broadcast(&bd->read_fifo[i].operable);
    pthread_cond_broadcast(&bd->write_fifo[i].operable);
  }
}

static int rshim_fifo_tx_avail(struct rshim_backend *bd)
{
  uint64_t word;
  int ret, max_size, avail;

  /* Get FIFO max size. */
  ret = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_TM_HOST_TO_TILE_CTL, &word);
  if (ret) {
    RSHIM_ERR("read_rshim error %d", ret);
    return ret;
  }
  max_size = (word >> RSH_TM_HOST_TO_TILE_CTL__MAX_ENTRIES_SHIFT) &
             RSH_TM_HOST_TO_TILE_CTL__MAX_ENTRIES_RMASK;

  /* Calculate available size. */
  ret = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_TM_HOST_TO_TILE_STS, &word);
  if (ret) {
    RSHIM_ERR("read_rshim error %d", ret);
    return ret;
  }
  avail = max_size - (int)(word & RSH_TM_HOST_TO_TILE_STS__COUNT_MASK) - 1;

  return avail;
}

static int rshim_fifo_sync(struct rshim_backend *bd)
{
  int i, avail, ret;
  union rshim_tmfifo_msg_hdr hdr;

  avail = rshim_fifo_tx_avail(bd);
  if (avail < 0)
    return avail;

  hdr.data = 0;
  hdr.type = VIRTIO_ID_NET;

  for (i = 0; i < avail; i++) {
    ret = bd->write_rshim(bd, RSHIM_CHANNEL, RSH_TM_HOST_TO_TILE_DATA,
                          hdr.data);
    if (ret)
      return ret;
  }

  return 0;
}

/* Just adds up all the bytes of the header. */
static uint8_t rshim_fifo_ctrl_checksum(union rshim_tmfifo_msg_hdr *hdr)
{
  uint8_t checksum = 0;
  int i;

  for (i = 0; i < sizeof(*hdr); i++)
    checksum += ((uint8_t *)hdr)[i];

  return checksum;
}

static void rshim_fifo_ctrl_update_checksum(union rshim_tmfifo_msg_hdr *hdr)
{
  uint8_t checksum;

  hdr->checksum = 0;
  checksum = rshim_fifo_ctrl_checksum(hdr);
  hdr->checksum = ~checksum + 1;
}

static bool rshim_fifo_ctrl_verify_checksum(union rshim_tmfifo_msg_hdr *hdr)
{
  uint8_t checksum = rshim_fifo_ctrl_checksum(hdr);

  return checksum ? false : true;
}

static void rshim_fifo_ctrl_rx(struct rshim_backend *bd,
             union rshim_tmfifo_msg_hdr *hdr)
{
  if (!rshim_fifo_ctrl_verify_checksum(hdr))
    return;

  switch (hdr->type) {
  case TMFIFO_MSG_MAC_1:
    memcpy(bd->peer_mac, hdr->mac, 3);
    break;
  case TMFIFO_MSG_MAC_2:
    memcpy(bd->peer_mac + 3, hdr->mac, 3);
    break;
  case TMFIFO_MSG_PXE_ID:
    bd->pxe_client_id = ntohl(hdr->pxe_id);
    /* Last info to receive, set the flag. */
    bd->peer_ctrl_resp = 1;
    pthread_cond_broadcast(&bd->ctrl_wait_cond);
    break;
  default:
    return;
  }
}

static int rshim_fifo_ctrl_tx(struct rshim_backend *bd)
{
  union rshim_tmfifo_msg_hdr hdr;
  int len = 0;

  if (bd->peer_mac_set) {
    bd->peer_mac_set = 0;
    hdr.data = 0;
    hdr.type = TMFIFO_MSG_MAC_1;
    memcpy(hdr.mac, bd->peer_mac, 3);
    rshim_fifo_ctrl_update_checksum(&hdr);
    memcpy(bd->write_buf, &hdr.data, sizeof(hdr.data));
    hdr.type = TMFIFO_MSG_MAC_2;
    memcpy(hdr.mac, bd->peer_mac + 3, 3);
    rshim_fifo_ctrl_update_checksum(&hdr);
    memcpy(bd->write_buf + sizeof(hdr.data), &hdr.data, sizeof(hdr.data));
    len = sizeof(hdr.data) * 2;
  } else if (bd->peer_pxe_id_set) {
    bd->peer_pxe_id_set = 0;
    hdr.data = 0;
    hdr.type = TMFIFO_MSG_PXE_ID;
    hdr.pxe_id = htonl(bd->pxe_client_id);
    rshim_fifo_ctrl_update_checksum(&hdr);
    memcpy(bd->write_buf, &hdr.data, sizeof(hdr.data));
    len = sizeof(hdr.data);
  } else if (bd->peer_ctrl_req) {
    bd->peer_ctrl_req = 0;
    hdr.data = 0;
    hdr.type = TMFIFO_MSG_CTRL_REQ;
    rshim_fifo_ctrl_update_checksum(&hdr);
    memcpy(bd->write_buf, &hdr.data, sizeof(hdr.data));
    len = sizeof(hdr.data);
  }

  return len;
}

static void rshim_fifo_input_notify(struct rshim_backend *bd)
{
  int chan = bd->rx_chan;

  RSHIM_DBG("rshim_fifo_input: woke up readable chan %d\n", chan);

  if (bd->rx_poll_handle[chan])
    fuse_lowlevel_notify_poll(bd->rx_poll_handle[chan]);

  pthread_cond_broadcast(&bd->read_fifo[chan].operable);
}

/* Drain the read buffer, and start another read/interrupt if needed. */
static void rshim_fifo_input(struct rshim_backend *bd)
{
  union rshim_tmfifo_msg_hdr *hdr;
  bool rx_avail = false;

  if (bd->is_boot_open)
    return;

again:
  while (bd->read_buf_next < bd->read_buf_bytes) {
    int copysize;

    /*
     * If we're at the start of a packet, then extract the
     * header, and update our count of bytes remaining in the
     * packet.
     */
    if (bd->read_buf_pkt_rem == 0) {
      /* Make sure header is received. */
      if (bd->read_buf_next + sizeof(*hdr) > bd->read_buf_bytes)
        break;

      RSHIM_DBG("read_buf_next %d\n", bd->read_buf_next);

      hdr = (union rshim_tmfifo_msg_hdr *)&bd->read_buf[bd->read_buf_next];

      bd->read_buf_pkt_rem = ntohs(hdr->len) + sizeof(*hdr);
      bd->read_buf_pkt_padding = (8 - (bd->read_buf_pkt_rem & 7)) & 7;
      if (hdr->type == VIRTIO_ID_NET)
        bd->rx_chan = TMFIFO_NET_CHAN;
      else if (hdr->type == VIRTIO_ID_CONSOLE) {
        bd->rx_chan = TMFIFO_CONS_CHAN;
        /* Strip off the message header for console. */
        bd->read_buf_next += sizeof(*hdr);
        bd->read_buf_pkt_rem -= sizeof(*hdr);
        if (bd->read_buf_pkt_rem == 0)
          continue;
      } else {
        RSHIM_DBG("bad type %d, drop it\n", hdr->type);
        bd->read_buf_pkt_rem = 0;
        bd->read_buf_pkt_padding = 0;
        if (hdr->len == 0) {
          bd->read_buf_next += sizeof(*hdr);
          rshim_fifo_ctrl_rx(bd, hdr);
          continue;
        } else {
          RSHIM_DBG("bad type %d, drop it", hdr->type);
          bd->read_buf_next = bd->read_buf_bytes;
          break;
        }
      }

      RSHIM_DBG("drain: hdr, nxt %d rem %d chn %d\n",
                bd->read_buf_next, bd->read_buf_pkt_rem, bd->rx_chan);
      bd->drop = 0;
    }

    if (bd->rx_chan == TMFIFO_CONS_CHAN &&
        !(bd->spin_flags & RSH_SFLG_CONS_OPEN)) {
      /*
       * If data is coming in for a closed console channel, we want to just
       * throw it away. Resetting the channel every time through this loop is
       * a relatively cheap way to do that.  Note that this works because the
       * read buffer is no larger than the read FIFO; thus, we know that if
       * we reset it here, we will always be able to drain the read buffer of
       * any console data, and will then launch another read.
       */
      read_reset(bd, TMFIFO_CONS_CHAN);
      bd->drop = 1;
    } else if (bd->rx_chan == TMFIFO_NET_CHAN && bd->net_notify_fd[0] < 0) {
      /* Drop if networking is not enabled. */
      read_reset(bd, TMFIFO_NET_CHAN);
      bd->drop = 1;
    }

    copysize = MIN(bd->read_buf_pkt_rem,
                   bd->read_buf_bytes - bd->read_buf_next);
    copysize = MIN(copysize, read_space_to_end(bd, bd->rx_chan));

    RSHIM_DBG("drain: copysize %d, head %d, tail %d, remaining %d\n",
              copysize,
              bd->read_fifo[bd->rx_chan].head,
              bd->read_fifo[bd->rx_chan].tail,
              bd->read_buf_pkt_rem);

    if (copysize == 0) {
      /* We have data, but no space to put it in, so we're done. */
      RSHIM_DBG("drain: no more space in channel %d\n",
                 bd->rx_chan);
      break;
    }

    if (!bd->drop) {
      memcpy(read_space_ptr(bd, bd->rx_chan), &bd->read_buf[bd->read_buf_next],
             copysize);
      read_add_bytes(bd, bd->rx_chan, copysize);
    }

    bd->read_buf_next += copysize;
    bd->read_buf_pkt_rem -= copysize;

    rshim_fifo_input_notify(bd);

    if (bd->read_buf_pkt_rem <= 0) {
      bd->read_buf_next = bd->read_buf_next + bd->read_buf_pkt_padding;
      rx_avail = true;
    }
  }

  /*
   * We've processed all of the data we can, so now we decide if we
   * need to launch another I/O.  If there's still data in the read
   * buffer, or if we're already reading, don't launch any new
   * operations.  If an interrupt just completed, and said there was
   * data, or the last time we did a read we got some data, then do
   * another read.  Otherwise, do an interrupt.
   */
  if (bd->read_buf_next < bd->read_buf_bytes ||
      (bd->spin_flags & RSH_SFLG_READING)) {
    /* We're doing nothing. */
    RSHIM_DBG("fifo_input: no new read: %s\n",
              (bd->read_buf_next < bd->read_buf_bytes) ?
              "have data" : "already reading");
  } else {
    int len;

    /* Process it if more data is received. */
    len = bd->read(bd, RSH_DEV_TYPE_TMFIFO, (char *)bd->read_buf,
                   READ_BUF_SIZE);
    if (len > 0) {
      bd->read_buf_bytes = len;
      bd->read_buf_next = 0;
      goto again;
    }
  }

  if (rx_avail && bd->rx_chan == TMFIFO_NET_CHAN) {
    if (__sync_bool_compare_and_swap(&bd->net_rx_pending, false, true))
      write(bd->net_notify_fd[1], &rx_avail, 1);
  }
}

ssize_t rshim_fifo_read(struct rshim_backend *bd, char *buffer,
                        size_t count, int chan, bool nonblock)
{
  size_t rd_cnt = 0;

  pthread_mutex_lock(&bd->mutex);

  while (count) {
    size_t readsize;
    int pass1;
    int pass2;

    RSHIM_DBG("fifo_read, top of loop, remaining count %zd\n", count);

    /*
     * We check this each time through the loop since the
     * device could get disconnected while we're waiting for
     * more data in the read FIFO.
     */
    if (!bd->has_tm) {
      pthread_mutex_unlock(&bd->mutex);
      RSHIM_DBG("fifo_read: returning %zd/ENODEV\n", rd_cnt);
      return rd_cnt ? rd_cnt : -ENODEV;
    }

    if (bd->tmfifo_error) {
      pthread_mutex_unlock(&bd->mutex);
      RSHIM_DBG("fifo_read: returning %zd/%d\n", rd_cnt, bd->tmfifo_error);
      return rd_cnt ? rd_cnt : bd->tmfifo_error;
    }

    if (read_empty(bd, chan)) {
      RSHIM_DBG("fifo_read: fifo empty\n");
      if (rd_cnt || nonblock) {
        if (rd_cnt == 0) {
          pthread_mutex_lock(&bd->ringlock);
          rshim_fifo_input(bd);
          pthread_mutex_unlock(&bd->ringlock);
        }
        pthread_mutex_unlock(&bd->mutex);
        RSHIM_DBG("fifo_read: returning %zd/EAGAIN\n", rd_cnt);
        return rd_cnt ? rd_cnt : -EAGAIN;
      }

      RSHIM_DBG("fifo_read: waiting for readable chan %d\n", chan);
      while (read_empty(bd, chan)) {
        if (pthread_cond_wait(&bd->read_fifo[chan].operable, &bd->mutex)) {
          RSHIM_DBG("fifo_read: returning ERESTARTSYS\n");
          pthread_mutex_unlock(&bd->mutex);
          return -EINTR;
        }
      }

      /*
       * Since we dropped the mutex, we must make sure our interface is still
       * there before we do anything else.
       */
      continue;
    }

    /* Figure out how many bytes we will transfer on this pass. */
    pthread_mutex_lock(&bd->ringlock);
    readsize = MIN(count, (size_t)read_cnt(bd, chan));
    pass1 = MIN(readsize, (size_t)read_cnt_to_end(bd, chan));
    pass2 = readsize - pass1;
    pthread_mutex_unlock(&bd->ringlock);

    RSHIM_DBG("fifo_read: readsize %zd, head %d, tail %d\n",
              readsize, bd->read_fifo[chan].head,
              bd->read_fifo[chan].tail);

    memcpy(buffer, read_data_ptr(bd, chan), pass1);
    if (pass2)
      memcpy(buffer + pass1, bd->read_fifo[chan].data, pass2);

    pthread_mutex_lock(&bd->ringlock);
    read_consume_bytes(bd, chan, readsize);

    /* Check if there is any more incoming data. */
    rshim_fifo_input(bd);
    pthread_mutex_unlock(&bd->ringlock);

    count -= readsize;
    buffer += readsize;
    rd_cnt += readsize;
    RSHIM_DBG("fifo_read: transferred %zd bytes\n", readsize);
  }

  pthread_mutex_unlock(&bd->mutex);

  RSHIM_DBG("fifo_read: returning %zd\n", rd_cnt);
  return rd_cnt;
}

static void rshim_fifo_output(struct rshim_backend *bd)
{
  int writesize, write_buf_next = 0;
  int write_avail = WRITE_BUF_SIZE - write_buf_next;
  int numchan = TMFIFO_MAX_CHAN;
  int chan, chan_offset;

  /* If we're already writing, we have nowhere to put data. */
  if (bd->spin_flags & RSH_SFLG_WRITING)
    return;

  if (!bd->write_buf_pkt_rem) {
    /* Send control messages. */
    writesize = rshim_fifo_ctrl_tx(bd);
    if (writesize > 0) {
      write_avail -= writesize;
      write_buf_next += writesize;
    }
  }

  /* Walk through all the channels, sending as much data as possible. */
  for (chan_offset = 0; chan_offset < numchan; chan_offset++) {
    /*
     * Pick the current channel if not done, otherwise round-robin
     * to the next channel.
     */
    if (bd->write_buf_pkt_rem > 0)
      chan = bd->tx_chan;
    else {
      uint16_t cur_len;
      union rshim_tmfifo_msg_hdr *hdr = &bd->msg_hdr;

      chan = bd->tx_chan = (bd->tx_chan + 1) % numchan;
      cur_len = write_cnt(bd, chan);

      /*
       * Set up message header for console data which is byte
       * stream. Network packets already have the message
       * header included.
       */
      if (chan == TMFIFO_CONS_CHAN) {
        if (cur_len == 0)
          continue;
        hdr->data = 0;
        hdr->type = VIRTIO_ID_CONSOLE;
        hdr->len = htons(cur_len);
      } else {
        int pass1;

        if (cur_len < sizeof(union rshim_tmfifo_msg_hdr))
          continue;

        pass1 = write_cnt_to_end(bd, chan);
        if (pass1 >= sizeof(*hdr)) {
          hdr = (union rshim_tmfifo_msg_hdr *) write_data_ptr(bd, chan);
        } else {
          memcpy(hdr, write_data_ptr(bd, chan), pass1);
          memcpy((uint8_t *)hdr + pass1, bd->write_fifo[chan].data,
                 sizeof(*hdr) - pass1);
        }
      }

      bd->write_buf_pkt_rem = ntohs(hdr->len) + sizeof(*hdr);
    }

    /* Send out the packet header for the console data. */
    if (chan == TMFIFO_CONS_CHAN &&
        bd->write_buf_pkt_rem > ntohs(bd->msg_hdr.len)) {
        union rshim_tmfifo_msg_hdr *hdr = &bd->msg_hdr;
        int left = bd->write_buf_pkt_rem - ntohs(hdr->len);
        uint8_t *pos = (uint8_t *)hdr + sizeof(*hdr) - left;

        writesize = MIN(write_avail, left);
        memcpy(&bd->write_buf[write_buf_next], pos, writesize);
        write_buf_next += writesize;
        bd->write_buf_pkt_rem -= writesize;
        write_avail -= writesize;

        /*
         * Don't continue if no more space for the header. It'll be picked up
         * next time.
         */
        if (left != writesize)
          break;
    }

    writesize = MIN(write_avail, (int)write_cnt(bd, chan));
    writesize = MIN(writesize, bd->write_buf_pkt_rem);

    /*
     * The write size should be aligned to 8 bytes unless for the
     * last block, which will be padded at the end.
     */
    if (bd->write_buf_pkt_rem != writesize)
      writesize &= -8;

    if (writesize > 0) {
      int pass1;
      int pass2;

      pass1 = MIN(writesize, (int)write_cnt_to_end(bd, chan));
      pass2 = writesize - pass1;

      RSHIM_DBG("fifo_output: chan %d, writesize %d, next %d,"
                 " head %d, tail %d\n",
                 chan, writesize, write_buf_next,
                 bd->write_fifo[chan].head,
                 bd->write_fifo[chan].tail);

      memcpy(&bd->write_buf[write_buf_next], write_data_ptr(bd, chan), pass1);
      memcpy(&bd->write_buf[write_buf_next + pass1],
             bd->write_fifo[chan].data, pass2);

      write_consume_bytes(bd, chan, writesize);
      write_buf_next += writesize;
      bd->write_buf_pkt_rem -= writesize;
      /* Add padding at the end. */
      if (bd->write_buf_pkt_rem == 0)
        write_buf_next = (write_buf_next + 7) & -8;
      write_avail = WRITE_BUF_SIZE - write_buf_next;

      pthread_cond_broadcast(&bd->write_fifo[chan].operable);
      RSHIM_DBG("rshim_fifo_output: woke up writable chan %d\n", chan);
    }
  }

  /* Drop the data if it is still booting. */
  if (bd->is_boot_open)
    return;

  /* If we actually put anything in the buffer, send it. */
  if (write_buf_next)
    bd->write(bd, RSH_DEV_TYPE_TMFIFO, (char *)bd->write_buf, write_buf_next);
}

int rshim_fifo_alloc(struct rshim_backend *bd)
{
  int i;

  for (i = 0; i < TMFIFO_MAX_CHAN; i++) {
    if (!bd->read_fifo[i].data)
      bd->read_fifo[i].data = malloc(READ_FIFO_SIZE);

    if (!bd->write_fifo[i].data)
      bd->write_fifo[i].data = malloc(WRITE_FIFO_SIZE);
  }

  return 0;
}

static void rshim_fifo_reset(struct rshim_backend *bd)
{
  int i;

  bd->read_buf_bytes = 0;
  bd->read_buf_pkt_rem = 0;
  bd->read_buf_next = 0;
  bd->read_buf_pkt_padding = 0;
  bd->write_buf_pkt_rem = 0;
  bd->rx_chan = bd->tx_chan = 0;

  pthread_mutex_lock(&bd->ringlock);
  bd->spin_flags &= ~(RSH_SFLG_WRITING | RSH_SFLG_READING);
  for (i = 0; i < TMFIFO_MAX_CHAN; i++) {
    read_reset(bd, i);
    write_reset(bd, i);
  }
  pthread_mutex_unlock(&bd->ringlock);
}

void rshim_fifo_free(struct rshim_backend *bd)
{
  int i;

  for (i = 0; i < TMFIFO_MAX_CHAN; i++) {
    free(bd->read_fifo[i].data);
    bd->read_fifo[i].data = NULL;
    free(bd->write_fifo[i].data);
    bd->write_fifo[i].data = NULL;
  }

  rshim_fifo_reset(bd);
  bd->has_tm = 0;
}

ssize_t rshim_fifo_write(struct rshim_backend *bd, const char *buffer,
                         size_t count, int chan, bool nonblock)
{
  size_t wr_cnt = 0;

  pthread_mutex_lock(&bd->mutex);

  while (count) {
    size_t writesize;
    int pass1;
    int pass2;

    /*
     * We check this each time through the loop since the
     * device could get disconnected while we're waiting for
     * more space in the write buffer.
     */
    if (!bd->has_tm) {
      pthread_mutex_unlock(&bd->mutex);
      RSHIM_DBG("fifo_write: returning %zd/ENODEV\n", wr_cnt);
      return wr_cnt ? wr_cnt : -ENODEV;
    }

    if (bd->tmfifo_error) {
      pthread_mutex_unlock(&bd->mutex);
      RSHIM_DBG("fifo_write: returning %zd/%d\n", wr_cnt, bd->tmfifo_error);
      return wr_cnt ? wr_cnt : bd->tmfifo_error;
    }

    if (write_full(bd, chan)) {
      RSHIM_DBG("fifo_write: fifo full\n");
      if (nonblock) {
        pthread_mutex_unlock(&bd->mutex);
        RSHIM_DBG("fifo_write: returning %zd/EAGAIN\n", wr_cnt);
        return wr_cnt ? wr_cnt : -EAGAIN;
      }

      RSHIM_DBG("fifo_write: waiting for writable chan %d\n", chan);
      while (write_full(bd, chan)) {
        if (pthread_cond_wait(&bd->write_fifo[chan].operable, &bd->mutex)) {
          RSHIM_DBG("fifo_write: returning %zd/ERESTARTSYS\n", wr_cnt);
          pthread_mutex_unlock(&bd->mutex);
          return wr_cnt ? wr_cnt : -EAGAIN;
        }
      }

      /*
       * Since we dropped the mutex, we must make sure our interface is still
       * there before we do anything else.
       */
      continue;
    }

    pthread_mutex_lock(&bd->ringlock);
    writesize = MIN(count, (size_t)write_space(bd, chan));
    pass1 = MIN(writesize, (size_t)write_space_to_end(bd, chan));
    pass2 = writesize - pass1;
    pthread_mutex_unlock(&bd->ringlock);

    RSHIM_DBG("fifo_write: writesize %zd, head %d, tail %d\n",
              writesize, bd->write_fifo[chan].head,
              bd->write_fifo[chan].tail);

    memcpy(write_space_ptr(bd, chan), buffer, pass1);
    if (pass2)
      memcpy(bd->write_fifo[chan].data, buffer + pass1, pass2);

    pthread_mutex_lock(&bd->ringlock);

    write_add_bytes(bd, chan, writesize);

    /* We have some new bytes, let's see if we can write any. */
    rshim_fifo_output(bd);

    pthread_mutex_unlock(&bd->ringlock);

    count -= writesize;
    buffer += writesize;
    wr_cnt += writesize;
    RSHIM_DBG("fifo_write: transferred %zd bytes this pass\n", writesize);
  }

  pthread_mutex_unlock(&bd->mutex);

  RSHIM_DBG("fifo_write: returning %zd\n", wr_cnt);
  return wr_cnt;
}

static void rshim_work_handler(struct rshim_backend *bd)
{
  pthread_mutex_lock(&bd->mutex);

  bd->work_pending = false;

  if (bd->keepalive && bd->has_rshim) {
      bd->write_rshim(bd, RSHIM_CHANNEL, RSH_SCRATCHPAD1,
                      RSH_KEEPALIVE_MAGIC_NUM);
      bd->keepalive = 0;
  }

  if (bd->boot_work_buf != NULL) {
    bd->boot_work_buf_actual_len = rshim_write_delayed(bd,
                                                       RSH_DEV_TYPE_BOOT,
                                                       bd->boot_work_buf,
                                                       bd->boot_work_buf_len);
    bd->boot_work_buf = NULL;
    pthread_cond_broadcast(&bd->boot_write_complete_cond);
  }

  if (bd->is_boot_open) {
    pthread_mutex_unlock(&bd->mutex);
    return;
  }

  if (bd->has_fifo_work) {
    int len;

    len = rshim_write_delayed(bd, bd->fifo_work_devtype,
                              bd->fifo_work_buf,
                              bd->fifo_work_buf_len);
    bd->has_fifo_work = 0;

    pthread_mutex_lock(&bd->ringlock);

    bd->spin_flags &= ~RSH_SFLG_WRITING;
    if (len == bd->fifo_work_buf_len) {
      pthread_cond_broadcast(&bd->fifo_write_complete_cond);
      rshim_notify(bd, RSH_EVENT_FIFO_OUTPUT, 0);
    } else {
      rshim_notify(bd, RSH_EVENT_FIFO_ERR, -1);
      RSHIM_ERR("fifo_write: completed abnormally (%d).", len);
    }

    pthread_mutex_unlock(&bd->ringlock);
  }

  if (bd->has_cons_work) {
    pthread_mutex_lock(&bd->ringlock);

    /* FIFO output. */
    rshim_fifo_output(bd);

    /* FIFO input. */
    rshim_fifo_input(bd);

    pthread_mutex_unlock(&bd->ringlock);

    bd->has_cons_work = 0;
  }

  if (!bd->has_reprobe && bd->is_cons_open) {
    bd->has_cons_work = 1;
    if (bd->timer - rshim_timer_ticks > 100)
      bd->timer = rshim_timer_ticks + 100;
  }

  pthread_mutex_unlock(&bd->mutex);
}

static int rshim_boot_done(struct rshim_backend *bd)
{
  if (bd->has_rshim && bd->has_tm) {
    /* Clear any previous errors. */
    bd->tmfifo_error = 0;

    /*
     * If someone might be waiting for the device to come up,
     * tell them it's ready.
     */
    if (bd->is_booting) {
        bd->is_booting = 0;

        RSHIM_DBG("signaling booting complete\n");
        pthread_cond_broadcast(&bd->boot_complete_cond);
    };

    /* If the console device is open, start the worker. */
    if (bd->is_cons_open && !bd->has_cons_work) {
      bd->has_cons_work = 1;
      RSHIM_DBG("probe: console_work submitted\n");
      rshim_work_signal(bd);
    }

    /* Tell the user this device is now attached. */
    RSHIM_INFO("rshim%d now attached\n", bd->dev_index);
  }

  return 0;
}

static int rshim_fifo_fsync(struct rshim_backend *bd, int chan)
{
  pthread_mutex_lock(&bd->mutex);

  /*
   * To ensure that all of our data has actually made it to the
   * device, we first wait until the channel is empty, then we wait
   * until there is no outstanding write urb.
   */
  while (!write_empty(bd, chan))
    if (pthread_cond_wait(&bd->write_fifo[chan].operable, &bd->mutex)) {
      pthread_mutex_unlock(&bd->mutex);
      return -EINTR;
    }

  while (bd->spin_flags & RSH_SFLG_WRITING) {
    if (pthread_cond_wait(&bd->fifo_write_complete_cond, &bd->mutex)) {
      pthread_mutex_unlock(&bd->mutex);
      return -EINTR;
    }
  }

  pthread_mutex_unlock(&bd->mutex);

  return 0;
}

static void rshim_fifo_poll(fuse_req_t req, struct fuse_file_info *fi,
                            struct fuse_pollhandle *ph, int chan)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  unsigned revents = 0;

  pthread_mutex_lock(&bd->mutex);

  pthread_mutex_lock(&bd->ringlock);

  if (!read_empty(bd, chan))
    revents |= POLLIN | POLLRDNORM;

  if (!write_full(bd, chan))
    revents |= POLLOUT | POLLWRNORM;

  /*
   * We don't report POLLERR on the console so that it doesn't get
   * automatically disconnected when it fails, and so that you can
   * connect to it in the error state before rebooting the target.
   * This is inconsistent, but being consistent turns out to be very
   * annoying.  If someone tries to actually type on it, they'll
   * get an error.
   */
  if (bd->tmfifo_error && chan != TMFIFO_CONS_CHAN)
    revents |= POLLERR;

  pthread_mutex_unlock(&bd->ringlock);

  pthread_mutex_unlock(&bd->mutex);

  if (ph) {
    if (!bd->rx_poll_handle[TMFIFO_CONS_CHAN])
      bd->rx_poll_handle[TMFIFO_CONS_CHAN] = ph;
    else if (ph != bd->rx_poll_handle[TMFIFO_CONS_CHAN])
      fuse_pollhandle_destroy(ph);
  }
  fuse_reply_poll(req, revents);
}

static void rshim_fifo_release(fuse_req_t req, struct fuse_file_info *fi,
                               int chan)
{
  struct rshim_backend *bd = fuse_req_userdata(req);

  pthread_mutex_lock(&bd->mutex);

  if (chan == TMFIFO_CONS_CHAN) {
    /*
     * If we aren't the last console file, nothing to do but
     * fix the reference count.
     */
    bd->console_opens--;
    if (bd->console_opens) {
        pthread_mutex_unlock(&bd->mutex);
        fuse_reply_err(req, 0);
        return;
    }

    /*
     * We've told the host to stop using the TM FIFO console,
     * but there may be a lag before it does.  Unless we
     * continue to read data from the console stream, the host
     * may spin forever waiting for the console to be drained
     * and not realize that it's time to stop using it.
     * Clearing the CONS_OPEN spin flag will discard any future
     * incoming console data, but if our input buffers are full
     * now, we might not be even reading from the hardware
     * FIFO.  To avoid problems, clear the buffers and call the
     * drainer so that it knows there's space.
     */
    pthread_mutex_lock(&bd->ringlock);

    bd->spin_flags &= ~RSH_SFLG_CONS_OPEN;
    read_reset(bd, TMFIFO_CONS_CHAN);
    write_reset(bd, TMFIFO_CONS_CHAN);

    if (bd->has_tm)
      rshim_fifo_input(bd);

    pthread_mutex_unlock(&bd->ringlock);
  }

  if (chan == TMFIFO_CONS_CHAN)
    bd->is_cons_open = 0;
  else
    bd->is_tm_open = 0;

  if (!bd->is_tm_open && !bd->is_cons_open) {
    if (bd->cancel)
      bd->cancel(bd, RSH_DEV_TYPE_TMFIFO, false);

    pthread_mutex_lock(&bd->ringlock);
    bd->spin_flags &= ~RSH_SFLG_READING;
    pthread_mutex_unlock(&bd->ringlock);
  }

  pthread_mutex_unlock(&bd->mutex);

  fuse_reply_err(req, 0);
}

#if TBD
/* TMFIFO file operations routines */

static void rshim_tmfifo_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                              off_t off, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = (struct rshim_backend *)fi->fh;
  char buf[1024];
  int len;

  return rshim_fifo_read(bd, buf, sizeof(buf), TMFIFO_NET_CHAN,
                         fi->f_flags & O_NONBLOCK);
}

static void rshim_tmfifo_write(fuse_req_t req, const char *buf, size_t size,
                               off_t off, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = (struct rshim_backend *)fi->fh;
  int rc;

  rc = rshim_fifo_write(bd, buf, count, TMFIFO_NET_CHAN,
                        file->f_flags & O_NONBLOCK);
}

static void rshim_tmfifo_fsync(fuse_req_t req, int datasync,
                               struct fuse_file_info *fi)
{
  rshim_fifo_fsync(req, datasync, fi, TMFIFO_NET_CHAN);
}

static void rshim_tmfifo_poll(fuse_req_t req, struct fuse_file_info *fi,
                              struct fuse_pollhandle *ph, int chan)
{
  rshim_fifo_poll(req, fi, ph, , TMFIFO_NET_CHAN);
}

static void rshim_tmfifo_release(fuse_req_t req, struct fuse_file_info *fi)
{
  rshim_fifo_release(req, fi, TMFIFO_NET_CHAN);
}

static const struct file_operations rshim_tmfifo_fops = {
    .owner = THIS_MODULE,
    .read = rshim_tmfifo_read,
    .write = rshim_tmfifo_write,
    .fsync = rshim_tmfifo_fsync,
    .poll = rshim_tmfifo_poll,
    .release = rshim_tmfifo_release,
};

static int rshim_tmfifo_open(struct file *file)
{
  struct rshim_backend *bd = file->private_data;

  file->f_op = &rshim_tmfifo_fops;

  pthread_mutex_lock(&bd->mutex);

  if (bd->is_tm_open) {
    RSHIM_DBG("tmfifo_open: file already open\n");
    pthread_mutex_unlock(&bd->mutex);
    return -EBUSY;
  }

  bd->is_tm_open = 1;

  /* Call the drainer to do an initial read, if needed. */
  pthread_mutex_lock(&bd->ringlock);
  rshim_fifo_input(bd);
  pthread_mutex_unlock(&bd->ringlock);

  pthread_mutex_unlock(&bd->mutex);

  return 0;
}
#endif

/* Console operations */

static void rshim_console_open(fuse_req_t req, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);

  pthread_mutex_lock(&bd->mutex);

  if (bd->is_cons_open) {
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_err(req, -EBUSY);
    return;
  }

  bd->is_cons_open = 1;

  pthread_mutex_lock(&bd->ringlock);

  bd->spin_flags |= RSH_SFLG_CONS_OPEN;

  pthread_mutex_unlock(&bd->ringlock);

  if (!bd->has_cons_work) {
    bd->has_cons_work = 1;
    rshim_work_signal(bd);
  }

  bd->console_opens++;
  pthread_mutex_unlock(&bd->mutex);

  fuse_reply_open(req, fi);
}

static void rshim_console_read(fuse_req_t req, size_t size, off_t off,
                               struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  char buf[512];
  int rc;

  if (off) {
    fuse_reply_err(req, -EINVAL);
    return;
  }

  if (size > sizeof(buf))
    size = sizeof(buf);

  rc = rshim_fifo_read(bd, buf, size, TMFIFO_CONS_CHAN,
                       fi->flags & O_NONBLOCK);
  if (rc < 0)
    fuse_reply_err(req, rc);
  else
    fuse_reply_buf(req, buf, rc);
}

static void rshim_console_write(fuse_req_t req, const char *buf, size_t size,
                                off_t off, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  int rc;

  if (off) {
    fuse_reply_err(req, -EINVAL);
    return;
  }

  rc = rshim_fifo_write(bd, buf, size, TMFIFO_CONS_CHAN,
                        fi->flags & O_NONBLOCK);
  if (rc >= 0)
    fuse_reply_write(req, rc);
  else
    fuse_reply_err(req, rc);
}

static void rshim_console_fsync(fuse_req_t req, int datasync,
                                struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  int rc;

  rc = rshim_fifo_fsync(bd, TMFIFO_CONS_CHAN);

  fuse_reply_err(req, rc);
}

static void rshim_console_ioctl(fuse_req_t req, int cmd, void *arg,
                                struct fuse_file_info *fi,
                                unsigned flags, const void *in_buf,
                                size_t in_bufsz, size_t out_bufsz)
{
  struct rshim_backend *bd = fuse_req_userdata(req);

  pthread_mutex_lock(&bd->mutex);

  switch (cmd) {
  case TCGETS:
    if (!out_bufsz) {
      struct iovec iov = { arg, sizeof(struct termio) };

      fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
    } else {
      fuse_reply_ioctl(req, 0, &bd->cons_termios, sizeof(struct termio));
    }
    break;

  case TCSETS:
  case TCSETSW:
  case TCSETSF:
    if (!in_bufsz) {
      struct iovec iov = {arg, sizeof(bd->cons_termios)};

      fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
    } else {
      memcpy(&bd->cons_termios, in_buf, sizeof(bd->cons_termios));
      fuse_reply_ioctl(req, 0, NULL, 0);
    }
    break;

  default:
    fuse_reply_err(req, ENOSYS);
    break;
  }

  pthread_mutex_unlock(&bd->mutex);
}

static void rshim_console_poll(fuse_req_t req, struct fuse_file_info *fi,
                               struct fuse_pollhandle *ph)
{
  rshim_fifo_poll(req, fi, ph, TMFIFO_CONS_CHAN);
}

static void rshim_console_release(fuse_req_t req, struct fuse_file_info *fi)
{
  rshim_fifo_release(req, fi, TMFIFO_CONS_CHAN);
}

static const struct cuse_lowlevel_ops rshim_console_fops = {
  .open = rshim_console_open,
  .read = rshim_console_read,
  .write = rshim_console_write,
  .fsync = rshim_console_fsync,
  .ioctl = rshim_console_ioctl,
  .poll = rshim_console_poll,
  .release = rshim_console_release,
};

/* Rshim file operations routines */

static void rshim_rshim_open(fuse_req_t req, struct fuse_file_info *fi)
{
    fuse_reply_open(req, fi);
}

static void rshim_rshim_read(fuse_req_t req, size_t size, off_t off,
                             struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  uint64_t data;
  int rc = 0;

  /* rshim registers are all 8-byte aligned. */
  if (size != 8 || (off & 7) != 0) {
    fuse_reply_err(req, -EINVAL);
    return;
  }

  pthread_mutex_lock(&bd->mutex);
  rc = bd->read_rshim(bd,
                      (off >> 16) & 0xF, /* channel # */
                      off & 0xFFFF,  /* addr */
                      &data);
  pthread_mutex_unlock(&bd->mutex);
  if (!rc)
    fuse_reply_buf(req, (void *)&data, size);
  else
    fuse_reply_err(req, rc);
}

static void rshim_rshim_write(fuse_req_t req, const char *buf, size_t size,
                              off_t off, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  uint64_t data;
  int rc;

  /* rshim registers are all 8-byte aligned. */
  if (size != 8 || (off & 7) != 0) {
    fuse_reply_err(req, -EINVAL);
    return;
  }

  /* Copy the data from userspace */
  memcpy(&data, buf, size);

  pthread_mutex_lock(&bd->mutex);
  rc = bd->write_rshim(bd,
                       (off >> 16) & 0xF, /* channel # */
                       off & 0xFFFF, /* addr */
                       data);
  pthread_mutex_unlock(&bd->mutex);

  if (!rc)
    fuse_reply_write(req, size);
  else
    fuse_reply_err(req, rc);
}

static void rshim_rshim_release(fuse_req_t req, struct fuse_file_info *fi)
{
  fuse_reply_err(req, 0);
}

static const struct cuse_lowlevel_ops rshim_rshim_fops = {
    .read = rshim_rshim_read,
    .write = rshim_rshim_write,
    .release = rshim_rshim_release,
};

/* Misc file operations routines */

static void rshim_misc_open(fuse_req_t req, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);

  if (bd) {
    int *id = calloc(1, sizeof(*id));
    fi->fh = (uint64_t)id;
    fuse_reply_open(req, fi);
  }
  else
    fuse_reply_err(req, ENODEV);
}

static void rshim_misc_read(fuse_req_t req, size_t size, off_t off,
                            struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  uint8_t *mac = bd->peer_mac;
  int rc, len = 4096, seg;
  struct timespec ts;
  struct timeval tp;
  uint64_t value;
  char *buf, *p;

  if ((*(int*)fi->fh)++) {
    fuse_reply_buf(req, NULL, 0);
    return;
  }

  /* Boot mode. */
  pthread_mutex_lock(&bd->mutex);
  rc = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_BOOT_CONTROL, &value);
  pthread_mutex_unlock(&bd->mutex);
  if (rc) {
    RSHIM_ERR("couldn't read rshim register\n");
    fuse_reply_err(req, rc);
    return;
  }

  buf = malloc(len);
  p = buf;
  
  seg = snprintf(p, len, "BOOT_MODE %lld\n", (long long)(value & RSH_BOOT_CONTROL__BOOT_MODE_MASK));
  p += seg;
  len -= seg;

  /* SW reset flag is always 0. */
  seg = snprintf(p, len, "SW_RESET  %d\n", 0);
  p += seg;
  len -= seg;

  /* Display the driver name. */
  seg = snprintf(p, len, "DRV_NAME  %s\n", bd->drv_name);
  p += seg;
  len -= seg;

  if (rshim_adv_cfg) {
    gettimeofday(&tp, NULL);

    /*
     * Display the target-side information. Send a request and wait for
     * some time for the response.
     */
    pthread_mutex_lock(&bd->mutex);
    bd->peer_ctrl_req = 1;
    bd->peer_ctrl_resp = 0;
    memset(mac, 0, 6);
    bd->has_cons_work = 1;
    rshim_work_signal(bd);

    ts.tv_sec  = tp.tv_sec + 1;
    ts.tv_nsec = tp.tv_usec * 1000;
    pthread_cond_timedwait(&bd->ctrl_wait_cond, &bd->mutex, &ts);
    pthread_mutex_unlock(&bd->mutex);
    seg = snprintf(p, len, "PEER_MAC  %02x:%02x:%02x:%02x:%02x:%02x\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    p += seg;
    len -= seg;
    seg = snprintf(p, len, "PXE_ID    0x%08x\n", htonl(bd->pxe_client_id));
    p += seg;
    len -= seg;
  }

  len = p - buf;
  if (off > p - buf)
    off = p - buf;
  if (off + size > p - buf)
    size = p - buf - off;

  fuse_reply_buf(req, buf + off, size);
  free(buf);
}

static void rshim_misc_write(fuse_req_t req, const char *buf, size_t size,
                             off_t off, struct fuse_file_info *fi)
{
  struct rshim_backend *bd = fuse_req_userdata(req);
  int i, rc = 0, value = 0, mac[6];
  const char *p = buf;
  char key[32];

  if (off) {
    fuse_reply_err(req, -EINVAL);
    return;
  }

  if (sscanf(buf, "%s", key) != 1) {
    fuse_reply_err(req, -EINVAL);
    return;
  }
  p += strlen(key);

  if (strcmp(key, "BOOT_MODE") == 0) {
    if (sscanf(p, "%x", &value) != 1) {
      fuse_reply_err(req, -EINVAL);
      return;
    }

    pthread_mutex_lock(&bd->mutex);
    rc = bd->write_rshim(bd, RSHIM_CHANNEL, RSH_BOOT_CONTROL,
                         value & RSH_BOOT_CONTROL__BOOT_MODE_MASK);
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_write(req, size);
  } else if (strcmp(key, "SW_RESET") == 0) {
    if (sscanf(p, "%x", &value) != 1) {
      fuse_reply_err(req, -EINVAL);
      return;
    }

    if (value) {
      if (!bd->has_reprobe) {
        /* Detach, which shouldn't hold bd->mutex. */
        rshim_notify(bd, RSH_EVENT_DETACH, 0);

        pthread_mutex_lock(&bd->mutex);
        /* Reset the TmFifo. */
        rshim_fifo_reset(bd);
        bd->is_booting = 1;
        pthread_mutex_unlock(&bd->mutex);
      }

      pthread_mutex_lock(&bd->mutex);
      rc = bd->write_rshim(bd, RSHIM_CHANNEL,
                           RSH_RESET_CONTROL,
                           RSH_RESET_CONTROL__RESET_CHIP_VAL_KEY);
      pthread_mutex_unlock(&bd->mutex);

      if (!bd->has_reprobe) {
        /* Attach. */
        sleep(bd->has_reprobe? 1 : 10);
        pthread_mutex_lock(&bd->mutex);
        bd->is_booting = 0;
        rshim_notify(bd, RSH_EVENT_ATTACH, 0);
        pthread_mutex_unlock(&bd->mutex);
      }
    }

    fuse_reply_write(req, size);
  } else if (strcmp(key, "PEER_MAC") == 0) {
    if (sscanf(p, "%x:%x:%x:%x:%x:%x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
      fuse_reply_err(req, -EINVAL);
      return;
    }
    pthread_mutex_lock(&bd->mutex);
    for (i = 0; i < 6; i++)
      bd->peer_mac[i] = mac[i];
    bd->peer_mac_set = 1;
    bd->has_cons_work = 1;
    rshim_work_signal(bd);
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_write(req, size);
  } else if (strcmp(key, "PXE_ID") == 0) {
    if (sscanf(p, "%x", &value) != 1) {
      fuse_reply_err(req, -EINVAL);
      return;
    }
    pthread_mutex_lock(&bd->mutex);
    bd->pxe_client_id = ntohl(value);
    bd->peer_pxe_id_set = 1;
    bd->has_cons_work = 1;
    rshim_work_signal(bd);
    pthread_mutex_unlock(&bd->mutex);
    fuse_reply_write(req, size);
  } else {
    fuse_reply_err(req, -EINVAL);
  }
}

static void rshim_misc_release(fuse_req_t req, struct fuse_file_info *fi)
{
  free((void*)fi->fh);
  fuse_reply_err(req, 0);
}

static const struct cuse_lowlevel_ops rshim_misc_fops = {
    .open = rshim_misc_open,
    .read = rshim_misc_read,
    .write = rshim_misc_write,
    .release = rshim_misc_release,
};

static void *cuse_worker(void *arg)
{
  struct fuse_session *se = arg;
  int rc;

  rc = fuse_session_loop_mt(se);
  cuse_lowlevel_teardown(se);

  return (void *)(unsigned long)rc;
}

static int rshim_fs_init(struct rshim_backend *bd)
{
  char buf[128], *name;
  pthread_t thread;
  const char *bufp = buf;
  struct cuse_info ci = { .dev_info_argc = 1,
                          .dev_info_argv = &bufp,
                          .flags = CUSE_UNRESTRICTED_IOCTL };
  const struct cuse_lowlevel_ops *ops[RSH_DEV_TYPES] = {
                          [RSH_DEV_TYPE_BOOT] = &rshim_boot_fops,
                          [RSH_DEV_TYPE_TMFIFO] = &rshim_console_fops,
                          [RSH_DEV_TYPE_RSHIM] = &rshim_rshim_fops,
                          [RSH_DEV_TYPE_MISC] = &rshim_misc_fops,
                          };
  char *argv[] = {"./bfrshim", "-f"};
  int i, rc;

  for (i = 0; i < RSH_DEV_TYPES; i++) {
    name = rshim_dev_minor_names[i];
    snprintf(buf, sizeof(buf), "DEVNAME=rshim%d/%s", bd->dev_index, name);
    if (!ops[i])
      continue;
    bd->fuse_session[i] = cuse_lowlevel_setup(sizeof(argv)/sizeof(char *), argv,
                                      &ci, ops[i], NULL, bd);
    if (!bd->fuse_session[i]) {
      RSHIM_ERR("Failed to setup CUSE %s", name);
      return -1;
    }
    rc = pthread_create(&bd->thread[i], NULL, cuse_worker, bd->fuse_session[i]);
    if (rc) {
      RSHIM_ERR("Failed to create cuse thread %m");
      return rc;
    }
  }

  return 0;
}

static int rshim_fs_del(struct rshim_backend *bd)
{
  int i;

  for (i = 0; i < RSH_DEV_TYPES; i++) {
    if (bd->fuse_session[i]) {
      fuse_session_exit(bd->fuse_session[i]);
      pthread_kill(bd->thread[i], SIGINT);
      pthread_join(bd->thread[i], NULL);
    }
  }
  return 0;
}

int rshim_notify(struct rshim_backend *bd, int event, int code)
{
    int i, rc = 0;

    switch (event) {
    case RSH_EVENT_FIFO_INPUT:
        rshim_fifo_input(bd);
        break;

    case RSH_EVENT_FIFO_OUTPUT:
        rshim_fifo_output(bd);
        break;

    case RSH_EVENT_FIFO_ERR:
        rshim_fifo_err(bd, code);
        break;

    case RSH_EVENT_ATTACH:
        rshim_boot_done(bd);

        /* Sync-up the tmfifo if reprobe is not supported. */
        if (!bd->has_reprobe && bd->has_rshim)
            rshim_fifo_sync(bd);

        /* Init network interface. */
        rc = rshim_net_init(bd);
        if (rc < 0) {
          RSHIM_ERR("Failed to init networking\n");
          break;
        }

        pthread_mutex_lock(&bd->ringlock);
        rshim_fifo_input(bd);
        pthread_mutex_unlock(&bd->ringlock);
        break;

    case RSH_EVENT_DETACH:
        /* Shutdown network interface. */
        rshim_net_del(bd);
        // bd->dev = NULL;
        break;
    }

    return rc;
}

static int rshim_find_index(char *dev_name)
{
    int i, index = -1;

    /* First look for a match with a previous device name. */
    for (i = 0; i < RSHIM_MAX_DEV; i++)
        if (rshim_dev_names[i] &&
            !strcmp(dev_name, rshim_dev_names[i])) {
            RSHIM_DBG("found match with previous at index %d\n", i);
            index = i;
            break;
        }

    /* Then look for a never-used slot. */
    if (index < 0) {
        for (i = 0; i < RSHIM_MAX_DEV; i++)
            if (!rshim_dev_names[i]) {
                index = i;
                break;
            }
    }

    /* Finally look for a currently-unused slot. */
    if (index < 0) {
        for (i = 0; i < RSHIM_MAX_DEV; i++) {
            if (!rshim_devs[i]) {
                RSHIM_DBG("found unused slot %d\n", i);
                index = i;
                break;
            }
        }
    }

    return index;
}

struct rshim_backend *rshim_find_by_name(char *dev_name)
{
    int index = rshim_find_index(dev_name);

    /* If none of that worked, we fail. */
    if (index < 0) {
        RSHIM_ERR("couldn't find slot for new device %s\n", dev_name);
        return NULL;
    }

    return rshim_devs[index];
}

struct rshim_backend *rshim_find_by_dev(void *dev)
{
  struct rshim_backend *bd;
  int index;

  for (index = 0; index < RSHIM_MAX_DEV; index++) {
    bd = rshim_devs[index];
    if (bd && bd->dev == dev)
      return bd;
  }

  return NULL;
}

/* House-keeping timer. */
static void rshim_timer_func(struct rshim_backend *bd)
{
    int period = rshim_keepalive_period;

    if (bd->has_cons_work)
        rshim_work_signal(bd);

    /* Request keepalive update and restart the ~300ms timer. */
    if (rshim_timer_ticks - (bd->last_keepalive + period) > 0) {
        bd->keepalive = 1;
        bd->last_keepalive = rshim_timer_ticks;
        rshim_work_signal(bd);
    }

    bd->timer = rshim_timer_ticks + period;
}

static void rshim_timer_run(void)
{
  struct rshim_backend *bd;
  int i;

  rshim_timer_ticks++;

  for (i = 0; i < RSHIM_MAX_DEV; i++) {
    bd = rshim_devs[i];
    if (bd) {
      if (rshim_timer_ticks - bd->timer > 0)
        rshim_timer_func(bd);

      /* Push out remaining data if not sent out in the epoll loop. */
      if (bd->net_fd >= 0)
        rshim_net_tx(bd);
        rshim_net_rx(bd);
    }
  }
}

/* Check whether backend is allowed to register or not. */
static int rshim_access_check(struct rshim_backend *bd)
{
    uint64_t value;
    int i, rc;

    /* Write value 0 to RSH_SCRATCHPAD1. */
    rc = bd->write_rshim(bd, RSHIM_CHANNEL, RSH_SCRATCHPAD1, 0);
    if (rc < 0)
        return -ENODEV;

    /*
     * Poll RSH_SCRATCHPAD1 up to one second to check whether it's reset to
     * the keepalive magic value, which indicates another backend driver has
     * already attached to this target.
     */
    for (i = 0; i < 10; i++) {
        rc = bd->read_rshim(bd, RSHIM_CHANNEL, RSH_SCRATCHPAD1, &value);
        if (rc < 0)
            return -ENODEV;

        if (value == RSH_KEEPALIVE_MAGIC_NUM) {
            RSHIM_INFO("another backend already attached.\n");
            return -EEXIST;
        }

        usleep(100000);
    }

    return 0;
}

int rshim_register(struct rshim_backend *bd)
{
    int i, rc, dev_index;

    if (bd->registered)
        return 0;

    dev_index = rshim_find_index(bd->dev_name);
    if (dev_index < 0)
        return -ENODEV;

    if (!bd->read_rshim || !bd->write_rshim) {
        RSHIM_ERR("read_rshim/write_rshim missing\n");
        return -EINVAL;
    }

    rc = rshim_access_check(bd);
    if (rc)
        return rc;

    if (!bd->write)
        bd->write = rshim_write_default;
    if (!bd->read)
        bd->read = rshim_read_default;

    pthread_mutex_init(&bd->ringlock, NULL);

    for (i = 0; i < TMFIFO_MAX_CHAN; i++) {
        pthread_cond_init(&bd->read_fifo[i].operable, NULL);
        pthread_cond_init(&bd->write_fifo[i].operable, NULL);
    }

    pthread_cond_init(&bd->fifo_write_complete_cond, NULL);
    pthread_cond_init(&bd->boot_complete_cond, NULL);
    pthread_cond_init(&bd->boot_write_complete_cond, NULL);
    pthread_cond_init(&bd->ctrl_wait_cond, NULL);
    memcpy(&bd->cons_termios, &init_console_termios,
           sizeof(init_console_termios));

    bd->dev_index = dev_index;
    if (rshim_dev_names[dev_index] != bd->dev_name) {
        free(rshim_dev_names[dev_index]);
        rshim_dev_names[dev_index] = bd->dev_name;
    }
    rshim_devs[dev_index] = bd;

    for (i = 0; i < 2; i++) {
        bd->boot_buf[i] = malloc(BOOT_BUF_SIZE);
        if (!bd->boot_buf[i]) {
            if (i == 1) {
                free(bd->boot_buf[0]);
                bd->boot_buf[0] = NULL;
            }
        }
    }

    bd->net_fd = -1;
    bd->registered = 1;

    /* Start the keepalive timer. */
    bd->last_keepalive = rshim_timer_ticks;
    bd->timer = rshim_timer_ticks + 1;

    // create chracter devices.
    rc = rshim_fs_init(bd);
    if (rc)
        return rc;

    return 0;
}

void rshim_deregister(struct rshim_backend *bd)
{
    int i;

    if (!bd->registered)
        return;

    rshim_fs_del(bd);

    for (i = 0; i < 2; i++)
        free(bd->boot_buf[i]);

    rshim_devs[bd->dev_index] = NULL;
    bd->registered = 0;
}

static void rshim_main(int argc, char *argv[])
{
  int i, fd, num, rc, epoll_fd, timer_fd;
  const int MAXEVENTS = 64;
  struct epoll_event events[MAXEVENTS];
  struct epoll_event event;
  struct rshim_backend *bd;
  void *usb_ctx = NULL;
  struct itimerspec ts;
  uint32_t len;
  bool found;

  memset(&event, 0, sizeof(event));
  memset(events, 0, sizeof(events));

  system("modprobe cuse");

  /* Create the epoll fd */
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd == -1) {
    RSHIM_ERR("epoll_create1 failed: %m\n");
    exit(-1);
  }
  rshim_epoll_fd = epoll_fd;

  /* Create and add work fd. */
  if (pipe(rshim_work_fd) == -1) {
    perror("Failed to create pipe %m");
    exit(-1);
  }
  event.data.fd = rshim_work_fd[0];
  event.events = EPOLLIN;
  rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, rshim_work_fd[0], &event);
  if (rc == -1) {
    RSHIM_ERR("epoll_ctl failed: %m\n");
    exit(-1);
  }

  /* Add timer fd. */
  timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (timer_fd == -1)
  {
    fprintf(stderr, "timerfd_create failed: %m\n");
    exit(1);
  }
  ts.it_interval.tv_sec = 0;
  ts.it_interval.tv_nsec = RSHIM_TIMER_INTERVAL * 1000000;
  ts.it_value.tv_sec = 0;
  ts.it_value.tv_nsec = ts.it_interval.tv_nsec;
  timerfd_settime(timer_fd, 0, &ts, NULL);
  event.data.fd = timer_fd;
  event.events = EPOLLIN | EPOLLOUT;
  rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &event);
  if (rc == -1)
  {
    fprintf(stderr, "epoll_ctl failed: %m\n");
    exit(1);
  }

  /* Scan rshim backends. */
  rc = 0;
  if (!rshim_backend_name) {
    usb_ctx = rshim_usb_init(epoll_fd);
    if (!usb_ctx) {
      rc = rshim_pcie_init();
      if (rc)
        rc = rshim_pcie_lf_init();
    }
  } else {
    if (!strcmp(rshim_backend_name, "usb"))
      usb_ctx = rshim_usb_init(epoll_fd);
    else if (!strcmp(rshim_backend_name, "pcie"))
      rc = rshim_pcie_init();
    else if (!strcmp(rshim_backend_name, "pcie_lf"))
      rc = rshim_pcie_lf_init();
  }
  if (!usb_ctx && rc) {
    if (rc) {
      RSHIM_ERR("No rshim devices found\n");
      exit(-1);
    }
  }

  for (;;) {
    num = epoll_wait(epoll_fd, events, MAXEVENTS, -1);
    if (num <= 0) {
      if (num < 0)
        RSHIM_DBG("epoll_wait failed; %m\n");
      continue;
    }

    for (i = 0; i < num; i++) {
      found = false;
      fd = events[i].data.fd;

      if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
        RSHIM_DBG("epoll error\n");
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        continue;
      }

      if (fd == timer_fd) {
        uint64_t res;

        found = true;
        rshim_fd_full_read(timer_fd, &res, sizeof(res));
        rshim_timer_run();
      } else if (fd == rshim_work_fd[0]) {
        found = true;
        rc = rshim_fd_full_read(rshim_work_fd[0], &bd, sizeof(bd));
        if (rc == sizeof(bd))
          rshim_work_handler(bd);
      } else {
        int index, tmp;

        /* Network. */
        for (index = 0; index < RSHIM_MAX_DEV; index++) {
          bd = rshim_devs[index];
          if (!bd)
            continue;

          if (fd == bd->net_notify_fd[0]) {
            /* Rx. */
            found = true;
            if (read(fd, &tmp, 1) == 1)
              rshim_net_rx(bd);
            break;
          } else if (fd == rshim_devs[index]->net_fd) {
            /* Tx. */
            found = true;
            rshim_net_tx(bd);
            break;
          }
        }
      }

      if (!found && usb_ctx)
        rshim_usb_poll(usb_ctx);
    }
  }
}

static void print_help(void)
{
  printf("./rshim [options]\n");
  printf("  -a                     advanced options\n");
  printf("  -b <usb|pcie|pcie_lf>  driver name\n");
  printf("  -k                     skip sw_reset\n");
  printf("  -l <0~4>               debug level\n");
}

int main(int argc, char *argv[])
{
  pthread_t thread;
  int c;

  /* Parse arguments. */
  while ((c = getopt (argc, argv, "ab:kl:h")) != -1) {
    switch (c) {
    case 'a':
      rshim_adv_cfg = 1;
      break;
    case 'b':
      rshim_backend_name = optarg;
      break;
    case 'k':
      rshim_sw_reset_skip = 1;
      break;
    case 'l':
      rshim_log_level = atoi(optarg);
      break;
    case 'h':
    default:
      print_help();
      return -1;
    }
  }

  /* Put into daemon mode if no debugging. */
  if (!rshim_log_level)
  {
    int pid = fork();

    if (pid < 0) {
      perror("fork failed: %m\n");
      return -1;
    } else if (pid > 0) {
      return 0;
    }

    umask(0);
    if (setsid() < 0) {
      perror("setsid failed: %m\n");
      return -1;
    }
    signal(SIGCHLD, SIG_IGN);
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
  }

  rshim_main(argc, argv);

  return 0;
}
