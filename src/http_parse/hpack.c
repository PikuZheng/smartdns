#include "hpack.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HPACK static table (RFC 7541 Appendix A) */
struct hpack_static_entry {
	const char *name;
	const char *value;
};

/* clang-format off */
static const struct hpack_static_entry hpack_static_table[] = {
	{":authority", ""},
	{":method", "GET"},
	{":method", "POST"},
	{":path", "/"},
	{":path", "/index.html"},
	{":scheme", "http"},
	{":scheme", "https"},
	{":status", "200"},
	{":status", "204"},
	{":status", "206"},
	{":status", "304"},
	{":status", "400"},
	{":status", "404"},
	{":status", "500"},
	{"accept-charset", ""},
	{"accept-encoding", "gzip, deflate"},
	{"accept-language", ""},
	{"accept-ranges", ""},
	{"accept", ""},
	{"access-control-allow-origin", ""},
	{"age", ""},
	{"allow", ""},
	{"authorization", ""},
	{"cache-control", ""},
	{"content-disposition", ""},
	{"content-encoding", ""},
	{"content-language", ""},
	{"content-length", ""},
	{"content-location", ""},
	{"content-range", ""},
	{"content-type", ""},
	{"cookie", ""},
	{"date", ""},
	{"etag", ""},
	{"expect", ""},
	{"expires", ""},
	{"from", ""},
	{"host", ""},
	{"if-match", ""},
	{"if-modified-since", ""},
	{"if-none-match", ""},
	{"if-range", ""},
	{"if-unmodified-since", ""},
	{"last-modified", ""},
	{"link", ""},
	{"location", ""},
	{"max-forwards", ""},
	{"proxy-authenticate", ""},
	{"proxy-authorization", ""},
	{"range", ""},
	{"referer", ""},
	{"refresh", ""},
	{"retry-after", ""},
	{"server", ""},
	{"set-cookie", ""},
	{"strict-transport-security", ""},
	{"transfer-encoding", ""},
	{"user-agent", ""},
	{"vary", ""},
	{"via", ""},
	{"www-authenticate", ""}
};
/* clang-format on */

#define HPACK_STATIC_TABLE_SIZE (sizeof(hpack_static_table) / sizeof(hpack_static_table[0]))

/* HPACK integer encoding/decoding */

static int hpack_encode_integer(uint64_t value, int prefix_bits, uint8_t *buf, size_t buf_size)
{
	unsigned int max_prefix;
	int offset = 0;

	/* prefix_bits must be in range [1, 8] */
	if (prefix_bits < 1 || prefix_bits > 8) {
		return -1;
	}

	max_prefix = (1U << prefix_bits) - 1;

	if (value < max_prefix) {
		if (buf_size < 1) {
			return -1;
		}
		/* Only modify the lower prefix_bits of buf[0]; high bits are
		 * preserved as provided by the caller. */
		buf[0] = (buf[0] & ~((uint8_t)max_prefix)) | (uint8_t)value;
		return 1;
	}

	if (buf_size < 1) {
		return -1;
	}
	buf[0] = (buf[0] & ~((uint8_t)max_prefix)) | (uint8_t)max_prefix;
	offset = 1;
	value -= max_prefix;

	while (value >= 128) {
		if ((size_t)offset >= buf_size) {
			return -1;
		}
		buf[offset++] = (uint8_t)((value & 0x7F) | 0x80);
		value >>= 7;
	}

	if ((size_t)offset >= buf_size) {
		return -1;
	}
	buf[offset++] = (uint8_t)value;
	return offset;
}

