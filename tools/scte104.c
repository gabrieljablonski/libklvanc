/*
 * Copyright (c) 2016 Kernel Labs Inc. All Rights Reserved
 *
 * Address: Kernel Labs Inc., PO Box 745, St James, NY. 11780
 * Contact: sales@kernellabs.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <libklvanc/vanc.h>

/* Normally we don't use a global, but we know our test harness will never be
   multi-threaded, and this is a really easy way to get the results out of the
   callback for comparison */
static uint16_t vancResult[16384];
static size_t vancResultCount;
static int passCount = 0;
static int failCount = 0;

/* CALLBACKS for message notification */
static int cb_SCTE_104(void *callback_context, struct vanc_context_s *ctx, struct packet_scte_104_s *pkt)
{
	int ret = -1;

#if 0
	/* Have the library display some debug */
	printf("Asking libklvanc to dump a struct\n");
	dump_SCTE_104(ctx, pkt);
#endif

	uint16_t *words;
	uint16_t wordCount;
	ret = convert_SCTE_104_to_words(pkt, &words, &wordCount);
	if (ret != 0) {
		fprintf(stderr, "Failed to convert 104 to words: %d\n", ret);
		return -1;
	}

	memcpy(vancResult, words, wordCount * sizeof(uint16_t));
	vancResultCount = wordCount;
	free(words);

	return 0;
}

static struct vanc_callbacks_s callbacks = 
{
	.scte_104		= cb_SCTE_104,
};
/* END - CALLBACKS for message notification */

static unsigned char __0_vancentry[] = {
	0x00, 0x00, 0x03, 0xff, 0x03, 0xff, 0x02, 0x41, 0x01, 0x07, 0x01, 0x52,
	0x01, 0x08, 0x02, 0xff, 0x02, 0xff, 0x02, 0x00, 0x01, 0x51, 0x02, 0x00,
	0x02, 0x00, 0x01, 0x52, 0x02, 0x00, 0x02, 0x05, 0x02, 0x00, 0x02, 0x00,
	0x02, 0x06, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x01, 0x0e, 0x01, 0x02,
	0x01, 0x40, 0x02, 0x00, 0x02, 0x00, 0x01, 0x52, 0x02, 0x00, 0x01, 0x64,
	0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0x02, 0x90, 0x02, 0x03, 0x01, 0x01,
	0x02, 0x00, 0x01, 0x01, 0x01, 0x04, 0x02, 0x00, 0x01, 0x02, 0x02, 0x00,
	0x02, 0x00, 0x01, 0x01, 0x02, 0x09, 0x02, 0x00, 0x02, 0x03, 0x02, 0x00,
	0x01, 0x01, 0x02, 0x30, 0x01, 0x01, 0x01, 0x0b, 0x02, 0x00, 0x02, 0x12,
	0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00,
	0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x01, 0x40, 0x02, 0x00, 0x02, 0x00,
	0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x02, 0x03,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x00, 0x02, 0x00, 0x01, 0x01, 0x01, 0x08,
	0x02, 0x00, 0x01, 0x08, 0x01, 0x01, 0x01, 0x0b, 0x02, 0x05, 0x01, 0x54,
	0x02, 0x56, 0x02, 0x4e, 0x01, 0x54, 0x02, 0x00, 0x02, 0x06
};

static int test_scte_104(struct vanc_context_s *ctx, const uint8_t *buf, size_t bufSize)
{
	int numWords = bufSize / 2;
	int mismatch = 0;

	printf("\nParsing a new SCTE104 VANC packet......\n");
	uint16_t *arr = malloc(bufSize);
	if (arr == NULL)
		return -1;

	for (int i = 0; i < numWords; i++) {
		arr[i] = buf[i * 2] << 8 | buf[i * 2 + 1];
	}

	printf("Original Input\n");
	for (int i = 0; i < numWords; i++) {
		printf("%04x ", arr[i]);
	}
	printf("\n");

	int ret = vanc_packet_parse(ctx, 13, arr, numWords);

	printf("Final output\n");
	for (int i = 0; i < vancResultCount; i++) {
		printf("%04x ", vancResult[i]);
	}
	printf("\n");

	for (int i = 0; i < vancResultCount; i++) {
		if (arr[i] != vancResult[i]) {
			fprintf(stderr, "Mismatch starting at offset 0x%02x\n", i);
			mismatch = 1;
			break;
		}
	}
	
	free(arr);

	if (mismatch) {
		printf("Printing mismatched structure:\n");
		failCount++;
		ret = vanc_packet_parse(ctx, 13, vancResult, vancResultCount);
	} else {
		printf("Original and generated versions match!\n");
		passCount++;
	}

	return ret;
}

int scte104_main(int argc, char *argv[])
{
	struct vanc_context_s *ctx;
	int ret;

	if (vanc_context_create(&ctx) < 0) {
		fprintf(stderr, "Error initializing library context\n");
		exit(1);
	}
	ctx->verbose = 0;
	ctx->callbacks = &callbacks;
	printf("Library initialized.\n");

	ret = test_scte_104(ctx, __0_vancentry, sizeof(__0_vancentry));
	if (ret < 0)
		fprintf(stderr, "SCTE-104 failed to parse\n");

	vanc_context_destroy(ctx);
	printf("Library destroyed.\n");

	printf("Final result: PASS: %d/%d, Failures: %d\n",
	       passCount, passCount + failCount, failCount);
	return 0;
}