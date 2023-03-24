/*********************************************************
 * Copyright (C) 2023 VMware, Inc. All rights reserved.
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

/**
 * @file slewAdjtime.c
 *
 * Implementation of slewing using Posix adjtime system call.
 */

#include "timeSync.h"
#include "timeSyncPosix.h"

#include <glib.h>
#include <sys/time.h>

/*
 ******************************************************************************
 * TimeSync_DisableTimeSlew --                                          */ /**
 *
 * Disable time slewing, canceling any pending slew.
 *
 * @return TRUE on success.
 *
 ******************************************************************************
 */

Bool
TimeSync_DisableTimeSlew(void)
{
   return TRUE;
}


/*
 ******************************************************************************
 * TimeSync_Slew --                                                     */ /**
 *
 * Slew the clock, correcting 'delta' microseconds.  timeSyncPeriod is
 * ignored by this implementation.  Report the amount of the previous
 * correction that has not been applied.
 *
 * @param[in]  delta              Correction to apply in us.
 * @param[in]  timeSyncPeriod     Time interval in us.
 * @param[out] remaining          Amount of previous correction not applied.
 *
 * @return TRUE on success.
 *
 ******************************************************************************
 */

Bool
TimeSync_Slew(int64 delta,
              int64 timeSyncPeriod,
              int64 *remaining)
{
   return FALSE;
}
