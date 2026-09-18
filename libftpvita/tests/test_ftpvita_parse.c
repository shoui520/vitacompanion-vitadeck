#include "../ftpvita_parse.h"

#include <assert.h>
#include <string.h>

int main(void)
{
	char command[16];
	char argument[32];
	const char *arguments = NULL;
	unsigned int offset = 0;
	unsigned int tuple[6] = {0};

	assert(ftpvita_parse_command_line("REST 1091\r\n", command,
		sizeof(command), &arguments));
	assert(strcmp(command, "REST") == 0);
	assert(strcmp(arguments, "1091\r\n") == 0);
	assert(ftpvita_parse_rest_offset(arguments, &offset));
	assert(offset == 1091u);

	assert(ftpvita_parse_rest_offset("0\r\n", &offset));
	assert(offset == 0u);
	assert(ftpvita_parse_rest_offset("2147483647\r\n", &offset));
	assert(offset == 0x7fffffffu);
	assert(!ftpvita_parse_rest_offset("2147483648\r\n", &offset));
	assert(!ftpvita_parse_rest_offset("-1\r\n", &offset));
	assert(!ftpvita_parse_rest_offset("12garbage\r\n", &offset));
	assert(!ftpvita_parse_rest_offset("\r\n", &offset));

	assert(ftpvita_parse_port_tuple("192,168,0,155,5,57\r\n", tuple));
	assert(tuple[0] == 192u && tuple[3] == 155u);
	assert(tuple[4] == 5u && tuple[5] == 57u);
	assert(!ftpvita_parse_port_tuple("256,168,0,155,5,57\r\n", tuple));
	assert(!ftpvita_parse_port_tuple("192,168,0,155,5\r\n", tuple));
	assert(!ftpvita_parse_port_tuple("192,168,0,155,5,57x\r\n", tuple));

	assert(ftpvita_copy_command_argument("ux0:/data/file name\r\n",
		argument, sizeof(argument)));
	assert(strcmp(argument, "ux0:/data/file name") == 0);
	assert(!ftpvita_copy_command_argument("\r\n", argument,
		sizeof(argument)));
	assert(!ftpvita_parse_command_line(
		"THIS_COMMAND_IS_TOO_LONG value\r\n", command,
		sizeof(command), &arguments));

	return 0;
}
