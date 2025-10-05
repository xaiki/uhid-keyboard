// SPDX-License-Identifier: GPL-2.0
/*
 * UHID Example
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * The code may be used by anyone for any purpose,
 * and can serve as a starting point for developing
 * applications using uhid.
 */

/*
 * UHID Keyboard Example
 * This example emulates a basic keyboard device over UHID. Run this program as root
 * and then type any characters to send them as keyboard events:
 *   q: Quit the application
 *   Any other character: Send as keyboard input
 *
 * The program will:
 * - Convert ASCII characters to HID key codes
 * - Handle modifier keys (Shift for uppercase letters)
 * - Send key press and release events for each character
 * - Support letters, numbers, and common special characters
 *
 * If uhid is not available as /dev/uhid, then you can pass a different path as
 * first argument.
 * If <linux/uhid.h> is not installed in /usr, then compile this with:
 *   gcc -o ./uhid_test -Wall -I./include ./samples/uhid/uhid-example.c
 * And ignore the warning about kernel headers. However, it is recommended to
 * use the installed uhid.h if available.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <linux/uhid.h>

/*
 * HID Report Descriptor
 * We emulate a basic keyboard device. This is the report-descriptor as the kernel will parse it:
 *
 * INPUT(1)[INPUT]
 *   Field(0)
 *     Application(GenericDesktop.Keyboard)
 *     Usage(8)
 *       Keyboard.LeftControl
 *       Keyboard.LeftShift
 *       Keyboard.LeftAlt
 *       Keyboard.LeftGUI
 *       Keyboard.RightControl
 *       Keyboard.RightShift
 *       Keyboard.RightAlt
 *       Keyboard.RightGUI
 *     Logical Minimum(0)
 *     Logical Maximum(1)
 *     Report Size(1)
 *     Report Count(8)
 *     Report Offset(0)
 *     Flags( Variable Absolute )
 *   Field(1)
 *     Application(GenericDesktop.Keyboard)
 *     Usage(6)
 *       Keyboard.Reserved
 *     Logical Minimum(0)
 *     Logical Maximum(1)
 *     Report Size(1)
 *     Report Count(6)
 *     Report Offset(8)
 *     Flags( Variable Absolute )
 *   Field(2)
 *     Application(GenericDesktop.Keyboard)
 *     Usage(6)
 *       Keyboard.Keypad
 *     Logical Minimum(0)
 *     Logical Maximum(101)
 *     Report Size(8)
 *     Report Count(6)
 *     Report Offset(16)
 *     Flags( Variable Absolute )
 *
 * This is the mapping that we expect:
 *   Keyboard.LeftControl ---> Key.LeftControl
 *   Keyboard.LeftShift ---> Key.LeftShift
 *   Keyboard.LeftAlt ---> Key.LeftAlt
 *   Keyboard.LeftGUI ---> Key.LeftGUI
 *   Keyboard.RightControl ---> Key.RightControl
 *   Keyboard.RightShift ---> Key.RightShift
 *   Keyboard.RightAlt ---> Key.RightAlt
 *   Keyboard.RightGUI ---> Key.RightGUI
 *   Keyboard.Keypad ---> Key.Keypad
 *
 * This information can be verified by reading /sys/kernel/debug/hid/<dev>/rdesc
 * This file should print the same information as showed above.
 */

