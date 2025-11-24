/* omron_usb.c - NUT driver for omron UPS

   Copyright (C)
       2025 Masahiro Kamikawa <masahiro@nanodesumu.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "config.h" /* must be the first header */

#include "main.h"
#include <libusb.h>

#define DRIVER_NAME	"omron ups driver"
#define DRIVER_VERSION	"0.10"

static int ondelay = 3;	/* minutes */
static int offdelay = 30;	/* seconds */
static int stayoff = 0;
static uint16_t productid = 0x0081;
static uint16_t vendorid = 0x0590;

#define USB_REQ_TYPE (LIBUSB_ENDPOINT_OUT + LIBUSB_REQUEST_TYPE_CLASS + LIBUSB_RECIPIENT_INTERFACE)
#define USB_RX_TRUNK_SIZE 64
static libusb_device_handle *udev = NULL;

/* driver description structure */
upsdrv_info_t upsdrv_info =
{
	DRIVER_NAME,
	DRIVER_VERSION,
	"Masahiro Kamikawa <masahiro@nanodesumu.com>",
	DRV_STABLE,
	{ NULL }
};

void OpenUsbPort() {
	udev = libusb_open_device_with_vid_pid(NULL, vendorid, productid);
	if (udev == NULL) {
		upslogx(LOG_ERR, "device not found");
		return;
	}

	int ret = libusb_kernel_driver_active(udev, 0);
	if (ret) {
		ret = libusb_detach_kernel_driver(udev, 0);
	}
	if (ret) {
		upslogx(LOG_ERR, "can not detach kernel driver, err code = %d", ret);
		libusb_close(udev);
		udev = NULL;
		return;
	}
	ret = libusb_claim_interface(udev, 0);
	if (ret) {
		upslogx(LOG_ERR, "can not claim device, err code = %d", ret);
		libusb_close(udev);
		udev = NULL;
		return;
	}
	upslogx(LOG_INFO, "UPS connected");
}

void CloseUsbPort() {
	if (udev) {
		libusb_release_interface(udev, 0x0);
		libusb_close(udev);
		udev = NULL;
	}
}

/*
 * Generic command processing function. Send a command and read a reply.
 * Returns 0 on error, the number of bytes read on success.
 */
int ups_command(const char *cmd, char *buf, int buflen) {
	if (udev == NULL) {
		OpenUsbPort();
	}
	if (udev == NULL) {
		return 0;
	}

	// TX
	upsdebugx(3, "send: %s", cmd);
	int len_cmd = strlen(cmd);
	unsigned char* tx_buf = (unsigned char*)malloc(len_cmd+1);
	if (tx_buf == NULL) {
		upslogx(LOG_ERR, "can not alloc tx buffer");
		return 0;
	}
	memcpy(tx_buf, cmd, len_cmd);
	tx_buf[len_cmd] = '\r';
	len_cmd += 1;
	
	// send HID SET_REPORT request
	int len_tx = libusb_control_transfer(udev, USB_REQ_TYPE, 0x09, 0x200, 0x0, tx_buf, len_cmd, 100);

	free(tx_buf);

	if (len_tx != len_cmd) {
		upslogx(LOG_ERR, "can not write ups port, err code = %d", len_tx);
		CloseUsbPort();
		return 0;
	}

	// RX
	unsigned char rx_buf[USB_RX_TRUNK_SIZE];
	int rx_num = 0;
	int ret = 0;
	while (ret == 0 && rx_num == 0) {
		ret = libusb_interrupt_transfer(udev, 0x81, rx_buf, USB_RX_TRUNK_SIZE, &rx_num, 400);
		if (ret == LIBUSB_ERROR_PIPE) {
			upsdebugx(1, "Clear stall condition");
			ret = libusb_clear_halt(udev, 0x81);
			rx_num = 0;
		}
	}

	if (ret) {
		upslogx(LOG_ERR, "read port fail, err code = %d", ret);
		CloseUsbPort();
		return 0;
	}

	int i;
	for (i=0; i<rx_num && i<buflen; i++) {
		if (rx_buf[i] == '\r') {
			buf[i] = 0;
			break;
		} else {
			buf[i] = rx_buf[i];
		}
	}

	upsdebugx(3, "receive: %s", buf);
	return i+1;
}

void upsdrv_help(void) {
}

void upsdrv_makevartable(void) {
	addvar(VAR_VALUE, "ondelay", "Delay before UPS startup (minutes)");
	addvar(VAR_VALUE, "offdelay", "Delay before UPS shutdown (seconds)");
	addvar(VAR_FLAG, "stayoff", "Do not restart after shutdown (flag)");
	addvar(VAR_VALUE, "vendorid", "ups vendorid (16bit hex)");
	addvar(VAR_VALUE, "productid", "ups productid (16bit hex)");
}