static int hpack_decode_integer(const uint8_t *data, int data_len, int prefix_bits, uint64_t *value)
{
	unsigned int max_prefix;
	int offset = 0;
	uint64_t result;
	int shift = 0;

	if (prefix_bits < 1 || prefix_bits > 8) {
		return -1;
	}

	max_prefix = (1U << prefix_bits) - 1;

	if (data_len < 1) {
		return -1;
	}

	result = data[offset++] & max_prefix;
	if (result < max_prefix) {
		*value = result;
		return offset;
	}

	while (offset < data_len) {
		uint8_t byte = data[offset++];
		uint64_t incr = (uint64_t)(byte & 0x7F);
		/* Check for uint64_t overflow before shifting / adding */
		if (shift > 0 && incr > (UINT64_MAX >> shift)) {
			return -1;
		}
		if (result > UINT64_MAX - (incr << shift)) {
			return -1;
		}
		result += incr << shift;
		shift += 7;
		if ((byte & 0x80) == 0) {
			*value = result;
			return offset;
		}
		if (shift > 63) {
			return -1;
		}
	}

	return -1;
}

/* HPACK string encoding/decoding */

static int hpack_encode_string(const char *str, uint8_t *buf, size_t buf_size)
{
	size_t len = strlen(str);
	size_t offset = 0;
	int ret;

	if (buf_size < 1) {
		return -1;
	}

	buf[offset] = 0; /* No Huffman encoding */
	ret = hpack_encode_integer(len, 7, buf + offset, buf_size - offset);
	if (ret < 0) {
		return -1;
	}
	offset += (size_t)ret;

	if (offset + len > buf_size) {
		return -1;
	}

	memcpy(buf + offset, str, len);
	offset += len;

	return (int)offset;
}

/* HPACK Huffman decoding table based on RFC 7541 Appendix B */
/* Each entry contains: symbol, code length in bits */
struct huffman_decode_entry {
	uint32_t bits;  /* Huffman code bits */
	uint8_t nbits;  /* Number of bits in code */
	uint8_t symbol; /* Decoded symbol */
};