static unsigned char rdesc[] = {
	0x05, 0x01,	/* USAGE_PAGE (Generic Desktop) */
	0x09, 0x06,	/* USAGE (Keyboard) */
	0xa1, 0x01,	/* COLLECTION (Application) */
	0x05, 0x07,		/* USAGE_PAGE (Keyboard) */
	0x19, 0xe0,		/* USAGE_MINIMUM (Keyboard LeftControl) */
	0x29, 0xe7,		/* USAGE_MAXIMUM (Keyboard Right GUI) */
	0x15, 0x00,		/* LOGICAL_MINIMUM (0) */
	0x25, 0x01,		/* LOGICAL_MAXIMUM (1) */
	0x75, 0x01,		/* REPORT_SIZE (1) */
	0x95, 0x08,		/* REPORT_COUNT (8) */
	0x81, 0x02,		/* INPUT (Data,Var,Abs) */
	0x95, 0x01,		/* REPORT_COUNT (1) */
	0x75, 0x08,		/* REPORT_SIZE (8) */
	0x81, 0x01,		/* INPUT (Cnst,Var,Abs) */
	0x95, 0x06,		/* REPORT_COUNT (6) */
	0x75, 0x08,		/* REPORT_SIZE (8) */
	0x15, 0x00,		/* LOGICAL_MINIMUM (0) */
	0x25, 0x65,		/* LOGICAL_MAXIMUM (101) */
	0x05, 0x07,		/* USAGE_PAGE (Keyboard) */
	0x19, 0x00,		/* USAGE_MINIMUM (Reserved) */
	0x29, 0x65,		/* USAGE_MAXIMUM (Keyboard Application) */
	0x81, 0x00,		/* INPUT (Data,Array,Abs) */
	0xc0,		/* END_COLLECTION */
};

static int uhid_write(int fd, const struct uhid_event *ev)
{
	ssize_t ret;

	ret = write(fd, ev, sizeof(*ev));
	if (ret < 0) {
		fprintf(stderr, "Cannot write to uhid: %m\n");
		return -errno;
	} else if (ret != sizeof(*ev)) {
		fprintf(stderr, "Wrong size written to uhid: %zd != %zu\n",
			ret, sizeof(ev));
		return -EFAULT;
	} else {
		return 0;
	}
}

static int create(int fd)
{
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE;
	strcpy((char*)ev.u.create.name, "test-uhid-device");
	ev.u.create.rd_data = rdesc;
	ev.u.create.rd_size = sizeof(rdesc);
	ev.u.create.bus = BUS_USB;
	ev.u.create.vendor = 0x15d9;
	ev.u.create.product = 0x0a37;
	ev.u.create.version = 0;
	ev.u.create.country = 0;

	return uhid_write(fd, &ev);
}

static void destroy(int fd)
{
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_DESTROY;

	uhid_write(fd, &ev);
}

/* This parses raw output reports sent by the kernel to the device. A normal
 * uhid program shouldn't do this but instead just forward the raw report.
 * However, for ducomentational purposes, we try to detect LED events here and
 * print debug messages for it. */
static void handle_output(struct uhid_event *ev)
{
	/* LED messages are adverised via OUTPUT reports; ignore the rest */
	if (ev->u.output.rtype != UHID_OUTPUT_REPORT)
		return;
	/* LED reports have length 2 bytes */
	if (ev->u.output.size != 2)
		return;
	/* first byte is report-id which is 0x02 for LEDs in our rdesc */
	if (ev->u.output.data[0] != 0x2)
		return;

	/* print flags payload */
	fprintf(stderr, "LED output report received with flags %x\n",
		ev->u.output.data[1]);
}

static int event(int fd)
{
	struct uhid_event ev;
	ssize_t ret;

	memset(&ev, 0, sizeof(ev));
	ret = read(fd, &ev, sizeof(ev));
	if (ret == 0) {
		fprintf(stderr, "Read HUP on uhid-cdev\n");
		return -EFAULT;
	} else if (ret < 0) {
		fprintf(stderr, "Cannot read uhid-cdev: %m\n");
		return -errno;
	} else if (ret != sizeof(ev)) {
		fprintf(stderr, "Invalid size read from uhid-dev: %zd != %zu\n",
			ret, sizeof(ev));
		return -EFAULT;
	}

	switch (ev.type) {
	case UHID_START:
		fprintf(stderr, "UHID_START from uhid-dev\n");
		break;
	case UHID_STOP:
		fprintf(stderr, "UHID_STOP from uhid-dev\n");
		break;
	case UHID_OPEN:
		fprintf(stderr, "UHID_OPEN from uhid-dev\n");
		break;
	case UHID_CLOSE:
		fprintf(stderr, "UHID_CLOSE from uhid-dev\n");
		break;
	case UHID_OUTPUT:
		fprintf(stderr, "UHID_OUTPUT from uhid-dev\n");
		handle_output(&ev);
		break;
	case UHID_OUTPUT_EV:
		fprintf(stderr, "UHID_OUTPUT_EV from uhid-dev\n");
		break;
	default:
		fprintf(stderr, "Invalid event from uhid-dev: %u\n", ev.type);
	}

	return 0;
}

