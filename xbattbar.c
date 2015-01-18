/*
 * $Id: xbattbar.c,v 1.16.2.4 2001/02/02 05:25:29 suguru Exp $
 *
 * xbattbar: yet another battery watcher for X11
 */

/*
 * Copyright (c) 1998-2001 Suguru Yamaguchi <suguru@wide.ad.jp>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 */

static char *ReleaseVersion="1.4.2";

#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <X11/Xlib.h>

#define PollingInterval 10	/* APM polling interval in sec */
#define BI_THICKNESS    3	/* battery indicator thickness in pixels */

#define BI_Bottom	0
#define BI_Top		1
#define BI_Left		2
#define BI_Right	3
#define BI_Horizontal	((bi_direction & 2) == 0)
#define BI_Vertical	((bi_direction & 2) == 2)

#define myEventMask (ExposureMask|EnterWindowMask|LeaveWindowMask|VisibilityChangeMask)
#define DefaultFont "fixed"
#define DiagXMergin 20
#define DiagYMergin 5

/*
 * Global variables
 */

int ac_line = -1;               /* AC line status */
int battery_level = -1;         /* battery level */

unsigned long onin, onout;      /* indicator colors for AC online */
unsigned long offin, offout;    /* indicator colors for AC offline */

int elapsed_time = 0;           /* for battery remaining estimation */

/* indicator default colors */
char *ONIN_C   = "green";
char *ONOUT_C  = "olive drab";
char *OFFIN_C  = "blue";
char *OFFOUT_C = "red";

int alwaysontop = False;

struct itimerval IntervalTimer;     /* APM polling interval timer */

int bi_direction = BI_Bottom;       /* status bar location */
int bi_height;                      /* height of Battery Indicator */
int bi_width;                       /* width of Battery Indicator */
int bi_x;                           /* x coordinate of upper left corner */
int bi_y;                           /* y coordinate of upper left corner */
int bi_thick = BI_THICKNESS;        /* thickness of Battery Indicator */
int bi_interval = PollingInterval;  /* interval of polling APM */

Display *disp;
Window winbar;                  /* bar indicator window */
Window winstat = -1;            /* battery status window */
GC gcbar;
GC gcstat;
unsigned int width,height;
XEvent theEvent;

/*
 * function prototypes
 */
void InitDisplay(void);
Status AllocColor(char *, unsigned long *);
void battery_check(void);
void plug_proc(int);
void battery_proc(int);
void redraw(void);
void showdiagbox(void);
void disposediagbox(void);
void usage(char **);
void about_this_program(void);
void estimate_remain(void);

/*
 * usage of this command
 */
void about_this_program()
{
  fprintf(stderr, 
	  "This is xbattbar version %s, "
	  "copyright (c)1998-2001 Suguru Yamaguchi\n",
	  ReleaseVersion);
}

void usage(char **argv)
{
  fprintf(stderr,
    "\n"	  
    "usage:\t%s [-a] [-h|v] [-p sec] [-t thickness]\n"
    "\t\t[-I color] [-O color] [-i color] [-o color]\n"
    "\t\t[ top | bottom | left | right ]\n"
    "-a:     always on top.\n"
    "-v, -h: show this message.\n"
    "-t:     bar (indicator) thickness. [def: 3 pixels]\n"
    "-p:     polling interval. [def: 10 sec.]\n"
    "-I, -O: bar colors in AC on-line. [def: \"green\" & \"olive drab\"]\n"
    "-i, -o: bar colors in AC off-line. [def: \"blue\" and \"red\"]\n"
    "top, bottom, left, right: bar localtion. [def: \"bottom\"]\n",
	  argv[0]);
  exit(0);
}

/*
 * AllocColor:
 * convert color name to pixel value
 */
Status AllocColor(char *name, unsigned long *pixel)
{
  XColor color,exact;
  int status;

  status = XAllocNamedColor(disp, DefaultColormap(disp, 0),
                           name, &color, &exact);
  *pixel = color.pixel;

  return(status);
}

/*
 * InitDisplay:
 * create small window in top or bottom
 */