/* Complete Huffman decoding table for HPACK (RFC 7541 Appendix B) */
/* Codes are right-aligned (LSB-aligned) as specified in RFC 7541 */
/* Sorted by symbol index for binary search within each length group */
static const struct huffman_decode_entry huffman_table[] = {
	/*   0 -  31: control characters (0x00 - 0x1f) */
	{0x00001ff8, 13, 0x00}, {0x007fffd8, 23, 0x01}, {0x0fffffe2, 28, 0x02},
	{0x0fffffe3, 28, 0x03}, {0x0fffffe4, 28, 0x04}, {0x0fffffe5, 28, 0x05},
	{0x0fffffe6, 28, 0x06}, {0x0fffffe7, 28, 0x07}, {0x0fffffe8, 28, 0x08},
	{0x000ffffea, 24, 0x09}, {0x3ffffffc, 30, 0x0a}, {0x0fffffe9, 28, 0x0b},
	{0x0fffffea, 28, 0x0c}, {0x3ffffffd, 30, 0x0d}, {0x0fffffeb, 28, 0x0e},
	{0x0fffffec, 28, 0x0f}, {0x0fffffed, 28, 0x10}, {0x0fffffee, 28, 0x11},
	{0x0fffffef, 28, 0x12}, {0x0ffffff0, 28, 0x13}, {0x0ffffff1, 28, 0x14},
	{0x0ffffff2, 28, 0x15}, {0x3ffffffe, 30, 0x16}, {0x0ffffff3, 28, 0x17},
	{0x0ffffff4, 28, 0x18}, {0x0ffffff5, 28, 0x19}, {0x0ffffff6, 28, 0x1a},
	{0x0ffffff7, 28, 0x1b}, {0x0ffffff8, 28, 0x1c}, {0x0ffffff9, 28, 0x1d},
	{0x0ffffffa, 28, 0x1e}, {0x0ffffffb, 28, 0x1f},
	/*  32 -  63: printable punctuation and digits */
	{0x00000014,  6, ' '},  {0x000003f8, 10, '!'},  {0x000003f9, 10, '"'},
	{0x00000ffa, 12, '#'},  {0x00001ff9, 13, '$'},  {0x00000015,  6, '%'},
	{0x000000f8,  8, '&'},  {0x000007fa, 11, '\''}, {0x000003fa, 10, '('},
	{0x000003fb, 10, ')'},  {0x000000f9,  8, '*'},  {0x000007fb, 11, '+'},
	{0x000000fa,  8, ','},  {0x00000016,  6, '-'},  {0x00000017,  6, '.'},
	{0x00000018,  6, '/'},  {0x00000000,  5, '0'},  {0x00000001,  5, '1'},
	{0x00000002,  5, '2'},  {0x00000019,  6, '3'},  {0x0000001a,  6, '4'},
	{0x0000001b,  6, '5'},  {0x0000001c,  6, '6'},  {0x0000001d,  6, '7'},
	{0x0000001e,  6, '8'},  {0x0000001f,  6, '9'},  {0x0000005c,  7, ':'},
	{0x000000fb,  8, ';'},  {0x00007ffc, 15, '<'},  {0x00000020,  6, '='},
	{0x00000ffb, 12, '>'},  {0x000003fc, 10, '?'},
	/*  64 -  95: uppercase letters and more */
	{0x00001ffa, 13, '@'},  {0x00000021,  6, 'A'},  {0x0000005d,  7, 'B'},
	{0x0000005e,  7, 'C'},  {0x0000005f,  7, 'D'},  {0x00000060,  7, 'E'},
	{0x00000061,  7, 'F'},  {0x00000062,  7, 'G'},  {0x00000063,  7, 'H'},
	{0x00000064,  7, 'I'},  {0x00000065,  7, 'J'},  {0x00000066,  7, 'K'},
	{0x00000067,  7, 'L'},  {0x00000068,  7, 'M'},  {0x00000069,  7, 'N'},
	{0x0000006a,  7, 'O'},  {0x0000006b,  7, 'P'},  {0x0000006c,  7, 'Q'},
	{0x0000006d,  7, 'R'},  {0x0000006e,  7, 'S'},  {0x0000006f,  7, 'T'},
	{0x00000070,  7, 'U'},  {0x00000071,  7, 'V'},  {0x00000072,  7, 'W'},
	{0x000000fc,  8, 'X'},  {0x00000073,  7, 'Y'},  {0x000000fd,  8, 'Z'},
	{0x00001ffb, 13, '['},  {0x0007fff0, 19, '\\'}, {0x00001ffc, 13, ']'},
	{0x00003ffc, 14, '^'},  {0x00000022,  6, '_'},
	/*  96 - 127: lowercase letters, backtick, braces, pipe, tilde, DEL */
	{0x00007ffd, 15, '`'},  {0x00000003,  5, 'a'},  {0x00000023,  6, 'b'},
	{0x00000004,  5, 'c'},  {0x00000024,  6, 'd'},  {0x00000005,  5, 'e'},
	{0x00000025,  6, 'f'},  {0x00000026,  6, 'g'},  {0x00000027,  6, 'h'},
	{0x00000006,  5, 'i'},  {0x00000074,  7, 'j'},  {0x00000075,  7, 'k'},
	{0x00000028,  6, 'l'},  {0x00000029,  6, 'm'},  {0x0000002a,  6, 'n'},
	{0x00000007,  5, 'o'},  {0x0000002b,  6, 'p'},  {0x00000076,  7, 'q'},
	{0x0000002c,  6, 'r'},  {0x00000008,  5, 's'},  {0x00000009,  5, 't'},
	{0x0000002d,  6, 'u'},  {0x00000077,  7, 'v'},  {0x00000078,  7, 'w'},
	{0x00000079,  7, 'x'},  {0x0000007a,  7, 'y'},  {0x0000007b,  7, 'z'},
	{0x00007ffe, 15, '{'},  {0x000007fc, 11, '|'},  {0x00003ffd, 14, '}'},
	{0x00001ffd, 13, '~'},  {0x0ffffffc, 28, 0x7f},
	/* 128 - 159: extended ASCII */
	{0x000fffe6, 20, 0x80}, {0x03fffd2, 22, 0x81}, {0x000fffe7, 20, 0x82},
	{0x000fffe8, 20, 0x83}, {0x03fffd3, 22, 0x84}, {0x03fffd4, 22, 0x85},
	{0x03fffd5, 22, 0x86}, {0x07fffd9, 23, 0x87}, {0x03fffd6, 22, 0x88},
	{0x07fffda, 23, 0x89}, {0x07fffdb, 23, 0x8a}, {0x07fffdc, 23, 0x8b},
	{0x07fffdd, 23, 0x8c}, {0x07fffde, 23, 0x8d}, {0x0ffffeb, 24, 0x8e},
	{0x07fffdf, 23, 0x8f}, {0x0ffffec, 24, 0x90}, {0x0ffffed, 24, 0x91},
	{0x03fffd7, 22, 0x92}, {0x07fffe0, 23, 0x93}, {0x0ffffee, 24, 0x94},
	{0x07fffe1, 23, 0x95}, {0x07fffe2, 23, 0x96}, {0x07fffe3, 23, 0x97},
	{0x07fffe4, 23, 0x98}, {0x01fffdc, 21, 0x99}, {0x03fffd8, 22, 0x9a},
	{0x07fffe5, 23, 0x9b}, {0x03fffd9, 22, 0x9c}, {0x07fffe6, 23, 0x9d},
	{0x07fffe7, 23, 0x9e}, {0x0ffffef, 24, 0x9f},
	/* 160 - 191 */
	{0x03fffda, 22, 0xa0}, {0x01fffdd, 21, 0xa1}, {0x000fffe9, 20, 0xa2},
	{0x03fffdb, 22, 0xa3}, {0x03fffdc, 22, 0xa4}, {0x07fffe8, 23, 0xa5},
	{0x07fffe9, 23, 0xa6}, {0x01fffde, 21, 0xa7}, {0x07fffea, 23, 0xa8},
	{0x03fffdd, 22, 0xa9}, {0x03fffde, 22, 0xaa}, {0x0fffff0, 24, 0xab},
	{0x01fffdf, 21, 0xac}, {0x03fffdf, 22, 0xad}, {0x07fffeb, 23, 0xae},
	{0x07fffec, 23, 0xaf}, {0x01fffe0, 21, 0xb0}, {0x01fffe1, 21, 0xb1},
	{0x03fffe0, 22, 0xb2}, {0x01fffe2, 21, 0xb3}, {0x07fffed, 23, 0xb4},
	{0x03fffe1, 22, 0xb5}, {0x07fffee, 23, 0xb6}, {0x07fffef, 23, 0xb7},
	{0x000fffea, 20, 0xb8}, {0x03fffe2, 22, 0xb9}, {0x03fffe3, 22, 0xba},
	{0x03fffe4, 22, 0xbb}, {0x07ffff0, 23, 0xbc}, {0x03fffe5, 22, 0xbd},
	{0x03fffe6, 22, 0xbe}, {0x07ffff1, 23, 0xbf},
	/* 192 - 223 */
	{0x03ffffe0, 26, 0xc0}, {0x03ffffe1, 26, 0xc1}, {0x000fffeb, 20, 0xc2},
	{0x0007fff1, 19, 0xc3}, {0x03fffe7, 22, 0xc4}, {0x07ffff2, 23, 0xc5},
	{0x03fffe8, 22, 0xc6}, {0x01ffffec, 25, 0xc7}, {0x03ffffe2, 26, 0xc8},
	{0x03ffffe3, 26, 0xc9}, {0x03ffffe4, 26, 0xca}, {0x07ffffde, 27, 0xcb},
	{0x07ffffdf, 27, 0xcc}, {0x03ffffe5, 26, 0xcd}, {0x0fffff1, 24, 0xce},
	{0x01ffffed, 25, 0xcf}, {0x0007fff2, 19, 0xd0}, {0x01fffe3, 21, 0xd1},
	{0x03ffffe6, 26, 0xd2}, {0x07ffffe0, 27, 0xd3}, {0x07ffffe1, 27, 0xd4},
	{0x03ffffe7, 26, 0xd5}, {0x07ffffe2, 27, 0xd6}, {0x0fffff2, 24, 0xd7},
	{0x01fffe4, 21, 0xd8}, {0x01fffe5, 21, 0xd9}, {0x03ffffe8, 26, 0xda},
	{0x03ffffe9, 26, 0xdb}, {0x0ffffffd, 28, 0xdc}, {0x07ffffe3, 27, 0xdd},
	{0x07ffffe4, 27, 0xde}, {0x07ffffe5, 27, 0xdf},
	/* 224 - 255 */
	{0x000fffec, 20, 0xe0}, {0x0fffff3, 24, 0xe1}, {0x000fffed, 20, 0xe2},
	{0x01fffe6, 21, 0xe3}, {0x03fffe9, 22, 0xe4}, {0x01fffe7, 21, 0xe5},
	{0x01fffe8, 21, 0xe6}, {0x07ffff3, 23, 0xe7}, {0x03fffea, 22, 0xe8},
	{0x03fffeb, 22, 0xe9}, {0x01ffffee, 25, 0xea}, {0x01ffffef, 25, 0xeb},
	{0x0fffff4, 24, 0xec}, {0x0fffff5, 24, 0xed}, {0x03ffffea, 26, 0xee},
	{0x07ffff4, 23, 0xef}, {0x03ffffeb, 26, 0xf0}, {0x07ffffe6, 27, 0xf1},
	{0x03ffffec, 26, 0xf2}, {0x03ffffed, 26, 0xf3}, {0x07ffffe7, 27, 0xf4},
	{0x07ffffe8, 27, 0xf5}, {0x07ffffe9, 27, 0xf6}, {0x07ffffea, 27, 0xf7},
	{0x07ffffeb, 27, 0xf8}, {0x0ffffffe, 28, 0xf9}, {0x07ffffec, 27, 0xfa},
	{0x07ffffed, 27, 0xfb}, {0x07ffffee, 27, 0xfc}, {0x07ffffef, 27, 0xfd},
	{0x07fffff0, 27, 0xfe}, {0x03ffffee, 26, 0xff},
};

