/*
 * koriki_rumble.h - Miyoo Mini vibration motor, same as Koriki's RetroArch
 * (input/drivers_joypad/sdl_miyoomini_joypad.c): GPIO 48, "0" = on, "1" = off.
 * Only writes when the state changes.
 */
#ifndef KORIKI_RUMBLE_H
#define KORIKI_RUMBLE_H

#include <fcntl.h>
#include <unistd.h>

static inline void koriki_rumble(int on)
{
	static char last = 0;
	char value = on ? '0' : '1';
	int fd;

	if (value == last)
		return;
	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd >= 0) { if (write(fd, "48", 2) < 0) {} close(fd); }
	fd = open("/sys/class/gpio/gpio48/direction", O_WRONLY);
	if (fd >= 0) { if (write(fd, "out", 3) < 0) {} close(fd); }
	fd = open("/sys/class/gpio/gpio48/value", O_WRONLY);
	if (fd >= 0) { if (write(fd, &value, 1) < 0) {} close(fd); }
	last = value;
}

#endif /* KORIKI_RUMBLE_H */