void InitDisplay(void)
{
  Window root;
  int x,y;
  unsigned int border,depth;
  XSetWindowAttributes att;

  if((disp = XOpenDisplay(NULL)) == NULL) {
      fprintf(stderr, "xbattbar: can't open display.\n");
      exit(1);
  }

  if(XGetGeometry(disp, DefaultRootWindow(disp), &root, &x, &y,
                 &width, &height, &border, &depth) == 0) {
    fprintf(stderr, "xbattbar: can't get window geometry\n");
    exit(1);
  }

  if (!AllocColor(ONIN_C,&onin) ||
       !AllocColor(OFFOUT_C,&offout) ||
       !AllocColor(OFFIN_C,&offin) ||
       !AllocColor(ONOUT_C,&onout)) {
    fprintf(stderr, "xbattbar: can't allocate color resources\n");
    exit(1);
  }

  switch (bi_direction) {
  case BI_Top: /* (0,0) - (width, bi_thick) */
    bi_width = width;
    bi_height = bi_thick;
    bi_x = 0;
    bi_y = 0;
    break;
  case BI_Bottom:
    bi_width = width;
    bi_height = bi_thick;
    bi_x = 0;
    bi_y = height - bi_thick;
    break;
  case BI_Left:
    bi_width = bi_thick;
    bi_height = height;
    bi_x = 0;
    bi_y = 0;
    break;
  case BI_Right:
    bi_width = bi_thick;
    bi_height = height;
    bi_x = width - bi_thick;
    bi_y = 0;
  }

  winbar = XCreateSimpleWindow(disp, DefaultRootWindow(disp),
                              bi_x, bi_y, bi_width, bi_height,
                              0, BlackPixel(disp,0), WhitePixel(disp,0));

  /* make this window without its titlebar */
  att.override_redirect = True;
  XChangeWindowAttributes(disp, winbar, CWOverrideRedirect, &att);

  XMapWindow(disp, winbar);

  gcbar = XCreateGC(disp, winbar, 0, 0);
}

main(int argc, char **argv)
{
  extern char *optarg;
  extern int optind;
  int ch;

  about_this_program();
  while ((ch = getopt(argc, argv, "at:f:hI:i:O:o:p:v")) != -1)
    switch (ch) {
    case 'a':
      alwaysontop = True;
      break;

    case 't':
    case 'f':
      bi_thick = atoi(optarg);
      break;

    case 'I':
      ONIN_C = optarg;
      break;
    case 'i':
      OFFIN_C = optarg;
      break;
    case 'O':
      ONOUT_C = optarg;
      break;
    case 'o':
      OFFOUT_C = optarg;
      break;

    case 'p':
      bi_interval = atoi(optarg);
      break;

    case 'h':
    case 'v':
      usage(argv);
      break;
    }
  argc -= optind;
  argv += optind;

  if (argc > 0) {
    if (strcasecmp(*argv, "top") == 0)
      bi_direction = BI_Top;
    else if (strcasecmp(*argv, "bottom") == 0)
      bi_direction = BI_Bottom;
    else if (strcasecmp(*argv, "left") == 0)
      bi_direction = BI_Left;
    else if (strcasecmp(*argv, "right") == 0)
      bi_direction = BI_Right;
  }

  /*
   * set APM polling interval timer
   */
  IntervalTimer.it_interval.tv_sec = (long)bi_interval;
  IntervalTimer.it_interval.tv_usec = (long)0;
  IntervalTimer.it_value.tv_sec = (long)1;
  IntervalTimer.it_value.tv_usec = (long)0;
  if ( setitimer(ITIMER_REAL, &IntervalTimer, NULL) != 0 ) {
    fprintf(stderr,"xbattbar: can't set interval timer\n");
    exit(1);
  }

  /*
   * X Window main loop
   */
  InitDisplay();
  signal(SIGALRM, (void *)(battery_check));
  battery_check();
  XSelectInput(disp, winbar, myEventMask);
  while (1) {
    XWindowEvent(disp, winbar, myEventMask, &theEvent);
    switch (theEvent.type) {
    case Expose:
      /* we redraw our window since our window has been exposed. */
      redraw();
      break;

    case EnterNotify:
      /* create battery status message */
      showdiagbox();
      break;

    case LeaveNotify:
      /* destroy status window */
      disposediagbox();
      break;

    case VisibilityNotify:
      if (alwaysontop) XRaiseWindow(disp, winbar);
      break;

    default:
      /* for debugging */
      fprintf(stderr, 
	      "xbattbar: unknown event (%d) captured\n",
	      theEvent.type);
    }
  }
}

void redraw(void)
{
  if (ac_line) {
    plug_proc(battery_level);
  } else {
    battery_proc(battery_level);
  }
  estimate_remain();
}