#define HUFFMAN_TABLE_SIZE (sizeof(huffman_table) / sizeof(huffman_table[0]))

/* Huffman decoder using bit-by-bit decoding */
static int hpack_decode_huffman(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
	size_t dst_pos = 0;
	uint64_t bits = 0;
	int nbits = 0;
	size_t i;

	for (i = 0; i < src_len; i++) {
		if (nbits > 56) {
			/* Bit buffer would overflow on next byte */
			return -1;
		}
		bits = (bits << 8) | src[i];
		nbits += 8;

		/* Try to decode symbols */
		while (nbits >= 5) { /* Minimum code length is 5 bits */
			int found = 0;
			int len;

			/* Try different code lengths from longest to shortest for current bits */
			for (len = (nbits > 30 ? 30 : nbits); len >= 5; len--) {
				uint32_t code = (uint32_t)((bits >> (nbits - len)) & (((uint64_t)1 << len) - 1));
				size_t j;

				/* Search for matching code in table */
				for (j = 0; j < HUFFMAN_TABLE_SIZE; j++) {
					if (huffman_table[j].nbits == (uint8_t)len && huffman_table[j].bits == code) {
						if (dst_pos >= dst_len) {
							return -1;
						}
						dst[dst_pos++] = huffman_table[j].symbol;
						nbits -= len;
						bits &= (((uint64_t)1 << nbits) - 1); /* Clear decoded bits */
						found = 1;
						break;
					}
				}

				if (found) {
					break;
				}
			}

			if (!found) {
				/* No match found - might need more bits or it's padding */
				if (i == src_len - 1) {
					/* Last byte - remaining bits should be padding (all 1s) */
					uint32_t padding_mask = (1U << nbits) - 1;
					uint32_t remaining = (uint32_t)(bits & padding_mask);
					if (remaining == padding_mask) {
						/* Valid padding */
						return dst_pos;
					}
				}
				break; /* Need more bits */
			}
		}
	}

	return dst_pos;
}

