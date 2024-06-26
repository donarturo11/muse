//=========================================================
//  MusE
//  Linux Music Editor
//  $Id: rtctimer.cpp,v 1.1.2.11 2009/03/09 02:05:18 terminator356 Exp $
//
//  Most code moved from midiseq.cpp by Werner Schweer.
//
//  (C) Copyright -2004 Werner Schweer (werner@seh.de)
//  (C) Copyright 2004 Robert Jonsson (rj@spamatica.se)
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================

#include "rtctimer.h"

#ifdef ALSA_SUPPORT

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
#include <linux/spinlock.h>
#include <linux/mc146818rtc.h>
#else
#include <linux/rtc.h>
//#include <poll.h>
#endif
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "globals.h"

namespace MusECore {

RtcTimer::RtcTimer()
    {
    timerFd = -1;
    }

RtcTimer::~RtcTimer()
    {
    if (timerFd != -1)
      close(timerFd);
    }

signed int RtcTimer::initTimer(unsigned long desiredFrequency)
    {
    if(TIMER_DEBUG)
          printf("RtcTimer::initTimer()\n");
    if (timerFd != -1) {
          fprintf(stderr,"RtcTimer::initTimer(): called on initialised timer!\n");
          return -1;
    }
    MusEGlobal::doSetuid();

#ifdef _WIN32
    QTimer *timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(showGPS()));
	timer->start(15000); //time specified in ms
//https://stackoverflow.com/questions/19287550/how-to-call-a-function-after-every-15-seconds-using-qt
//http://doc.qt.io/qt-5/qtimer.html#details
//http://doc.qt.io/qt-5/qt.html#TimerType-enum
#else
    timerFd = ::open("/dev/rtc", O_RDONLY);
    if (timerFd == -1) {
          fprintf(stderr, "fatal error: open /dev/rtc failed: %s\n", strerror(errno));
          MusEGlobal::undoSetuid();
          return timerFd;
          }
    if (!setTimerFreq(desiredFrequency)) {
          // unable to set timer frequency
          return -1;
          }
    // check if timer really works, start and stop it once.
    if (!startTimer()) {
          return -1;
          }
    if (!stopTimer()) {
          return -1;
          }
    return timerFd;
#endif
    }

unsigned long RtcTimer::setTimerResolution(unsigned long resolution)
    {
    if(TIMER_DEBUG)
      printf("RtcTimer::setTimerResolution(%lu)\n",resolution);
    /* The RTC can take power-of-two frequencies from 2 to 8196 Hz.
     * It doesn't really have a resolution as such.
     */
    return 0;
    }

unsigned long RtcTimer::setTimerFreq(unsigned long freq)
    {
#ifdef _WIN32
//    int rc = _getch(timerFd, );
#else
    int rc = ioctl(timerFd, RTC_IRQP_SET, freq); //RTC_IRQP_SET = set periodic interrupt frequency
#endif
    if (rc == -1) {
            fprintf(stderr, "RtcTimer::setTimerFreq(): cannot set freq %lu on /dev/rtc: %s\n", freq,
               strerror(errno));
            fprintf(stderr, "  precise timer not available, check file permissions and allowed RTC freq (/sys/class/rtc/rtc0/max_user_freq)\n");
            return 0;
            }
    return freq;
    }

unsigned long RtcTimer::getTimerResolution()
    {
    /* The RTC doesn't really work with a set resolution as such.
     * Not sure how this fits into things yet.
     */
    return 0;
    }

unsigned long RtcTimer::getTimerFreq()
    {
    unsigned long freq;
    int rv = ioctl(timerFd, RTC_IRQP_READ, &freq);
    if (rv < 0)
      return 0;
    return freq;
    }

bool RtcTimer::startTimer()
    {
    if(TIMER_DEBUG)
      printf("RtcTimer::startTimer()\n");
    if (timerFd == -1) {
          fprintf(stderr, "RtcTimer::startTimer(): no timer open to start!\n");
          return false;
          }
    if (ioctl(timerFd, RTC_PIE_ON, 0) == -1) {
        perror("MidiThread: start: RTC_PIE_ON failed");
        MusEGlobal::undoSetuid();
        return false;
        }
    return true;
    }

bool RtcTimer::stopTimer()
    {
    if(TIMER_DEBUG)
      printf("RtcTimer::stopTimer\n");
    if (timerFd != -1) {
        ioctl(timerFd, RTC_PIE_OFF, 0);
        }
    else {
        fprintf(stderr,"RtcTimer::stopTimer(): no RTC to stop!\n");
        return false;
        }
    return true;
    }

unsigned long RtcTimer::getTimerTicks(bool /*printTicks*/)// prevent compiler warning: unused parameter
    {
    if(TIMER_DEBUG)
      printf("getTimerTicks()\n");
    unsigned long int nn;
    if (timerFd==-1) {
        fprintf(stderr,"RtcTimer::getTimerTicks(): no RTC open to read!\n");
        return 0;
    }
    if (read(timerFd, &nn, sizeof(unsigned long)) != sizeof(unsigned long)) {
        fprintf(stderr,"RtcTimer::getTimerTicks(): error reading RTC\n");
        return 0;
        }
    return nn;
    }

} // namespace MusECore

#endif // ALSA_SUPPORT