void showdiagbox(void)
{
  XSetWindowAttributes att;
  XFontStruct *fontp;
  XGCValues theGC;
  int pixw, pixh;
  int boxw, boxh;
  char diagmsg[64];

  /* compose diag message and calculate its size in pixels */
  sprintf(diagmsg,
         "AC %s-line: battery level is %d%%",
         ac_line ? "on" : "off", battery_level);
  fontp = XLoadQueryFont(disp, DefaultFont);
  pixw = XTextWidth(fontp, diagmsg, strlen(diagmsg));
  pixh = fontp->ascent + fontp->descent;
  boxw = pixw + DiagXMergin * 2;
  boxh = pixh + DiagYMergin * 2;

  /* create status window */
  if(winstat != -1) disposediagbox();
  winstat = XCreateSimpleWindow(disp, DefaultRootWindow(disp),
                               (width-boxw)/2, (height-boxh)/2,
                               boxw, boxh,
                               2, BlackPixel(disp,0), WhitePixel(disp,0));

  /* make this window without time titlebar */
  att.override_redirect = True;
  XChangeWindowAttributes(disp, winstat, CWOverrideRedirect, &att);
  XMapWindow(disp, winstat);
  theGC.font = fontp->fid;
  gcstat = XCreateGC(disp, winstat, GCFont, &theGC);
  XDrawString(disp, winstat,
             gcstat,
             DiagXMergin, fontp->ascent+DiagYMergin,
             diagmsg, strlen(diagmsg));
}

void disposediagbox(void)
{
  if ( winstat != -1 ) {
    XDestroyWindow(disp, winstat);
    winstat = -1;
  }
}

void battery_proc(int left)
{
  int pos;
  if (BI_Horizontal) {
    pos = width * left / 100;
    XSetForeground(disp, gcbar, offin);
    XFillRectangle(disp, winbar, gcbar, 0, 0, pos, bi_thick);
    XSetForeground(disp, gcbar, offout);
    XFillRectangle(disp, winbar, gcbar, pos, 0, width, bi_thick);
  } else {
    pos = height * left / 100;
    XSetForeground(disp, gcbar, offin);
    XFillRectangle(disp, winbar, gcbar, 0, height-pos, bi_thick, height);
    XSetForeground(disp, gcbar, offout);
    XFillRectangle(disp, winbar, gcbar, 0, 0, bi_thick, height-pos);
  }
  XFlush(disp);
}

void plug_proc(int left)
{
  int pos;

  if (BI_Horizontal) {
    pos = width * left / 100;
    XSetForeground(disp, gcbar, onin);
    XFillRectangle(disp, winbar, gcbar, 0, 0, pos, bi_thick);
    XSetForeground(disp, gcbar, onout);
    XFillRectangle(disp, winbar, gcbar, pos+1, 0, width, bi_thick);
  } else {
    pos = height * left / 100;
    XSetForeground(disp, gcbar, onin);
    XFillRectangle(disp, winbar, gcbar, 0, height-pos, bi_thick, height);
    XSetForeground(disp, gcbar, onout);
    XFillRectangle(disp, winbar, gcbar, 0, 0, bi_thick, height-pos);
  }
  XFlush(disp);
}


/*
 * estimating time for battery remaining / charging 
 */

#define CriticalLevel  5

void estimate_remain()
{
  static int battery_base = -1;
  int diff;
  int remain;

  /* static value initialize */
  if (battery_base == -1) {
    battery_base = battery_level;
    return;
  }

  diff = battery_base - battery_level;

  if (diff == 0) return;

  /* estimated time for battery remains */
  if (diff > 0) {
    remain = elapsed_time * (battery_level - CriticalLevel) / diff ;
    remain = remain * bi_interval;  /* in sec */
    if (remain < 0 ) remain = 0;
    printf("battery remain: %2d hr. %2d min. %2d sec.\n",
	   remain / 3600, (remain % 3600) / 60, remain % 60);
    elapsed_time = 0;
    battery_base = battery_level;
    return;
  }

  /* estimated time of battery charging */
  remain = elapsed_time * (battery_level - 100) / diff;
  remain = remain * bi_interval;  /* in sec */
  printf("charging remain: %2d hr. %2d min. %2d sec.\n",
	 remain / 3600, (remain % 3600) / 60, remain % 60);
  elapsed_time = 0;
  battery_base = battery_level;
}



