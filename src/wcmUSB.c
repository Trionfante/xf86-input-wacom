/*
 * Copyright 1995-2003 by Frederic Lepied, France. <Lepied@XFree86.org>
 *                                                                            
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is  hereby granted without fee, provided that
 * the  above copyright   notice appear  in   all  copies and  that both  that
 * copyright  notice   and   this  permission   notice  appear  in  supporting
 * documentation, and that   the  name of  Frederic   Lepied not  be  used  in
 * advertising or publicity pertaining to distribution of the software without
 * specific,  written      prior  permission.     Frederic  Lepied   makes  no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.                   
 *                                                                            
 * FREDERIC  LEPIED DISCLAIMS ALL   WARRANTIES WITH REGARD  TO  THIS SOFTWARE,
 * INCLUDING ALL IMPLIED   WARRANTIES OF MERCHANTABILITY  AND   FITNESS, IN NO
 * EVENT  SHALL FREDERIC  LEPIED BE   LIABLE   FOR ANY  SPECIAL, INDIRECT   OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA  OR PROFITS, WHETHER  IN  AN ACTION OF  CONTRACT,  NEGLIGENCE OR OTHER
 * TORTIOUS  ACTION, ARISING    OUT OF OR   IN  CONNECTION  WITH THE USE    OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "xf86Wacom.h"

#if LINUX_INPUT

static Bool xf86WcmUSBDetect(LocalDevicePtr);
static Bool xf86WcmUSBInit(LocalDevicePtr pDev);
static void xf86WcmUSBRead(LocalDevicePtr pDev);

	WacomDeviceClass wcmUSBDevice =
	{
		xf86WcmUSBDetect,
		xf86WcmUSBInit,
		xf86WcmUSBRead,
	};

/*****************************************************************************
 * xf86WcmUSBDetect --
 *   Test if the attached device is USB.
 ****************************************************************************/

static Bool xf86WcmUSBDetect(LocalDevicePtr local)
{
	int version;
	int err;

	DBG(1, ErrorF("xf86WcmUSBDetect\n"));
    
	SYSCALL(err = ioctl(local->fd, EVIOCGVERSION, &version));
    
	if (!err)
	{
		ErrorF("%s Wacom Kernel Input driver version is %d.%d.%d\n",
				XCONFIG_PROBED, version >> 16,
				(version >> 8) & 0xff, version & 0xff);
		return 1;
	}

	return 0;
}

/*****************************************************************************
 * xf86WcmUSBRead --
 *   Read the new events from the device, and enqueue them.
 ****************************************************************************/

static void xf86WcmUSBRead(LocalDevicePtr local)
{
	WacomCommonPtr common = ((WacomDevicePtr)local->private)->common;
	WacomDeviceState ds;
	ssize_t len;
	int loop;
	struct input_event *event, *readevent, eventbuf[MAX_EVENTS];

	/* start from previous state */
	ds = common->wcmChannel[0].state;

	#define MOD_BUTTONS(bit, value) do { \
		ds.buttons = (((value) != 0) ? \
		(ds.buttons | (bit)) : (ds.buttons & ~(bit))); \
		} while (0)

	SYSCALL(len = xf86WcmRead(local->fd, eventbuf,
			sizeof(eventbuf))/sizeof(struct input_event));

	DBG(10, ErrorF("xf86WcmUSBRead read %d events\n", len));
    
	if (len <= 0)
	{
		ErrorF("Error reading wacom device : %s\n", strerror(errno));
		return;
	}

	common->wcmIndex = 0; /* wcmIndex means bytes not events. Purposely misused here? */
	for (readevent = eventbuf; readevent < (eventbuf+len); readevent++)
	{
		/* sanity check */
		if (common->wcmIndex >= MAX_USB_EVENTS)
		{
			DBG(11, ErrorF("xf86WcmUSBRead resetting buf index\n"));
			common->wcmIndex = 0;
		}
		    
		common->wcmEvent[common->wcmIndex++] = *readevent;
		
		/* MSC_SERIAL is the event terminator */
		if (!(readevent->type == EV_MSC && readevent->code == MSC_SERIAL))
	    continue;
	}

	for (loop=0; loop<common->wcmIndex; loop++)
	{
		event = common->wcmEvent + loop;

		DBG(11, ErrorF("xf86WcmUSBRead event[%d]->type=%d "
			"code=%d value=%d\n", loop, event->type,
			event->code, event->value));

		/* absolute events */
		if (event->type == EV_ABS)
		{
			if (event->code == ABS_X)
				ds.x = event->value;
			else if (event->code == ABS_Y)
				ds.y = event->value;
			else if (event->code == ABS_RZ)
				ds.rotation = event->value;
			else if (event->code == ABS_TILT_X)
				ds.tiltx = event->value;
			else if (event->code ==  ABS_TILT_Y)
				ds.tilty = event->value;
			else if (event->code == ABS_PRESSURE)
			{
				ds.pressure = event->value;
				MOD_BUTTONS (1, event->value >
					common->wcmThreshold ? 1 : 0);
			}
			else if (event->code == ABS_DISTANCE)
			{
			}
			else if (event->code == ABS_WHEEL)
				ds.wheel = event->value;
			else if (event->code == ABS_THROTTLE)
				ds.throttle = event->value;
		}

		else if (event->type == EV_REL)
		{
			if (event->code == REL_WHEEL)
				ds.wheel += event->value;
			else
			{
				ErrorF("wacom: rel event recv'd (%d)!\n",
					event->code);
			}
		}

		else if (event->type == EV_KEY)
		{
			if ((event->code == BTN_TOOL_PEN) ||
				(event->code == BTN_TOOL_PENCIL) ||
				(event->code == BTN_TOOL_BRUSH) ||
				(event->code == BTN_TOOL_AIRBRUSH))
			{
				ds.device_type = STYLUS_ID;
				ds.proximity = (event->value != 0);
				DBG(6, ErrorF("USB stylus detected %x\n",
					event->code));
			}
			else if (event->code == BTN_TOOL_RUBBER)
			{
				ds.device_type = ERASER_ID;
				ds.proximity = (event->value != 0);
				if (ds.proximity) 
					ds.proximity = ERASER_PROX;
				DBG(6, ErrorF("USB eraser detected %x\n",
					event->code));
			}
			else if ((event->code == BTN_TOOL_MOUSE) ||
				(event->code == BTN_TOOL_LENS))
			{
				DBG(6, ErrorF("USB mouse detected %x\n",						event->code));
				ds.device_type = CURSOR_ID;
				ds.proximity = (event->value != 0);
			}
			else if (event->code == BTN_TOUCH)
			{
				/* we use the pressure to determine
				 * the button 1 */
			}
			else if ((event->code == BTN_STYLUS) ||
				(event->code == BTN_MIDDLE))
			{
				MOD_BUTTONS (2, event->value);
			}
			else if ((event->code == BTN_STYLUS2) ||
				(event->code == BTN_RIGHT))
			{
				MOD_BUTTONS (4, event->value);
			}
			else if (event->code == BTN_LEFT)
				MOD_BUTTONS (1, event->value);
			else if (event->code == BTN_SIDE)
				MOD_BUTTONS (8, event->value);
			else if (event->code == BTN_EXTRA)
				MOD_BUTTONS (16, event->value);
		}

		else if (event->type == EV_MSC)
		{
			if (event->code == MSC_SERIAL)
			{
				ds.serial_num = event->value;
				DBG(10, ErrorF("wacom tool serial number=%d\n",
					ds.serial_num));
			}
		}
	} /* next event */

	/* dispatch event */
	xf86WcmEvent(common,0,&ds);
}