/* Keyboard state tracking */
static unsigned char modifier_keys = 0;  /* Bitfield for modifier keys */
static unsigned char key_codes[6] = {0};  /* Array for up to 6 simultaneous key presses */
static int num_keys_pressed = 0;          /* Number of keys currently pressed */

/* Escape sequence buffer for arrow keys */
static char escape_buf[8];
static int escape_len = 0;

static int send_event(int fd)
{
	struct uhid_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_INPUT;
	ev.u.input.size = 8;  /* 1 byte modifiers + 1 byte reserved + 6 bytes key codes */

	ev.u.input.data[0] = modifier_keys;  /* Modifier keys bitfield */
	ev.u.input.data[1] = 0;              /* Reserved byte */
	
	/* Ensure no stale keycodes remain in the report on key-up */
	memset(&ev.u.input.data[2], 0, 6);
	if (num_keys_pressed > 0) {
		/* Copy only currently pressed keys */
		int copy_len = num_keys_pressed > 6 ? 6 : num_keys_pressed;
		memcpy(&ev.u.input.data[2], key_codes, copy_len);
	}

	/* Debug output for all HID reports */
	if (num_keys_pressed > 0) {
		fprintf(stderr, "HID Report: modifiers=0x%02x, keys=[", modifier_keys);
		for (int i = 0; i < 6; i++) {
			if (key_codes[i] != 0) {
				fprintf(stderr, "0x%02x ", key_codes[i]);
			}
		}
		fprintf(stderr, "]\n");
	} else {
		fprintf(stderr, "HID Report: modifiers=0x%02x, keys=[ ] (no keys pressed)\n", modifier_keys);
	}

	return uhid_write(fd, &ev);
}

/* Map ASCII character to HID key code */
static unsigned char ascii_to_hid(char c)
{
	if (c >= 'a' && c <= 'z')
		return 0x04 + (c - 'a');  /* HID_A = 0x04 */
	if (c >= 'A' && c <= 'Z')
		return 0x04 + (c - 'A');  /* Same as lowercase */
	if (c >= '1' && c <= '9')
		return 0x1e + (c - '1');  /* HID_1 = 0x1e */
	if (c == '0')
		return 0x27;  /* HID_0 = 0x27 */
	if (c == ' ')
		return 0x2c;  /* HID_SPACE = 0x2c */
	if (c == '\n' || c == '\r')
		return 0x28;  /* HID_ENTER = 0x28 */
	if (c == '\b')
		return 0x2a;  /* HID_BACKSPACE = 0x2a */
	if (c == '\t')
		return 0x2b;  /* HID_TAB = 0x2b */
	if (c == 27)  /* ESC */
		return 0x29;  /* HID_ESCAPE = 0x29 */
	
	/* Special characters */
	switch (c) {
	case '!': return 0x1e;  /* 1 */
	case '@': return 0x1f;  /* 2 */
	case '#': return 0x20;  /* 3 */
	case '$': return 0x21;  /* 4 */
	case '%': return 0x22;  /* 5 */
	case '^': return 0x23;  /* 6 */
	case '&': return 0x24;  /* 7 */
	case '*': return 0x25;  /* 8 */
	case '(': return 0x26;  /* 9 */
	case ')': return 0x27;  /* 0 */
	case '-': return 0x2d;  /* HID_MINUS */
	case '=': return 0x2e;  /* HID_EQUAL */
	case '[': return 0x2f;  /* HID_LEFTBRACE */
	case ']': return 0x30;  /* HID_RIGHTBRACE */
	case '\\': return 0x31; /* HID_BACKSLASH */
	case ';': return 0x33;  /* HID_SEMICOLON */
	case '\'': return 0x34; /* HID_APOSTROPHE */
	case '`': return 0x35;  /* HID_GRAVE */
	case ',': return 0x36;  /* HID_COMMA */
	case '.': return 0x37;  /* HID_DOT */
	case '/': return 0x38;  /* HID_SLASH */
	default: return 0;  /* Unknown character */
	}
}