#ifdef __bsdi__

#include <machine/apm.h>
#include <machine/apmioctl.h>

int first = 1;
void battery_check(void)
{
  int fd;
  struct apmreq ar ;

  ++elapsed_time;

  ar.func = APM_GET_POWER_STATUS ;
  ar.dev = APM_DEV_ALL ;

  if ((fd = open(_PATH_DEVAPM, O_RDONLY)) < 0) {
    perror(_PATH_DEVAPM) ;
    exit(1) ;
  }
  if (ioctl(fd, PIOCAPMREQ, &ar) < 0) {
    fprintf(stderr, "xbattbar: PIOCAPMREQ: APM_GET_POWER_STATUS error 0x%x\n", ar.err);
  }
  close (fd);

  if (first || ac_line != ((ar.bret >> 8) & 0xff)
      || battery_level != (ar.cret&0xff)) {
    first = 0;
    ac_line = (ar.bret >> 8) & 0xff;
    battery_level = ar.cret&0xff;
    redraw();
  }
  signal(SIGALRM, (void *)(battery_check));
}

#endif /* __bsdi__ */

#ifdef __FreeBSD__

#include <machine/apm_bios.h>

#define APMDEV21       "/dev/apm0"
#define APMDEV22       "/dev/apm"

#define        APM_STAT_UNKNOWN        255

#define        APM_STAT_LINE_OFF       0
#define        APM_STAT_LINE_ON        1

#define        APM_STAT_BATT_HIGH      0
#define        APM_STAT_BATT_LOW       1
#define        APM_STAT_BATT_CRITICAL  2
#define        APM_STAT_BATT_CHARGING  3

int first = 1;
void battery_check(void)
{
  int fd, r, p;
  struct apm_info     info;

  if ((fd = open(APMDEV21, O_RDWR)) == -1 &&
      (fd = open(APMDEV22, O_RDWR)) == -1) {
    fprintf(stderr, "xbattbar: cannot open apm device\n");
    exit(1);
  }
  if (ioctl(fd, APMIO_GETINFO, &info) == -1) {
    fprintf(stderr, "xbattbar: ioctl APMIO_GETINFO failed\n");
    exit(1);
  }
  close (fd);

  ++elapsed_time;

  /* get current status */
  if (info.ai_batt_life == APM_STAT_UNKNOWN) {
    switch (info.ai_batt_stat) {
    case APM_STAT_BATT_HIGH:
      r = 100;
      break;
    case APM_STAT_BATT_LOW:
      r = 40;
      break;
    case APM_STAT_BATT_CRITICAL:
      r = 10;
      break;
    default:        /* expected to be APM_STAT_UNKNOWN */
      r = 100;
    }
  } else if (info.ai_batt_life > 100) {
    /* some APM BIOSes return values slightly > 100 */
    r = 100;
  } else {
    r = info.ai_batt_life;
  }

  /* get AC-line status */
  if (info.ai_acline == APM_STAT_LINE_ON) {
    p = APM_STAT_LINE_ON;
  } else {
    p = APM_STAT_LINE_OFF;
  }

  if (first || ac_line != p || battery_level != r) {
    first = 0;
    ac_line = p;
    battery_level = r;
    redraw();
  }
  signal(SIGALRM, (void *)(battery_check));
}

#endif /* __FreeBSD__ */

#ifdef __NetBSD__

#include <machine/apmvar.h>

#define _PATH_APM_SOCKET       "/var/run/apmdev"
#define _PATH_APM_CTLDEV       "/dev/apmctl"
#define _PATH_APM_NORMAL       "/dev/apm"

int first = 1;
void battery_check(void)
{
       int fd, r, p;
       struct apm_power_info info;

       if ((fd = open(_PATH_APM_NORMAL, O_RDONLY)) == -1) {
               fprintf(stderr, "xbattbar: cannot open apm device\n");
               exit(1);
       }

       if (ioctl(fd, APM_IOC_GETPOWER, &info) != 0) {
               fprintf(stderr, "xbattbar: ioctl APM_IOC_GETPOWER failed\n");
               exit(1);
       }

       close(fd);

       ++elapsed_time;

       /* get current remoain */
       if (info.battery_life > 100) {
               /* some APM BIOSes return values slightly > 100 */
               r = 100;
       } else {
               r = info.battery_life;
       }

       /* get AC-line status */
       if (info.ac_state == APM_AC_ON) {
               p = APM_AC_ON;
       } else {
               p = APM_AC_OFF;
       }

       if (first || ac_line != p || battery_level != r) {
               first = 0;
               ac_line = p;
               battery_level = r;
               redraw();
       }
}

