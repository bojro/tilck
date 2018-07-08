
#include <exos/common/basic_defs.h>
#include <exos/common/string_util.h>
#include <exos/common/debug/termios_debug.c.h>

#include <exos/kernel/fs/exvfs.h>
#include <exos/kernel/errno.h>
#include <exos/kernel/user.h>
#include <exos/kernel/term.h>

#include <termios.h>      // system header
#include <sys/ioctl.h>    // system header

struct termios c_term;

const struct termios default_termios =
{
   .c_iflag = ICRNL | IXON,
   .c_oflag = OPOST | ONLCR,
   .c_cflag = CREAD | B38400 | CS8,
   .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
   .c_line = 0,

   .c_cc =
   {
      [VINTR]     = 0x03,        /* typical value for TERM=linux, Ctrl+C */
      [VQUIT]     = 0x1c,        /* typical value for TERM=linux, Ctrl+\ */
      [VERASE]    = 0x7f,        /* typical value for TERM=linux */
      [VKILL]     = TERM_KILL_C,
      [VEOF]      = 0x04,        /* typical value for TERM=linux, Ctrl+D */
      [VTIME]     = 0,           /* typical value for TERM=linux (unset) */
      [VMIN]      = 0x01,        /* typical value for TERM=linux */
      [VSWTC]     = 0,           /* typical value for TERM=linux (unsupported)*/
      [VSTART]    = 0x11,        /* typical value for TERM=linux, Ctrl+Q */
      [VSTOP]     = 0x13,        /* typical value for TERM=linux, Ctrl+S */
      [VSUSP]     = 0x1a,        /* typical value for TERM=linux, Ctrl+Z */
      [VEOL]      = 0,           /* typical value for TERM=linux (unset) */
      [VREPRINT]  = 0x12,        /* typical value for TERM=linux, Ctrl+R */
      [VDISCARD]  = 0x0f,        /* typical value for TERM=linux, Ctrl+O */
      [VWERASE]   = TERM_WERASE_C,
      [VLNEXT]    = 0x16,        /* typical value for TERM=linux, Ctrl+V */
      [VEOL2]     = 0            /* typical value for TERM=linux (unset) */
   }
};

void tty_update_special_ctrl_handlers(void);

static int tty_ioctl_tcgets(fs_handle h, void *argp)
{
   int rc = copy_to_user(argp, &c_term, sizeof(struct termios));

   if (rc < 0)
      return -EFAULT;

   return 0;
}

static int tty_ioctl_tcsets(fs_handle h, void *argp)
{
   struct termios saved = c_term;
   int rc = copy_from_user(&c_term, argp, sizeof(struct termios));

   if (rc < 0) {
      c_term = saved;
      return -EFAULT;
   }

   tty_update_special_ctrl_handlers();
   return 0;
}

static int tty_ioctl_tiocgwinsz(fs_handle h, void *argp)
{
   struct winsize sz = {
      .ws_row = term_get_rows(),
      .ws_col = term_get_cols(),
      .ws_xpixel = 0,
      .ws_ypixel = 0
   };

   int rc = copy_to_user(argp, &sz, sizeof(struct winsize));

   if (rc < 0)
      return -EFAULT;

   return 0;
}

int tty_ioctl(fs_handle h, uptr request, void *argp)
{
   switch (request) {

      case TCGETS:
         return tty_ioctl_tcgets(h, argp);

      case TCSETS:
         return tty_ioctl_tcsets(h, argp);

      case TCSETSW:
         // TODO: implement the correct behavior for TCSETSW
         return tty_ioctl_tcsets(h, argp);

      case TCSETSF:
         // TODO: implement the correct behavior for TCSETSF
         return tty_ioctl_tcsets(h, argp);

      case TIOCGWINSZ:
         return tty_ioctl_tiocgwinsz(h, argp);

      default:
         printk("WARNING: unknown tty_ioctl() request: %p\n", request);
         return -EINVAL;
   }
}
