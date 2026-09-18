#ifndef FTPVITA_PARSE_H
#define FTPVITA_PARSE_H

#include <stddef.h>

/* Keep FTP control parsing independent of libc scanf.  The Vita PDCLib scanf
 * implementation used by shell plugins faults on the REST suppression format
 * ("%*[^ ] %d"), so command input must be parsed with explicit bounds. */
static inline int ftpvita_parse_command_line(const char *line, char *command,

		size_t command_size, const char **arguments)
{
	const char *cursor = line;
	size_t length = 0;

	if (!line || !command || command_size < 2 || !arguments)
		return 0;

	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	while (cursor[length] && cursor[length] != ' ' &&
		cursor[length] != '\t' && cursor[length] != '\r' &&
		cursor[length] != '\n') {
		if (length + 1 >= command_size)
			return 0;
		command[length] = cursor[length];
		length++;
	}
	if (length == 0)
		return 0;
	command[length] = '\0';
	cursor += length;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	*arguments = cursor;
	return 1;
}

static inline int ftpvita_copy_command_argument(const char *arguments,

		char *output, size_t output_size)
{
	size_t length = 0;

	if (!arguments || !output || output_size == 0)
		return 0;
	while (arguments[length] && arguments[length] != '\r' &&
		arguments[length] != '\n' && arguments[length] != '\t') {
		if (length + 1 >= output_size) {
			output[0] = '\0';
			return 0;
		}
		output[length] = arguments[length];
		length++;
	}
	if (length == 0) {
		output[0] = '\0';
		return 0;
	}
	output[length] = '\0';
	return 1;
}

static inline int ftpvita_parse_rest_offset(const char *arguments,

		unsigned int *offset)
{
	const unsigned int maximum = 0x7fffffffu;
	unsigned int value = 0;
	const char *cursor = arguments;

	if (!cursor || !offset)
		return 0;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor < '0' || *cursor > '9')
		return 0;
	while (*cursor >= '0' && *cursor <= '9') {
		const unsigned int digit = (unsigned int)(*cursor - '0');
		if (value > (maximum - digit) / 10u)
			return 0;
		value = value * 10u + digit;
		cursor++;
	}
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor != '\0' && *cursor != '\r' && *cursor != '\n')
		return 0;
	*offset = value;
	return 1;
}

static inline int ftpvita_parse_port_tuple(const char *arguments,

		unsigned int values[6])
{
	const char *cursor = arguments;

	if (!cursor || !values)
		return 0;
	for (unsigned int index = 0; index < 6; index++) {
		unsigned int value = 0;
		unsigned int digits = 0;
		while (*cursor >= '0' && *cursor <= '9') {
			value = value * 10u + (unsigned int)(*cursor - '0');
			if (value > 255u)
				return 0;
			digits++;
			cursor++;
		}
		if (digits == 0)
			return 0;
		values[index] = value;
		if (index != 5) {
			if (*cursor != ',')
				return 0;
			cursor++;
		}
	}
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	return *cursor == '\0' || *cursor == '\r' || *cursor == '\n';
}

#endif