static int hpack_decode_string(const uint8_t *data, int data_len, char **str)
{
	uint64_t len;
	int huffman;
	int offset = 0;
	int ret;

	if (data_len < 1) {
		return -1;
	}

	huffman = (data[0] & 0x80) != 0;
	ret = hpack_decode_integer(data, data_len, 7, &len);
	if (ret < 0) {
		return -1;
	}
	offset += ret;

	if (offset + (int)len > data_len) {
		return -1;
	}

	if (huffman) {
		/* Huffman decoding */

		/* Allocate buffer for decoded string (worst case: same size as encoded) */
		uint8_t *decoded = malloc(len * 2 + 1); /* Extra space for safety */
		if (!decoded) {
			return -1;
		}

		int decoded_len = hpack_decode_huffman(data + offset, len, decoded, len * 2);
		if (decoded_len < 0) {
			free(decoded);
			return -1;
		}

		*str = malloc(decoded_len + 1);
		if (!*str) {
			free(decoded);
			return -1;
		}

		memcpy(*str, decoded, decoded_len);
		(*str)[decoded_len] = '\0';
		free(decoded);
	} else {
		/* Literal string */
		*str = malloc(len + 1);
		if (*str == NULL) {
			return -1;
		}

		memcpy(*str, data + offset, len);
		(*str)[len] = '\0';
	}

	offset += len;

	return offset;
}