static int omron_instcmd(const char *cmdname, const char *extra) {
	const struct {
		const char *cmd;
		const char *ups;
	} instcmd[] = {
		{ "test.battery.start.quick", "T" },
		{ "shutdown.stop", "C" },
		{ NULL, NULL }
	};

	char	buf[SMALLBUF] = "";
	int	i;

	upslogx(LOG_INFO, "instcmd(%s, %s)", cmdname, extra ? extra : "[NULL]");

	for (i = 0; instcmd[i].cmd; i++) {

		if (strcasecmp(cmdname, instcmd[i].cmd)) {
			continue;
		}

		snprintf(buf, sizeof(buf), "%s", instcmd[i].ups);

		/*
		 * If a command is invalid, it will be echoed back
		 * As an exception, Best UPS units will report "OK" in case of success!
		 */
		if (ups_command(buf, buf, SMALLBUF) > 0) {
			if (strncmp(buf, "OK", 2)) {
				upslogx(LOG_ERR, "instcmd: command [%s] failed", cmdname);
				return STAT_INSTCMD_FAILED;
			}
		}

		upslogx(LOG_INFO, "instcmd: command [%s] handled", cmdname);
		return STAT_INSTCMD_HANDLED;
	}

	if (!strcasecmp(cmdname, "shutdown.return")) {
		/*
		 * SnRm: Shutdown after n minutes and then turn on after m minutes
		 * Accepted values for n: .2 -> .9 , 01 -> 20
		 * Accepted values for m: 00001 -> 09999
		 */
		if (offdelay < 60) {
			snprintf(buf, sizeof(buf), "S.%uR%05u", offdelay / 6, ondelay);
		} else {
			snprintf(buf, sizeof(buf), "S%02uR%05u", offdelay / 60, ondelay);
		}
	} else if (!strcasecmp(cmdname, "shutdown.stayoff")) {
		/*
		 * Sfn
		 * Shutdown after n minutes and stay off
		 * Accepted values for n: .2 -> .9 , 01 -> 20
		 */
		if (offdelay < 60) {
			snprintf(buf, sizeof(buf), "Sf.%u", offdelay / 6);
		} else {
			snprintf(buf, sizeof(buf), "Sf%02u", offdelay / 60);
		}
	} else {
		upslogx(LOG_ERR, "instcmd: command [%s] not found", cmdname);
		return STAT_INSTCMD_UNKNOWN;
	}

	/*
	 * If a command is invalid, it will be echoed back.
	 * As an exception, Best UPS units will report "OK" in case of success!
	 */
	if (ups_command(buf, buf, SMALLBUF) > 0) {
		if (strncmp(buf, "OK", 2)) {
			upslogx(LOG_ERR, "instcmd: command [%s] failed", cmdname);
			return STAT_INSTCMD_FAILED;
		}
	}

	upslogx(LOG_INFO, "instcmd: command [%s] handled", cmdname);
	return STAT_INSTCMD_HANDLED;
}

//upsdrv_initups
//Open the port (device_path) and do any low-level things that it may need to start using that port. If you have to set DTR or RTS on a serial port, do it here.
//Don’t do any sort of hardware detection here, since you may be going into upsdrv_shutdown next.
void upsdrv_initups(void) {
	const char	*val;

	val = getval("vendorid");
	if (val) {
		vendorid = strtol(val, NULL, 16);
	}

	val = getval("productid");
	if (val) {
		productid = strtol(val, NULL, 16);
	}

	val = getval("ondelay");
	if (val) {
		ondelay = strtol(val, NULL, 10);
	}

	if ((ondelay < 0) || (ondelay > 9999)) {
		ondelay = 3;
	}

	val = getval("offdelay");
	if (val) {
		offdelay = strtol(val, NULL, 10);
	}

	if (offdelay < 12) {
		offdelay = 12;
	}

	if (offdelay > 1200) {
		offdelay = 1200;
	}

	/* Truncate to nearest setable value */
	if (offdelay < 60) {
		offdelay -= (offdelay % 6);
	} else {
		offdelay -= (offdelay % 60);
	}

	stayoff = testvar("stayoff");

	if (libusb_init(NULL) < 0) {
		libusb_exit(NULL);
		fatal_with_errno(EXIT_FAILURE, "Failed to init libusb 1.0");
	}
	OpenUsbPort();

	return;
}

//upsdrv_initinfo
//Try to detect what kind of UPS is out there, if any, assuming that’s possible for your hardware. If there is a way to detect that hardware and it doesn’t appear to be connected, display an error and exit. This is the last time your driver is allowed to bail out.
//This is usually a good place to create variables like ups.mfr, ups.model, ups.serial, determine and declare supported instant commands (maybe model-dependent, typically for all devices supported by the driver), and other "one time only" items.
void upsdrv_initinfo(void) {
	dstate_setinfo("ups.delay.start", "%dmin", ondelay);
	dstate_setinfo("ups.delay.shutdown", "%ds", offdelay);

	char buf[SMALLBUF];

	if (ups_command("PSNR", buf, SMALLBUF) > 0) {
		dstate_setinfo("ups.serial", "%s", buf);
	}

	if (ups_command("Si ?", buf, SMALLBUF) > 0) {
		dstate_setinfo("ups.model", "%s", buf);
	}

	if (ups_command("BRR", buf, SMALLBUF) > 0) {
		dstate_setinfo("battery.date", "%s", buf);
	}

	dstate_addcmd("shutdown.return");
	dstate_addcmd("shutdown.stayoff");
	dstate_addcmd("shutdown.stop");
	dstate_addcmd("test.battery.start.quick");

	upsh.instcmd = omron_instcmd;
}