#endif /* __NetBSD__ */


#ifdef linux

#include <errno.h>
#include <linux/apm_bios.h>

#define APM_PROC	"/proc/apm"

#define        APM_STAT_LINE_OFF       0
#define        APM_STAT_LINE_ON        1

typedef struct apm_info {
   const char driver_version[10];
   int        apm_version_major;
   int        apm_version_minor;
   int        apm_flags;
   int        ac_line_status;
   int        battery_status;
   int        battery_flags;
   int        battery_percentage;
   int        battery_time;
   int        using_minutes;
} apm_info;


int first = 1;

#define SYS_CAPACITY "/sys/class/power_supply/battery/capacity"
#define SYS_STATUS   "/sys/class/power_supply/battery/status"

static void battery_check_sys(void)
{
  int r,p;
  FILE *pt;
  int percentage;
  int charging;
  struct apm_info i;
  char buf[100];

  /* get current status */
  if ( (pt = fopen( SYS_CAPACITY, "r" )) == NULL) {
    fprintf(stderr, "xbattbar: Can't read %s: %s\n",
	    SYS_CAPACITY, strerror(errno));
    exit(1);
  }
  fgets( buf, sizeof( buf ) - 1, pt );
  buf[ sizeof( buf ) - 1 ] = '\0';
  sscanf(buf, "%d\n", &percentage);
  fclose (pt);

  if ( (pt = fopen( SYS_STATUS, "r" )) == NULL) {
    fprintf(stderr, "xbattbar: Can't read %s: %s\n",
	    SYS_STATUS, strerror(errno));
    exit(1);
  }
  fgets( buf, sizeof( buf ) - 1, pt );
  charging = strncmp(buf, "Charging\n", sizeof(buf)) == 0;
  fclose (pt);

  ++elapsed_time;

  /* some APM BIOSes return values slightly > 100 */
  if ( percentage > 100 )
    percentage = 100;

  /* get AC-line status */
  p = charging ? APM_STAT_LINE_ON : APM_STAT_LINE_OFF;

  if (first || ac_line != p || battery_level != percentage) {
    first = 0;
    ac_line = p;
    battery_level = percentage;
    redraw();
  }
  signal(SIGALRM, (void *)(battery_check));
}

static void battery_check_apm(void)
{
  int r,p;
  FILE *pt;
  struct apm_info i;
  char buf[100];

  /* get current status */
  errno = 0;
  if ( (pt = fopen( APM_PROC, "r" )) == NULL) {
    fprintf(stderr, "xbattbar: Can't read proc info: %s\n", strerror(errno));
    signal(SIGALRM, (void *)(battery_check));
    exit(1);
  }

  fgets( buf, sizeof( buf ) - 1, pt );
  buf[ sizeof( buf ) - 1 ] = '\0';
  sscanf( buf, "%s %d.%d %x %x %x %x %d%% %d %d\n",
	  &i.driver_version,
	  &i.apm_version_major,
	  &i.apm_version_minor,
	  &i.apm_flags,
	  &i.ac_line_status,
	  &i.battery_status,
	  &i.battery_flags,
	  &i.battery_percentage,
	  &i.battery_time,
	  &i.using_minutes );

  fclose (pt);

  ++elapsed_time;

   /* some APM BIOSes return values slightly > 100 */
   if ( (r = i.battery_percentage) > 100 ){
     r = 100;
   }

   /* get AC-line status */
   if ( i.ac_line_status == APM_STAT_LINE_ON) {
     p = APM_STAT_LINE_ON;
   } else {
     p = APM_STAT_LINE_OFF;
   }

  if (first || ac_line != p || battery_level != r) {
    first = 0;
    ac_line = p;
    battery_level = r;
    redraw();
  }
  signal(SIGALRM, (void *)(battery_check));
}

void battery_check(void)
{
  struct stat st;
  if (lstat(APM_PROC, &st) != -1)
    battery_check_apm();
  else if (lstat(SYS_CAPACITY, &st) != -1)
    battery_check_sys();
  else {
    fprintf(stderr, "xbattbar: Neither apm nor power_supply files found\n");
    exit(1);
  }
}

#endif /* linux */