/* HPACK dynamic table management */

void hpack_init_context(struct hpack_context *hpack)
{
	hpack->dynamic_table = NULL;
	hpack->dynamic_table_size = 0;
	hpack->max_dynamic_table_size = 65536; /* Default size */
	hpack->entry_count = 0;
}

void hpack_free_context(struct hpack_context *hpack)
{
	struct hpack_dynamic_entry *entry = hpack->dynamic_table;
	while (entry) {
		struct hpack_dynamic_entry *next = entry->next;
		free(entry->name);
		free(entry->value);
		free(entry);
		entry = next;
	}
	hpack->dynamic_table = NULL;
	hpack->dynamic_table_size = 0;
	hpack->entry_count = 0;
}

static int hpack_add_dynamic_entry(struct hpack_context *hpack, const char *name, const char *value)
{
	struct hpack_dynamic_entry *entry;
	size_t entry_size = strlen(name) + strlen(value) + 32;
	char *name_copy = NULL;
	char *value_copy = NULL;

	name_copy = strdup(name);
	value_copy = strdup(value);
	if (!name_copy || !value_copy) {
		free(name_copy);
		free(value_copy);
		return -1;
	}

	/* Evict entries if necessary */
	while (hpack->dynamic_table_size + entry_size > hpack->max_dynamic_table_size && hpack->dynamic_table) {
		struct hpack_dynamic_entry *last = hpack->dynamic_table;
		struct hpack_dynamic_entry *prev = NULL;

		while (last->next) {
			prev = last;
			last = last->next;
		}

		if (prev) {
			prev->next = NULL;
		} else {
			hpack->dynamic_table = NULL;
		}

		hpack->dynamic_table_size -= last->size;
		hpack->entry_count--;
		free(last->name);
		free(last->value);
		free(last);
	}

	if (entry_size > hpack->max_dynamic_table_size) {
		free(name_copy);
		free(value_copy);
		return 0;
	}

	entry = malloc(sizeof(*entry));
	if (!entry) {
		free(name_copy);
		free(value_copy);
		return -1;
	}

	entry->name = name_copy;
	entry->value = value_copy;
	entry->size = entry_size;
	entry->next = hpack->dynamic_table;
	hpack->dynamic_table = entry;
	hpack->dynamic_table_size += entry_size;
	hpack->entry_count++;

	return 0;
}