/* Process escape sequence buffer and return HID code if complete */
static unsigned char process_escape_sequence(void)
{
	if (escape_len >= 3 && escape_buf[0] == 27 && escape_buf[1] == '[') {
		unsigned char hid_code = 0;
		switch (escape_buf[2]) {
		case 'A': hid_code = 0x52; break;  /* HID_UP_ARROW */
		case 'B': hid_code = 0x51; break;  /* HID_DOWN_ARROW */
		case 'C': hid_code = 0x4f; break;  /* HID_RIGHT_ARROW */
		case 'D': hid_code = 0x50; break;  /* HID_LEFT_ARROW */
		}
		if (hid_code != 0) {
			/* Clear the escape buffer */
			escape_len = 0;
			return hid_code;
		}
	}
	return 0;  /* Not a complete arrow key sequence */
}

/* Add character to escape sequence buffer */
static void add_to_escape_buf(char c)
{
	if (escape_len < sizeof(escape_buf) - 1) {
		escape_buf[escape_len++] = c;
	} else {
		/* Buffer overflow, reset */
		escape_len = 0;
	}
}

/* Add a key to the pressed keys array */
static void add_key(unsigned char hid_code)
{
	if (num_keys_pressed >= 6) return;  /* Maximum 6 keys */
	
	for (int i = 0; i < num_keys_pressed; i++) {
		if (key_codes[i] == hid_code) return;  /* Already pressed */
	}
	
	key_codes[num_keys_pressed++] = hid_code;
}

/* Remove a key from the pressed keys array */
static void remove_key(unsigned char hid_code)
{
	for (int i = 0; i < num_keys_pressed; i++) {
		if (key_codes[i] == hid_code) {
			/* Shift remaining keys left */
			for (int j = i; j < num_keys_pressed - 1; j++) {
				key_codes[j] = key_codes[j + 1];
			}
			/* Clear the now-unused last slot to avoid stale values */
			num_keys_pressed--;
			key_codes[num_keys_pressed] = 0;
			break;
		}
	}
}

/* Clear all pressed keys */
static void clear_keys(void)
{
	memset(key_codes, 0, sizeof(key_codes));
	num_keys_pressed = 0;
	modifier_keys = 0;
}