//upsdrv_updateinfo
//Poll the hardware, and update any variables that you care about monitoring. Use dstate_setinfo() to store the new values.
//Do at most one pass of the variables. You MUST return from this function or upsd will be unable to read data from your driver. main will call this function at regular intervals.
//Don’t spent more than a couple of seconds in this function. Typically five (5) seconds is the maximum time allowed before you risk that the server declares the driver stale. If your UPS hardware requires a timeout period of several seconds before it answers, consider returning from this function after sending a command immediately and read the answer the next time it is called.
//You must never abort from upsdrv_updateinfo(), even when the UPS doesn’t seem to be attached anymore. If the connection with the UPS is lost, the driver should retry to re-establish communication for as long as it is running. Calling exit() or any of the fatal*() functions is specifically not allowed anymore.
void upsdrv_updateinfo(void) {
	char buf[SMALLBUF];
	if (ups_command("Q1", buf, SMALLBUF) == 47) { // limit buflen to prevent USB timeout
		status_init();
		alarm_init();

		unsigned int uStatus = 0;
		float s[7];

		sscanf(buf, "(%f %f %f %f %f %f %f %8x", s, s+1, s+2, s+3, s+4, s+5, s+6, &uStatus);

		dstate_setinfo("input.voltage",       "%.1fv",  s[0]);
		dstate_setinfo("input.voltage.fault", "%.1fv",  s[1]);
		dstate_setinfo("output.voltage",      "%.1fv",  s[2]);
		dstate_setinfo("ups.load",            "%.0f%%", s[3]);
		dstate_setinfo("input.frequency",     "%.1fHz", s[4]);
		dstate_setinfo("battery.voltage",     "%.1fv",  s[5]);
		dstate_setinfo("ups.temperature",     "%.1fC",  s[6]);

		if (uStatus & 0x10000000) {
			status_set("OB");
		} else {
			status_set("OL");
		}

		if (uStatus & 0x01000000) {
			status_set("LB");
		}

		if (uStatus & 0x00100000) {
			dstate_setinfo("ups.inverter", "off");
		} else {
			dstate_setinfo("ups.inverter", "on");
		}

		if (uStatus & 0x00010000) {
			alarm_set("UPS selftest failed!");
		}

		if (uStatus & 0x00001000) {
			dstate_setinfo("ups.type", "offline / line interactive");
		} else {
			dstate_setinfo("ups.type", "online");
		}

		if (uStatus & 0x00000100) {
			status_set("CAL");
		}

		if (uStatus & 0x00000010) {
			alarm_set("Shutdown imminent!");
			status_set("FSD");
		}

		if (uStatus & 0x00000001) {
			alarm_set("Battery test failed!");
			status_set("RB");
		}

		if (ups_command("TBN ?", buf, SMALLBUF) > 0) {
			unsigned int backup_cnt;
			sscanf(buf, "%u", &backup_cnt);
			dstate_setinfo("ups.backup_cnt", "%u", backup_cnt);
		}

		status_commit();
		alarm_commit();
		dstate_dataok();
	} else {
		upslogx(LOG_WARNING, "Communications with UPS lost: status read failed!");
		dstate_datastale();
	}
}

//upsdrv_shutdown
//Do whatever you can to make the UPS power off the load but also return after the power comes back on. You may use a different command that keeps the UPS off if the user has requested that with a configuration setting.
//You should attempt the UPS shutdown command even if the UPS detection fails. If the UPS does not shut down the load, then the user is vulnerable to a race if the power comes back on during the shutdown process.
//This method should not directly exit() the driver program (neither should it call fatalx() nor fatal_with_errno() methods). It can upslogx(LOG_ERR, ...) or upslog_with_errno(LOG_ERR, ...), and then set_exit_flag(N) if required, using values EF_EXIT_FAILURE (-1) for eventual exit(EXIT_FAILURE) and EF_EXIT_SUCCESS (-2) for exit(EXIT_SUCCESS), which would be handled in the standard driver loop or in forceshutdown() method of main.c.
void upsdrv_shutdown(void)
	__attribute__((noreturn));

void upsdrv_shutdown(void) {
	int retry = 1;
	while(1) {
		if (retry) {
			int ret;
			if (stayoff) {
				ret = omron_instcmd("shutdown.stayoff", NULL);
			} else {
				ret = omron_instcmd("shutdown.return", NULL);
			}
			if (ret == STAT_INSTCMD_HANDLED) {
				retry = 0;
			}
		}
		usleep(100000);
	}
}

void upsdrv_cleanup(void) {
	CloseUsbPort();
	libusb_exit(NULL);
}

void upsdrv_tweak_prognames(void) {}