static int hpack_get_entry(struct hpack_context *hpack, uint64_t index, const char **name, const char **value)
{
	if (index == 0) {
		return -1;
	}

	if (index <= HPACK_STATIC_TABLE_SIZE) {
		*name = hpack_static_table[index - 1].name;
		*value = hpack_static_table[index - 1].value;
		return 0;
	}

	/* Dynamic table */
	uint64_t dynamic_index = index - HPACK_STATIC_TABLE_SIZE - 1;
	struct hpack_dynamic_entry *entry = hpack->dynamic_table;
	uint64_t i = 0;

	while (entry && i < dynamic_index) {
		entry = entry->next;
		i++;
	}

	if (!entry) {
		return -1;
	}

	*name = entry->name;
	*value = entry->value;
	return 0;
}

static int hpack_find_index(struct hpack_context *hpack, const char *name, const char *value, int *index,
							int *name_only_index)
{
	int i;

	*index = 0;
	*name_only_index = 0;

	/* Search static table */
	for (i = 0; i < (int)HPACK_STATIC_TABLE_SIZE; i++) {
		if (strcmp(hpack_static_table[i].name, name) == 0) {
			if (*name_only_index == 0) {
				*name_only_index = i + 1;
			}
			if (strcmp(hpack_static_table[i].value, value) == 0) {
				*index = i + 1;
				return 0;
			}
		}
	}

	/* Search dynamic table */
	struct hpack_dynamic_entry *entry = hpack->dynamic_table;
	i = 0;
	while (entry) {
		if (strcmp(entry->name, name) == 0) {
			if (*name_only_index == 0) {
				*name_only_index = HPACK_STATIC_TABLE_SIZE + 1 + i;
			}
			if (strcmp(entry->value, value) == 0) {
				*index = HPACK_STATIC_TABLE_SIZE + 1 + i;
				return 0;
			}
		}
		entry = entry->next;
		i++;
	}

	return 0;
}

/* HPACK encoding */

int hpack_encode_header(struct hpack_context *hpack, const char *name, const char *value, uint8_t *buf, int buf_size)
{
	int index, name_only_index;
	int offset = 0;
	int ret;

	hpack_find_index(hpack, name, value, &index, &name_only_index);

	if (index > 0) {
		/* Indexed header field */
		if (buf_size < 1) {
			return -1;
		}
		buf[offset] = 0x80;
		ret = hpack_encode_integer(index, 7, buf + offset, (size_t)(buf_size - offset));
		if (ret < 0) {
			return -1;
		}
		return ret;
	}

	if (name_only_index > 0) {
		/* Literal with incremental indexing - indexed name */
		if (buf_size < 1) {
			return -1;
		}
		buf[offset] = 0x40;
		ret = hpack_encode_integer(name_only_index, 6, buf + offset, (size_t)(buf_size - offset));
		if (ret < 0) {
			return -1;
		}
		offset += ret;

		ret = hpack_encode_string(value, buf + offset, (size_t)(buf_size - offset));
		if (ret < 0) {
			return -1;
		}
		offset += ret;

		hpack_add_dynamic_entry(hpack, name, value);
		return offset;
	}

	/* Literal with incremental indexing - new name */
	if (buf_size < 1) {
		return -1;
	}
	buf[offset++] = 0x40;

	ret = hpack_encode_string(name, buf + offset, (size_t)(buf_size - offset));
	if (ret < 0) {
		return -1;
	}
	offset += ret;

	ret = hpack_encode_string(value, buf + offset, (size_t)(buf_size - offset));
	if (ret < 0) {
		return -1;
	}
	offset += ret;

	hpack_add_dynamic_entry(hpack, name, value);
	return offset;
}

/* HPACK decoding */

void hpack_resize_dynamic_table(struct hpack_context *hpack, size_t new_size)
{
	hpack->max_dynamic_table_size = new_size;

	/* Evict entries if necessary */
	while (hpack->dynamic_table_size > hpack->max_dynamic_table_size && hpack->dynamic_table) {
		struct hpack_dynamic_entry *last = hpack->dynamic_table;
		struct hpack_dynamic_entry *prev = NULL;

		while (last->next) {
			prev = last;
			last = last->next;
		}

		if (prev) {
			prev->next = NULL;
		} else {
			hpack->dynamic_table = NULL;
		}

		hpack->dynamic_table_size -= last->size;
		hpack->entry_count--;
		free(last->name);
		free(last->value);
		free(last);
	}
}