static int keyboard(int fd)
{
	char buf[128];
	ssize_t ret, i;

	ret = read(STDIN_FILENO, buf, sizeof(buf));
	if (ret == 0) {
		fprintf(stderr, "Read HUP on stdin\n");
		return -EFAULT;
	} else if (ret < 0) {
		fprintf(stderr, "Cannot read stdin: %m\n");
		return -errno;
	}

	for (i = 0; i < ret; ++i) {
		unsigned char hid_code = 0;
		const char *key_name = "UNKNOWN";
		
		/* Check if we're in the middle of an escape sequence */
		if (escape_len > 0) {
			/* We're building an escape sequence */
			add_to_escape_buf(buf[i]);
			hid_code = process_escape_sequence();
			
			if (hid_code != 0) {
				/* We have a complete arrow key sequence */
				switch (escape_buf[2]) {
				case 'A': key_name = "UP_ARROW"; break;
				case 'B': key_name = "DOWN_ARROW"; break;
				case 'C': key_name = "RIGHT_ARROW"; break;
				case 'D': key_name = "LEFT_ARROW"; break;
				}
			} else {
				/* Still building escape sequence, continue */
				continue;
			}
		} else if (buf[i] == 27) {
			/* Check if this is a standalone ESC or start of arrow sequence */
			if (i + 1 < ret && buf[i+1] == '[') {
				/* This is the start of an arrow key sequence */
				add_to_escape_buf(buf[i]);
				/* Don't process yet, wait for the full sequence */
				continue;
			} else {
				/* This is a standalone ESC key */
				hid_code = ascii_to_hid(buf[i]);
				key_name = "ESC";
			}
		} else {
			/* Regular character */
			hid_code = ascii_to_hid(buf[i]);
			if (buf[i] == ' ') key_name = "SPACE";
			else if (buf[i] == '\n' || buf[i] == '\r') key_name = "ENTER";
			else if (buf[i] == 27) key_name = "ESC";
			else if (buf[i] >= 'a' && buf[i] <= 'z') key_name = "LETTER";
			else if (buf[i] >= 'A' && buf[i] <= 'Z') key_name = "LETTER";
			else if (buf[i] >= '0' && buf[i] <= '9') key_name = "NUMBER";
		}
		
		if (hid_code == 0) {
			fprintf(stderr, "Unknown character: %c (0x%02x)\n", buf[i], (unsigned char)buf[i]);
			continue;
		}
		
		/* Debug output for all keys */
		fprintf(stderr, "Processing character: %c (0x%02x) -> %s (HID code: 0x%02x)\n", 
			buf[i], (unsigned char)buf[i], key_name, hid_code);
		
		/* Check for shift requirement (exclude non-printable like arrows) */
		if (buf[i] >= 'A' && buf[i] <= 'Z' &&
			!(hid_code == 0x52 || hid_code == 0x51 || hid_code == 0x4f || hid_code == 0x50)) {
			modifier_keys |= 0x02;  /* Left Shift */
		}
		
		/* Press the key */
		add_key(hid_code);
		send_event(fd);
		
		/* Release the key */
		remove_key(hid_code);
		if (buf[i] >= 'A' && buf[i] <= 'Z' &&
			!(hid_code == 0x52 || hid_code == 0x51 || hid_code == 0x4f || hid_code == 0x50)) {
			modifier_keys &= ~0x02;  /* Release Left Shift */
		}
		send_event(fd);
	}

	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	const char *path = "/dev/uhid";
	struct pollfd pfds[2];
	int ret;
	struct termios state;

	ret = tcgetattr(STDIN_FILENO, &state);
	if (ret) {
		fprintf(stderr, "Cannot get tty state\n");
	} else {
		state.c_lflag &= ~ICANON;
		state.c_cc[VMIN] = 1;
		ret = tcsetattr(STDIN_FILENO, TCSANOW, &state);
		if (ret)
			fprintf(stderr, "Cannot set tty state\n");
	}

	if (argc >= 2) {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			fprintf(stderr, "Usage: %s [%s]\n", argv[0], path);
			return EXIT_SUCCESS;
		} else {
			path = argv[1];
		}
	}

	fprintf(stderr, "Open uhid-cdev %s\n", path);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open uhid-cdev %s: %m\n", path);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "Create uhid device\n");
	ret = create(fd);
	if (ret) {
		close(fd);
		return EXIT_FAILURE;
	}

	pfds[0].fd = STDIN_FILENO;
	pfds[0].events = POLLIN;
	pfds[1].fd = fd;
	pfds[1].events = POLLIN;

	fprintf(stderr, "Keyboard UHID device created. Type any characters to send as keyboard input.\n");
	while (1) {
		ret = poll(pfds, 2, -1);
		if (ret < 0) {
			fprintf(stderr, "Cannot poll for fds: %m\n");
			break;
		}
		if (pfds[0].revents & POLLHUP) {
			fprintf(stderr, "Received HUP on stdin\n");
			break;
		}
		if (pfds[1].revents & POLLHUP) {
			fprintf(stderr, "Received HUP on uhid-cdev\n");
			break;
		}

		if (pfds[0].revents & POLLIN) {
			ret = keyboard(fd);
			if (ret)
				break;
		}
		if (pfds[1].revents & POLLIN) {
			ret = event(fd);
			if (ret)
				break;
		}
	}

	fprintf(stderr, "Destroy uhid device\n");
	destroy(fd);
	return EXIT_SUCCESS;
}