/*
 ***************************************************************************
 *
 * xf86WcmUSBInit --
 *
 ***************************************************************************
 */

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define test_bit(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)
#define OFF(x)   ((x)%BITS_PER_LONG)
#define LONG(x)  ((x)/BITS_PER_LONG)

static Bool xf86WcmUSBInit(LocalDevicePtr local)
{
	WacomDevicePtr priv = (WacomDevicePtr)local->private;
	WacomCommonPtr common = priv->common;
	char name[256] = "Unknown";
	int abs[5];
	unsigned long bit[EV_MAX][NBITS(KEY_MAX)];
	int i, j;

	DBG(1, ErrorF("initializing USB tablet\n"));    

	ioctl(local->fd, EVIOCGNAME(sizeof(name)), name);
	ErrorF("%s Wacom Kernel Input device name: \"%s\"\n", XCONFIG_PROBED, name);

	if (strstr(name, "Intuos"))
	{
		common->wcmResolX = common->wcmResolY = 2540;
	}
	else
	/* Graphire2 */
	{
		common->wcmResolX = common->wcmResolY = 1016;
	}

	memset(bit, 0, sizeof(bit));
	ioctl(local->fd, EVIOCGBIT(0, EV_MAX), bit[0]);
	
	for (i = 0; i < EV_MAX; i++)
	{
		if (test_bit(i, bit[0]))
		{
			ioctl(local->fd, EVIOCGBIT(i, KEY_MAX), bit[i]);
			for (j = 0; j < KEY_MAX; j++) 
			{
				if (test_bit(j, bit[i]))
				{
					if (i == EV_ABS)
					{
						ioctl(local->fd, EVIOCGABS(j), abs);
						switch (j)
						{
							case ABS_X:
								if (common->wcmMaxX == 0)
									common->wcmMaxX = abs[2];
								break;

							case ABS_Y:
								if (common->wcmMaxY == 0)
									common->wcmMaxY = abs[2];
								break;

							case ABS_PRESSURE:
								if (common->wcmMaxZ == 0)
									common->wcmMaxZ = abs[2];
								break;
						}
					}
				}
			}
		}
	}
    
	DBG(2, ErrorF("setup is max X=%d(%d) Y=%d(%d) Z=%d(%d)\n",
			common->wcmMaxX, common->wcmResolX,
			common->wcmMaxY, common->wcmResolY,
			common->wcmMaxZ, common->wcmResolZ));
  
	/* send the tilt mode command after setup because it must be enabled */
	/* after multi-mode to take precedence */
	if (HANDLE_TILT(common))
	{
		/* Unfortunately, the USB driver doesn't allow to send this
		 * command to the tablet. Any other solutions ? */
		DBG(2, ErrorF("Sending tilt mode order\n"));
	}

	if (xf86Verbose)
	ErrorF("%s Wacom tablet maximum X=%d maximum Y=%d "
			"X resolution=%d Y resolution=%d suppress=%d%s\n",
			XCONFIG_PROBED, common->wcmMaxX, common->wcmMaxY,
			common->wcmResolX, common->wcmResolY, common->wcmSuppress,
			HANDLE_TILT(common) ? " Tilt" : "");

	return Success;
}
#endif /* LINUX_INPUT */