int hpack_decode_headers(struct hpack_context *hpack, const uint8_t *data, int data_len, hpack_on_header_fn on_header,
						 void *ctx)
{
	int offset = 0;
	int header_field_seen = 0;

	while (offset < data_len) {
		const char *name = NULL;
		const char *value = NULL;
		char *allocated_name = NULL;
		char *allocated_value = NULL;

		if ((data[offset] & 0x80) != 0) {
			/* Indexed header field */
			uint64_t index;
			const char *static_name, *static_value;
			int ret = hpack_decode_integer(data + offset, data_len - offset, 7, &index);
			if (ret < 0) {
				return -1;
			}
			if (hpack_get_entry(hpack, index, &static_name, &static_value) < 0) {
				return -1;
			}
			offset += ret;

			name = static_name;
			value = static_value;
			header_field_seen = 1;
		} else if ((data[offset] & 0x40) != 0) {
			/* Literal with incremental indexing */
			uint64_t index;
			int ret = hpack_decode_integer(data + offset, data_len - offset, 6, &index);
			if (ret < 0) {
				return -1;
			}
			offset += ret;

			if (index > 0) {
				const char *static_name, *static_value;
				if (hpack_get_entry(hpack, index, &static_name, &static_value) < 0) {
					return -1;
				}
				name = static_name;
				if (index > HPACK_STATIC_TABLE_SIZE) {
					allocated_name = strdup(name);
					if (!allocated_name) {
						return -1;
					}
					name = allocated_name;
				}
			} else {
				ret = hpack_decode_string(data + offset, data_len - offset, &allocated_name);
				if (ret < 0) {
					return -1;
				}
				offset += ret;
				name = allocated_name;
			}

			ret = hpack_decode_string(data + offset, data_len - offset, &allocated_value);
			if (ret < 0) {
				free(allocated_name);
				return -1;
			}
			offset += ret;
			value = allocated_value;

			if (name && value) {
				hpack_add_dynamic_entry(hpack, name, value);
			}
			header_field_seen = 1;
		} else if ((data[offset] & 0x20) != 0) {
			/* Dynamic Table Size Update */
			uint64_t new_size;
			if (header_field_seen) {
				return -1;
			}
			int ret = hpack_decode_integer(data + offset, data_len - offset, 5, &new_size);
			if (ret < 0) {
				return -1;
			}
			offset += ret;
			hpack_resize_dynamic_table(hpack, new_size);
			continue; /* Continue to next field */
		} else {
			/* Literal without indexing or never indexed */
			uint64_t index;
			int prefix = 4; /* Both types use 4-bit prefix */
			int ret = hpack_decode_integer(data + offset, data_len - offset, prefix, &index);
			if (ret < 0) {
				return -1;
			}
			offset += ret;

			if (index > 0) {
				const char *static_name, *static_value;
				if (hpack_get_entry(hpack, index, &static_name, &static_value) < 0) {
					return -1;
				}
				name = static_name;
			} else {
				ret = hpack_decode_string(data + offset, data_len - offset, &allocated_name);
				if (ret < 0) {
					return -1;
				}
				offset += ret;
				name = allocated_name;
			}

			ret = hpack_decode_string(data + offset, data_len - offset, &allocated_value);
			if (ret < 0) {
				free(allocated_name);
				return -1;
			}
			offset += ret;
			value = allocated_value;
			header_field_seen = 1;
		}

		/* Add header to stream */
		if (on_header(ctx, name, value) < 0) {
			free(allocated_name);
			free(allocated_value);
			return -1;
		}

		/* Free allocated strings if they were copied */
		if (allocated_name) {
			free(allocated_name);
		}
		if (allocated_value) {
			free(allocated_value);
		}
	}

	return 0;
}
